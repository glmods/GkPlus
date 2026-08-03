#include "VkDraw.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <volk.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "Core.h"
#include "Shaders.gen.inc.h"
#include "VkInternal.h"
#include "VkResources.h"

namespace gk {
namespace vulkan {
namespace {

// The push constant block, matching `Push` in src/shaders/world.slang field for field.
//
// 72 bytes of a guaranteed 128, where it used to be 120: the matrix and the lighting inputs
// moved into the per-draw GpuDrawRecord (§4.26), because a light array could not fit here and
// the block was already full. What is left is the texture stages - the material half of §2's
// design, which is what moves next - plus the three addresses and the record index.
struct PushConstants {
  uint64_t vertices;
  uint64_t draws;
  uint64_t lights;
  uint32_t record;
  uint32_t base_vertex;
  uint32_t stage_count;
  uint32_t flags;
  uint32_t stage0_texture;
  uint32_t stage0_sampler;
  uint32_t stage0_color;
  uint32_t stage0_alpha;
  uint32_t stage1_texture;
  uint32_t stage1_sampler;
  uint32_t stage1_color;
  uint32_t stage1_alpha;
};
static_assert(sizeof(PushConstants) == 72);
static_assert(offsetof(PushConstants, stage0_texture) == 40,
              "the shader reads the stages at fixed offsets, so a field inserted above them "
              "shifts every one");

constexpr uint32_t kMaxDrawsPerFrame = 8192;

bool Ready = false;
std::string Error;
DrawStats TheStats;

VkPipelineLayout Layout = VK_NULL_HANDLE;
VkFormat ColourFormat = VK_FORMAT_UNDEFINED;

// One VkPipeline per distinct fixed-function state, built on first use. Five of them on
// level01 (§4.19), and the map is walked once per draw - a linear scan over five entries would
// do, but the map costs nothing and does not have to be revisited on a level that needs more.
//
// VK_NULL_HANDLE is a legitimate value here: it means "this state was tried and would not
// build", cached so the failure costs one attempt rather than one per draw.
std::map<PipelineState, VkPipeline> Pipelines;
VkShaderModule VertexModule = VK_NULL_HANDLE;
VkShaderModule FragmentModule = VK_NULL_HANDLE;

VkImage Depth = VK_NULL_HANDLE;
VkDeviceMemory DepthMemory = VK_NULL_HANDLE;
VkImageView DepthView = VK_NULL_HANDLE;
VkFormat DepthFormat = VK_FORMAT_UNDEFINED;
bool DepthStencil = false;

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

// D24_UNORM_S8 is not universally supported and D32_SFLOAT is; every candidate is checked
// rather than assumed, because a format the device rejects fails at image creation with nothing
// useful in the message.
//
// **The stencil-carrying formats come first, and that ordering is the fix for §4.21.** This
// used to prefer D32_SFLOAT, which has no stencil aspect - so the game's two shadow-volume
// passes drew invisibly and correctly, counted nothing, and the 50%-black quad that should have
// been masked to the shadows darkened the entire screen. The depth-only formats stay in the
// list as a fallback: no device in the target range lacks both stencil formats, and a renderer
// that will not start at all is worse than one without shadows.
bool ChooseDepthFormat() {
  struct Candidate {
    VkFormat format;
    bool stencil;
  };
  const Candidate candidates[] = {{VK_FORMAT_D24_UNORM_S8_UINT, true},
                                  {VK_FORMAT_D32_SFLOAT_S8_UINT, true},
                                  {VK_FORMAT_D32_SFLOAT, false},
                                  {VK_FORMAT_D16_UNORM, false}};
  for (const Candidate &candidate : candidates) {
    VkFormatProperties properties = {};
    vkGetPhysicalDeviceFormatProperties(GetPhysicalDevice(), candidate.format, &properties);
    if (properties.optimalTilingFeatures &
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
      DepthFormat = candidate.format;
      DepthStencil = candidate.stencil;
      if (!candidate.stencil) {
        DebugWrite("gkplus: vulkan draw: no stencil-capable depth format; shadow volumes will "
                   "not be masked\n");
      }
      return true;
    }
  }
  return Fail("no usable depth format");
}

VkImageAspectFlags DepthAspect() {
  return VK_IMAGE_ASPECT_DEPTH_BIT |
         (DepthStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VkImageAspectFlags(0));
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
  // Both aspects on the one view, which is what a depth/stencil attachment takes: dynamic
  // rendering's depth and stencil attachments are allowed to name the same view, and a view
  // restricted to one aspect could only serve one of them.
  view.subresourceRange = {DepthAspect(), 0, 1, 0, 1};
  if (vkCreateImageView(GetDevice(), &view, nullptr, &DepthView) != VK_SUCCESS) {
    return Fail("could not create the depth image view");
  }
  return true;
}

// --- D3D fixed-function state -> Vulkan ---------------------------------------------------
//
// D3DCMPFUNC and VkCompareOp enumerate the same eight comparisons in the same order, one apart:
// D3DCMP_NEVER is 1 and VK_COMPARE_OP_NEVER is 0. Written as the subtraction it is, with the
// range checked, rather than as an eight-case switch that would only restate it.
VkCompareOp ToCompareOp(uint32_t d3d) {
  if (d3d < 1 || d3d > 8) {
    return VK_COMPARE_OP_LESS_OR_EQUAL;
  }
  return static_cast<VkCompareOp>(d3d - 1);
}

VkBlendFactor ToBlendFactor(uint32_t d3d) {
  switch (d3d) {
  case 1:  return VK_BLEND_FACTOR_ZERO;                     // D3DBLEND_ZERO
  case 2:  return VK_BLEND_FACTOR_ONE;                      // ONE
  case 3:  return VK_BLEND_FACTOR_SRC_COLOR;                // SRCCOLOR
  case 4:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;      // INVSRCCOLOR
  case 5:  return VK_BLEND_FACTOR_SRC_ALPHA;                // SRCALPHA
  case 6:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;      // INVSRCALPHA
  case 7:  return VK_BLEND_FACTOR_DST_ALPHA;                // DESTALPHA
  case 8:  return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;      // INVDESTALPHA
  case 9:  return VK_BLEND_FACTOR_DST_COLOR;                // DESTCOLOR
  case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;      // INVDESTCOLOR
  case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;       // SRCALPHASAT
  default: return VK_BLEND_FACTOR_ONE;
  }
}

// D3DPRIMITIVETYPE -> VkPrimitiveTopology. The two enumerate the same six in the same order,
// one apart, but they are written out rather than subtracted: unlike the compare functions,
// nothing in either specification promises that, and a silently wrong topology draws geometry
// that is plausible and wrong.
VkPrimitiveTopology ToTopology(uint32_t d3d) {
  switch (d3d) {
  case 1: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
  case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
  case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
  case 4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
  default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  }
}

// D3DSTENCILOP -> VkStencilOp. Written out because the two enumerations do **not** correspond:
// D3D orders the saturating pair before INVERT and the wrapping pair after it, while Vulkan
// puts INVERT between INCREMENT_AND_CLAMP/DECREMENT_AND_CLAMP and the wrapping pair in a
// different order again. Subtracting a constant, as ToCompareOp legitimately does, would turn
// INCRSAT into INVERT here.
VkStencilOp ToStencilOp(uint32_t d3d) {
  switch (d3d) {
  case 1: return VK_STENCIL_OP_KEEP;                   // D3DSTENCILOP_KEEP
  case 2: return VK_STENCIL_OP_ZERO;                   // ZERO
  case 3: return VK_STENCIL_OP_REPLACE;                // REPLACE
  case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;    // INCRSAT - saturates
  case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;    // DECRSAT
  case 6: return VK_STENCIL_OP_INVERT;                 // INVERT
  case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;     // INCR - wraps
  case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;     // DECR
  default: return VK_STENCIL_OP_KEEP;
  }
}

VkPipeline CreatePipelineFor(const PipelineState &state) {
  // The entry point names are the Slang ones, kept verbatim by -fvk-use-entrypoint-name rather
  // than being rewritten to "main". Worth the flag: two modules both called "main" is exactly
  // the sort of thing that goes unnoticed until the wrong stage is bound.
  const VkPipelineShaderStageCreateInfo stages[] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_VERTEX_BIT, VertexModule, "vertex_main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_FRAGMENT_BIT, FragmentModule, "fragment_main", nullptr},
  };

  // No vertex input state at all: the vertex shader pulls from the arena by address. This is
  // the whole reason a draw binds nothing.
  VkPipelineVertexInputStateCreateInfo vertex_input = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

  VkPipelineInputAssemblyStateCreateInfo assembly = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = ToTopology(state.topology);

  VkPipelineViewportStateCreateInfo viewport = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  // D3DCULL_CCW - cull counter-clockwise faces - is D3D's default and what the game uses for
  // world geometry. CLOCKWISE as the front face for that case is measured rather than reasoned:
  // the projection matrix negates Y for Vulkan's clip space, so the intuition is that winding
  // reverses and COUNTER_CLOCKWISE is right. It is not - that setting culls the ground and
  // most of the level. Two conventions and one A/B settled it; the reasoning would have had to
  // account for D3D's left-handed clip space as well as the flip, and got only one of the two.
  // D3DCULL_CW is therefore the same rule with the front face the other way round, and
  // D3DCULL_NONE culls nothing - 1,776 draws a session, the effect sprites (§4.19).
  raster.cullMode =
      state.cull_mode == 1 /* D3DCULL_NONE */ ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
  raster.frontFace = state.cull_mode == 2 /* D3DCULL_CW */ ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                           : VK_FRONT_FACE_CLOCKWISE;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depth = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = state.depth_test != 0 ? VK_TRUE : VK_FALSE;
  depth.depthWriteEnable = state.depth_write != 0 ? VK_TRUE : VK_FALSE;
  depth.depthCompareOp = ToCompareOp(state.depth_func);
  // D3D8 has one stencil state, not the two-sided pair D3D9 added, so both faces take it -
  // which matters here rather than being a formality: the shadow-volume passes draw with
  // D3DCULL_NONE, so a back face is genuinely rasterised and must count the same way.
  depth.stencilTestEnable =
      (state.stencil_enable != 0 && DepthStencil) ? VK_TRUE : VK_FALSE;
  depth.front.compareOp = ToCompareOp(state.stencil_func);
  depth.front.failOp = ToStencilOp(state.stencil_fail);
  depth.front.depthFailOp = ToStencilOp(state.stencil_zfail);
  depth.front.passOp = ToStencilOp(state.stencil_pass);
  // The reference and the two masks are dynamic, so they are left at zero here and set per
  // draw. A pipeline built with them baked in would multiply by every reference value the
  // game uses.
  depth.back = depth.front;

  // The alpha half of the blend takes the same factors as the colour half. D3D8 has no
  // separate alpha blend - D3DRS_SEPARATEALPHABLENDENABLE arrived with D3D9 - so one pair of
  // factors is the whole state, not a simplification.
  VkPipelineColorBlendAttachmentState attachment = {};
  attachment.blendEnable = state.blend_enable != 0 ? VK_TRUE : VK_FALSE;
  attachment.srcColorBlendFactor = ToBlendFactor(state.src_blend);
  attachment.dstColorBlendFactor = ToBlendFactor(state.dest_blend);
  attachment.colorBlendOp = VK_BLEND_OP_ADD;
  attachment.srcAlphaBlendFactor = attachment.srcColorBlendFactor;
  attachment.dstAlphaBlendFactor = attachment.dstColorBlendFactor;
  attachment.alphaBlendOp = VK_BLEND_OP_ADD;
  // D3DCOLORWRITEENABLE_RED/GREEN/BLUE/ALPHA are 1/2/4/8 and VK_COLOR_COMPONENT_R/G/B/A_BIT
  // are the same four values, so the mask carries across unchanged. Written out anyway,
  // because "the same bits" is a coincidence of two specifications rather than a rule.
  attachment.colorWriteMask =
      ((state.colour_write & 1) != 0 ? VK_COLOR_COMPONENT_R_BIT : 0) |
      ((state.colour_write & 2) != 0 ? VK_COLOR_COMPONENT_G_BIT : 0) |
      ((state.colour_write & 4) != 0 ? VK_COLOR_COMPONENT_B_BIT : 0) |
      ((state.colour_write & 8) != 0 ? VK_COLOR_COMPONENT_A_BIT : 0);
  VkPipelineColorBlendStateCreateInfo blend = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &attachment;

  // Every pipeline declares the three stencil states dynamic whether or not it tests, so
  // RecordDraws can set them once per change rather than reasoning about which pipeline is
  // bound. They are ignored by a pipeline with the test off.
  const VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,            VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_STENCIL_REFERENCE,   VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
      VK_DYNAMIC_STATE_STENCIL_WRITE_MASK};
  VkPipelineDynamicStateCreateInfo dynamic = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = static_cast<uint32_t>(std::size(dynamic_states));
  dynamic.pDynamicStates = dynamic_states;

  // Dynamic rendering, so there is no VkRenderPass and no framebuffer to keep in step with the
  // swapchain - the same reason VkRenderer uses it for the overlay.
  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &ColourFormat;
  rendering.depthAttachmentFormat = DepthFormat;
  rendering.stencilAttachmentFormat = DepthStencil ? DepthFormat : VK_FORMAT_UNDEFINED;

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

  VkPipeline pipeline = VK_NULL_HANDLE;
  if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
      VK_SUCCESS) {
    ++TheStats.pipeline_failures;
    Fail("could not create a world pipeline");
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

// The pipeline for this draw's state, built on first sight of it. A cached VK_NULL_HANDLE is a
// state that would not build, so a failure costs one attempt rather than one per draw.
VkPipeline PipelineFor(const PipelineState &state) {
  const auto found = Pipelines.find(state);
  if (found != Pipelines.end()) {
    return found->second;
  }
  const VkPipeline pipeline = CreatePipelineFor(state);
  Pipelines.emplace(state, pipeline);
  TheStats.pipelines = Pipelines.size();
  return pipeline;
}

// The shader modules outlive every pipeline, unlike the old single-pipeline version which
// destroyed them at the end of creation - a pipeline built for a state first seen mid-level
// needs them just as much as the first one did.
bool CreatePipelineLayout() {
  VertexModule = CreateModule(kVertexMainSpv, sizeof(kVertexMainSpv));
  FragmentModule = CreateModule(kFragmentMainSpv, sizeof(kFragmentMainSpv));
  if (VertexModule == VK_NULL_HANDLE || FragmentModule == VK_NULL_HANDLE) {
    return Fail("could not create the shader modules");
  }

  auto set_layout = reinterpret_cast<VkDescriptorSetLayout>(BindlessDescriptorSetLayout());
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
  return true;
}

} // namespace

static_assert(std::has_unique_object_representations_v<PipelineState>,
              "the ordering is a memcmp, so a padding byte would make two equal states compare "
              "unequal and build a second pipeline for each one");

bool PipelineState::operator<(const PipelineState &other) const {
  return std::memcmp(this, &other, sizeof(PipelineState)) < 0;
}

bool StartDraw(uint32_t width, uint32_t height, uint32_t colour_format) {
  if (Ready) {
    return true;
  }
  ColourFormat = static_cast<VkFormat>(colour_format);
  if (!ChooseDepthFormat() || !CreateDepth(width, height) || !CreatePipelineLayout()) {
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
  // Read here rather than per draw, and read *before* RotateFrameScratch: both carry the slice
  // the scene now being recorded wrote into, so they are only valid inside this call.
  const uint64_t draw_records = ScratchDrawAddress();
  const uint64_t lights = ScratchLightAddress();
  if (arena_vertices == 0 || arena_indices == VK_NULL_HANDLE || draw_records == 0) {
    Items.clear();
    return;
  }

  if (set != VK_NULL_HANDLE) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, Layout, 0, 1, &set, 0,
                            nullptr);
  }

  // The index binding is tracked rather than reissued per draw: buffered geometry all shares
  // the one arena, so in practice this changes only where the list crosses from arena draws to
  // user-pointer ones and back.
  VkBuffer bound_indices = VK_NULL_HANDLE;
  VkIndexType bound_type = VK_INDEX_TYPE_UINT16;
  // The list is recorded in the order the game issued it, which is what makes blending come
  // out right without any sorting here: RenderQueue_Flush has already state-sorted the opaque
  // draws and put the back-to-front list last (rendering_notes.md §4). Sorting by pipeline to
  // save binds would undo that.
  VkPipeline bound_pipeline = VK_NULL_HANDLE;
  // The dynamic stencil state, tracked so it is issued on change rather than per draw. Started
  // at values the game cannot ask for, so the first stencil draw of the frame always sets all
  // three: the command buffer is fresh, so nothing carries over from the previous frame.
  uint32_t set_ref = UINT32_MAX, set_mask = 0, set_write_mask = 0;
  bool stencil_state_set = false;

  for (const DrawItem &item : Items) {
    const VkPipeline pipeline = PipelineFor(item.pipeline);
    if (pipeline == VK_NULL_HANDLE) {
      continue;
    }
    if (pipeline != bound_pipeline) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      bound_pipeline = pipeline;
      ++TheStats.pipeline_binds;
    }

    if (item.pipeline.stencil_enable != 0) {
      ++TheStats.stencil_draws;
      if (!DepthStencil) {
        ++TheStats.stencil_draws_without_buffer;
      }
    }
    // Set for every draw, not only the stencil-testing ones: a pipeline declares these dynamic
    // unconditionally, and a draw that never set them at all would read whatever the last one
    // left behind if a later pipeline does test.
    if (!stencil_state_set || item.stencil_ref != set_ref) {
      vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, item.stencil_ref);
      set_ref = item.stencil_ref;
    }
    if (!stencil_state_set || item.stencil_mask != set_mask) {
      vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, item.stencil_mask);
      set_mask = item.stencil_mask;
    }
    if (!stencil_state_set || item.stencil_write_mask != set_write_mask) {
      vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, item.stencil_write_mask);
      set_write_mask = item.stencil_write_mask;
    }
    stencil_state_set = true;

    PushConstants push = {};
    push.vertices =
        item.vertex_source == DrawSource::Scratch ? scratch_vertices : arena_vertices;
    push.draws = draw_records;
    push.lights = lights;
    push.record = item.record;
    push.base_vertex = item.base_vertex;
    push.stage_count = item.stage_count;
    push.flags = item.flags;
    push.stage0_texture = item.stages[0].texture_index;
    push.stage0_sampler = item.stages[0].sampler_index;
    push.stage0_color = item.stages[0].color;
    push.stage0_alpha = item.stages[0].alpha;
    push.stage1_texture = item.stages[1].texture_index;
    push.stage1_sampler = item.stages[1].sampler_index;
    push.stage1_color = item.stages[1].color;
    push.stage1_alpha = item.stages[1].alpha;
    if (push.vertices == 0) {
      continue;
    }
    vkCmdPushConstants(cmd, Layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);

    if (item.indexed) {
      VkBuffer want =
          item.index_source == DrawSource::Scratch ? scratch_indices : arena_indices;
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

bool DepthHasStencil() { return DepthStencil; }

uint32_t DepthFormatValue() { return static_cast<uint32_t>(DepthFormat); }

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
      "%llu unconvertible, %llu scratch full, %llu no draw record\n",
      (unsigned long long)TheStats.skipped_topology,
      (unsigned long long)TheStats.skipped_no_slot,
      (unsigned long long)TheStats.skipped_no_transform,
      (unsigned long long)TheStats.skipped_unconvertible,
      (unsigned long long)TheStats.skipped_scratch_full,
      (unsigned long long)TheStats.skipped_no_record);
  add("  no slot breaks down as: %llu foreign stream, %llu unslotted vertices, "
      "%llu unslotted indices, %llu 32-bit index buffer\n",
      (unsigned long long)TheStats.skipped_foreign_stream,
      (unsigned long long)TheStats.skipped_unslotted_vertices,
      (unsigned long long)TheStats.skipped_unslotted_indices,
      (unsigned long long)TheStats.skipped_index_stride);
  add("    ... of the unslotted vertices, %llu were never unlocked either\n",
      (unsigned long long)TheStats.skipped_never_unlocked);
  add("lit draws: %llu (%llu with a light switched on, %llu want COLOR2, %llu before any "
      "material, %llu lights dropped - must be 0)\n",
      (unsigned long long)TheStats.lit_draws,
      (unsigned long long)TheStats.lit_draws_with_lights,
      (unsigned long long)TheStats.lit_draws_wanting_colour2,
      (unsigned long long)TheStats.lit_draws_without_material,
      (unsigned long long)TheStats.dropped_lights);
  add("buffers seeded from their own contents: %llu (%llu refused by pool, %llu read failures)\n",
      (unsigned long long)TheStats.buffers_seeded,
      (unsigned long long)TheStats.seed_refused_pool,
      (unsigned long long)TheStats.seed_read_failures);
  add("depth format: %u (%s)   stencil draws: %llu (%llu with no stencil buffer - must be 0)\n",
      (unsigned)DepthFormat, DepthStencil ? "with stencil" : "DEPTH ONLY",
      (unsigned long long)TheStats.stencil_draws,
      (unsigned long long)TheStats.stencil_draws_without_buffer);
  add("index binds: %llu   pipelines: %llu (%llu binds, %llu failures - must be 0)\n",
      (unsigned long long)TheStats.index_binds, (unsigned long long)TheStats.pipelines,
      (unsigned long long)TheStats.pipeline_binds,
      (unsigned long long)TheStats.pipeline_failures);
  add("stages: %llu draws name an unimplemented op, %llu need more than two, "
      "%llu bound textures unresolved (must be 0)\n",
      (unsigned long long)TheStats.unsupported_stage_op,
      (unsigned long long)TheStats.truncated_stages,
      (unsigned long long)TheStats.stage_texture_unresolved);
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
  for (const auto &[state, pipeline] : Pipelines) {
    if (pipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(GetDevice(), pipeline, nullptr);
    }
  }
  Pipelines.clear();
  for (VkShaderModule *module : {&VertexModule, &FragmentModule}) {
    if (*module != VK_NULL_HANDLE) {
      vkDestroyShaderModule(GetDevice(), *module, nullptr);
      *module = VK_NULL_HANDLE;
    }
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
