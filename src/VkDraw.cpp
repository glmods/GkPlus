#include "VkDraw.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <volk.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "Core.h"
#include "Shaders.gen.inc.h"
#include "VkInternal.h"
#include "VkResources.h"

namespace gk {
namespace vulkan {
namespace {

// The push constant block, matching src/shaders/world.vert byte for byte. 88 bytes against a
// guaranteed 128, so there is room for the material fields the übershader will want without
// this becoming a buffer.
struct PushConstants {
  float mvp[16];
  uint64_t vertices;
  uint32_t base_vertex;
  uint32_t texture_index;
  uint32_t sampler_index;
  uint32_t flags;
};
static_assert(sizeof(PushConstants) == 88);

constexpr uint32_t kMaxDrawsPerFrame = 8192;

bool Ready = false;
std::string Error;
DrawStats TheStats;

VkPipelineLayout Layout = VK_NULL_HANDLE;
VkPipeline Pipeline = VK_NULL_HANDLE;
VkFormat ColourFormat = VK_FORMAT_UNDEFINED;

VkImage Depth = VK_NULL_HANDLE;
VkDeviceMemory DepthMemory = VK_NULL_HANDLE;
VkImageView DepthView = VK_NULL_HANDLE;
VkFormat DepthFormat = VK_FORMAT_UNDEFINED;

std::vector<DrawItem> Items;

bool Fail(const std::string &message) {
  Error = message;
  DebugWrite("gkplus: vulkan draw: " + message + "\n");
  return false;
}

VkShaderModule CreateModule(const uint32_t *code, size_t bytes) {
  VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = bytes;
  info.pCode = code;
  VkShaderModule module = VK_NULL_HANDLE;
  if (vkCreateShaderModule(GetDevice(), &info, nullptr, &module) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return module;
}

// D24_UNORM_S8 is not universally supported and D32_SFLOAT is; both are checked rather than
// assumed, because a format the device rejects fails at image creation with nothing useful in
// the message.
bool ChooseDepthFormat() {
  const VkFormat candidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT,
                                 VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D16_UNORM};
  for (const VkFormat format : candidates) {
    VkFormatProperties properties = {};
    vkGetPhysicalDeviceFormatProperties(GetPhysicalDevice(), format, &properties);
    if (properties.optimalTilingFeatures &
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
      DepthFormat = format;
      return true;
    }
  }
  return Fail("no usable depth format");
}

void DestroyDepth() {
  if (DepthView != VK_NULL_HANDLE) {
    vkDestroyImageView(GetDevice(), DepthView, nullptr);
    DepthView = VK_NULL_HANDLE;
  }
  if (Depth != VK_NULL_HANDLE) {
    vkDestroyImage(GetDevice(), Depth, nullptr);
    Depth = VK_NULL_HANDLE;
  }
  if (DepthMemory != VK_NULL_HANDLE) {
    vkFreeMemory(GetDevice(), DepthMemory, nullptr);
    DepthMemory = VK_NULL_HANDLE;
  }
}

// Allocated directly rather than through VMA: this is one image that lives as long as the
// swapchain, and pulling the allocator in from VkResources.cpp for it would export a handle
// that nothing else needs.
bool CreateDepth(uint32_t width, uint32_t height) {
  DestroyDepth();
  if (width == 0 || height == 0) {
    return false;
  }

  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = DepthFormat;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(GetDevice(), &info, nullptr, &Depth) != VK_SUCCESS) {
    return Fail("could not create the depth image");
  }

  VkMemoryRequirements requirements = {};
  vkGetImageMemoryRequirements(GetDevice(), Depth, &requirements);
  VkPhysicalDeviceMemoryProperties memory = {};
  vkGetPhysicalDeviceMemoryProperties(GetPhysicalDevice(), &memory);
  uint32_t type = UINT32_MAX;
  for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    if ((requirements.memoryTypeBits & (1u << i)) != 0 &&
        (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
      type = i;
      break;
    }
  }
  if (type == UINT32_MAX) {
    return Fail("no device-local memory type for the depth image");
  }

  VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = type;
  if (vkAllocateMemory(GetDevice(), &allocate, nullptr, &DepthMemory) != VK_SUCCESS ||
      vkBindImageMemory(GetDevice(), Depth, DepthMemory, 0) != VK_SUCCESS) {
    return Fail("could not back the depth image");
  }

  VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = Depth;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = DepthFormat;
  view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(GetDevice(), &view, nullptr, &DepthView) != VK_SUCCESS) {
    return Fail("could not create the depth image view");
  }
  return true;
}

bool CreatePipeline() {
  VkShaderModule vertex = CreateModule(kVertexMainSpv, sizeof(kVertexMainSpv));
  VkShaderModule fragment = CreateModule(kFragmentMainSpv, sizeof(kFragmentMainSpv));
  if (vertex == VK_NULL_HANDLE || fragment == VK_NULL_HANDLE) {
    return Fail("could not create the shader modules");
  }

  auto set_layout =
      reinterpret_cast<VkDescriptorSetLayout>(BindlessDescriptorSetLayout());
  VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(PushConstants)};
  VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = set_layout != VK_NULL_HANDLE ? 1 : 0;
  layout.pSetLayouts = &set_layout;
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &range;
  if (vkCreatePipelineLayout(GetDevice(), &layout, nullptr, &Layout) != VK_SUCCESS) {
    return Fail("could not create the pipeline layout");
  }

  // The entry point names are the Slang ones, kept verbatim by -fvk-use-entrypoint-name rather
  // than being rewritten to "main". Worth the flag: two modules both called "main" is exactly
  // the sort of thing that goes unnoticed until the wrong stage is bound.
  const VkPipelineShaderStageCreateInfo stages[] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_VERTEX_BIT, vertex, "vertex_main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "fragment_main", nullptr},
  };

  // No vertex input state at all: the vertex shader pulls from the arena by address. This is
  // the whole reason a draw binds nothing.
  VkPipelineVertexInputStateCreateInfo vertex_input = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

  VkPipelineInputAssemblyStateCreateInfo assembly = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewport = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  // D3D's default is D3DCULL_CCW - cull counter-clockwise faces - and the game leaves it there
  // for world geometry. CLOCKWISE as the front face, which is measured rather than reasoned:
  // the projection matrix negates Y for Vulkan's clip space, so the intuition is that winding
  // reverses and COUNTER_CLOCKWISE is right. It is not - that setting culls the ground and
  // most of the level. Two conventions and one A/B settled it; the reasoning would have had to
  // account for D3D's left-handed clip space as well as the flip, and got only one of the two.
  raster.cullMode = VK_CULL_MODE_BACK_BIT;
  raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depth = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = VK_TRUE;
  depth.depthWriteEnable = VK_TRUE;
  depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

  VkPipelineColorBlendAttachmentState attachment = {};
  attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &attachment;

  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                           VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;

  // Dynamic rendering, so there is no VkRenderPass and no framebuffer to keep in step with the
  // swapchain - the same reason VkRenderer uses it for the overlay.
  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &ColourFormat;
  rendering.depthAttachmentFormat = DepthFormat;

  VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = Layout;

  const VkResult result =
      vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr, &Pipeline);
  vkDestroyShaderModule(GetDevice(), vertex, nullptr);
  vkDestroyShaderModule(GetDevice(), fragment, nullptr);
  if (result != VK_SUCCESS) {
    return Fail("could not create the world pipeline");
  }
  return true;
}

} // namespace

bool StartDraw(uint32_t width, uint32_t height, uint32_t colour_format) {
  if (Ready) {
    return true;
  }
  ColourFormat = static_cast<VkFormat>(colour_format);
  if (!ChooseDepthFormat() || !CreateDepth(width, height) || !CreatePipeline()) {
    return false;
  }
  Items.reserve(kMaxDrawsPerFrame);
  Ready = true;
  TheStats.ready = true;
  Error.clear();
  DebugWrite("gkplus: vulkan world pipeline up\n");
  return true;
}

bool DrawReady() { return Ready; }

bool ResizeDraw(uint32_t width, uint32_t height) {
  return Ready ? CreateDepth(width, height) : false;
}

void SubmitDraw(const DrawItem &item) {
  if (Items.size() >= kMaxDrawsPerFrame) {
    ++TheStats.dropped_over_capacity;
    return;
  }
  Items.push_back(item);
}

void ClearDraws() { Items.clear(); }

void RecordDraws(void *command_buffer) {
  TheStats.items = Items.size();
  if (Items.size() > TheStats.max_items) {
    TheStats.max_items = Items.size();
  }
  if (!Ready || Items.empty()) {
    Items.clear();
    return;
  }
  auto cmd = static_cast<VkCommandBuffer>(command_buffer);
  auto set = reinterpret_cast<VkDescriptorSet>(BindlessDescriptorSet());
  const uint64_t arena_vertices = VertexArenaAddress();
  const uint64_t scratch_vertices = ScratchVertexAddress();
  auto arena_indices = reinterpret_cast<VkBuffer>(IndexArenaBuffer());
  auto scratch_indices = reinterpret_cast<VkBuffer>(ScratchIndexBuffer());
  if (arena_vertices == 0 || arena_indices == VK_NULL_HANDLE) {
    Items.clear();
    return;
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, Pipeline);
  if (set != VK_NULL_HANDLE) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, Layout, 0, 1, &set, 0,
                            nullptr);
  }

  // The index binding is tracked rather than reissued per draw: buffered geometry all shares
  // the one arena, so in practice this changes only where the list crosses from arena draws to
  // user-pointer ones and back.
  VkBuffer bound_indices = VK_NULL_HANDLE;
  VkIndexType bound_type = VK_INDEX_TYPE_UINT16;

  for (const DrawItem &item : Items) {
    PushConstants push = {};
    std::memcpy(push.mvp, item.mvp, sizeof(push.mvp));
    push.vertices =
        item.source == DrawSource::Scratch ? scratch_vertices : arena_vertices;
    push.base_vertex = item.base_vertex;
    push.texture_index = item.texture_index;
    push.sampler_index = item.sampler_index;
    push.flags = item.flags;
    if (push.vertices == 0) {
      continue;
    }
    vkCmdPushConstants(cmd, Layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);

    if (item.indexed) {
      VkBuffer want =
          item.source == DrawSource::Scratch ? scratch_indices : arena_indices;
      const VkIndexType type =
          item.index_stride == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
      if (want == VK_NULL_HANDLE) {
        continue;
      }
      if (want != bound_indices || type != bound_type) {
        // Offset 0 always: both index sources make their first-index absolute from the start
        // of their buffer, which is what keeps this to one bind rather than one per draw.
        vkCmdBindIndexBuffer(cmd, want, 0, type);
        bound_indices = want;
        bound_type = type;
        ++TheStats.index_binds;
      }
      vkCmdDrawIndexed(cmd, item.count, 1, item.first_index, item.vertex_offset, 0);
    } else {
      // DrawPrimitiveUP has no indices, and vertex pulling means it needs none: gl_VertexIndex
      // counts up from 0 and the shader adds `base_vertex` itself.
      vkCmdDraw(cmd, item.count, 1, 0, 0);
    }
    ++TheStats.drawn;
  }
  Items.clear();
}

uint64_t DepthImageView() { return reinterpret_cast<uint64_t>(DepthView); }

uint64_t DepthImage() { return reinterpret_cast<uint64_t>(Depth); }

const DrawStats &Draws() { return TheStats; }

std::string FormatDrawStats() {
  std::string out;
  char line[256];
  auto add = [&](const char *fmt, auto... args) {
    std::snprintf(line, sizeof(line), fmt, args...);
    out += line;
  };
  add("world pipeline: %s\n", Ready ? "up" : "down");
  if (!Error.empty()) {
    out += "error: " + Error + "\n";
  }
  add("draws: %llu this frame / %llu peak / %llu total\n",
      (unsigned long long)TheStats.items, (unsigned long long)TheStats.max_items,
      (unsigned long long)TheStats.drawn);
  add("skipped: %llu topology, %llu no arena slot, %llu no transform, "
      "%llu unconvertible, %llu scratch full\n",
      (unsigned long long)TheStats.skipped_topology,
      (unsigned long long)TheStats.skipped_no_slot,
      (unsigned long long)TheStats.skipped_no_transform,
      (unsigned long long)TheStats.skipped_unconvertible,
      (unsigned long long)TheStats.skipped_scratch_full);
  add("index binds: %llu\n", (unsigned long long)TheStats.index_binds);
  add("dropped over capacity: %llu (must be 0)\n",
      (unsigned long long)TheStats.dropped_over_capacity);
  return out;
}

void ShutdownDraw() {
  if (!Ready) {
    return;
  }
  vkDeviceWaitIdle(GetDevice());
  DestroyDepth();
  if (Pipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(GetDevice(), Pipeline, nullptr);
    Pipeline = VK_NULL_HANDLE;
  }
  if (Layout != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(GetDevice(), Layout, nullptr);
    Layout = VK_NULL_HANDLE;
  }
  Items.clear();
  Ready = false;
  TheStats.ready = false;
}

DrawStats &MutableDrawStats() { return TheStats; }

} // namespace vulkan
} // namespace gk
