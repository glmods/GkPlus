#include "VkDraw.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <volk.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "Core.h"
#include "Shaders.gen.inc.h"
#include "VkContext.h"
#include "VkInternal.h"
#include "VertexFormat.h"
#include "VkResources.h"

namespace gk {
namespace vulkan {
namespace {

// The push constant block, matching `Push` in src/shaders/world.slang field for field.
//
// **48 bytes of a guaranteed 128**, where it was 120 before the draw record (§4.26) and 72
// before the material table (§4.30). Everything per-draw is now an index into an array reached
// by address, which is §2's design arrived at in full: four addresses, three indices, nothing
// that describes a draw. What is left cannot shrink further without giving up the addresses,
// and there is no reason to.
//
// `base_vertex` is the one index that is not into one of those arrays - it is where this draw's
// buffer starts inside the vertex arena, and it stays here because the vertex shader adds it to
// SV_VertexID before it has read anything at all.
//
// 44 bytes of content and 48 of struct: the four addresses give it 8-byte alignment, so the
// compiler pads the tail. The pad is spelled out rather than left implicit because the range
// declared in the pipeline layout is this `sizeof`, and a reader comparing it against the Slang
// side - which stops at 44 - should not have to work out where the extra four bytes came from.
struct PushConstants {
  uint64_t vertices;
  uint64_t draws;
  uint64_t lights;
  uint64_t materials;
  uint32_t record;
  uint32_t material;
  uint32_t base_vertex;
  // The LOD probe (§4.34). Negative means "sample normally"; anything else makes every texture
  // fetch an explicit-LOD one at that level. It occupies what was tail padding, so it is free -
  // see the note above about why the pad exists at all.
  float force_lod;
};
static_assert(sizeof(PushConstants) == 48);

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
// The swapchain's extent, kept because a draw's viewport has to be reissued when its depth
// slice changes and only the width and height stay constant across that (§4.32).
uint32_t ViewportWidth = 0;
uint32_t ViewportHeight = 0;

std::vector<DrawItem> Items;
// The list as it was last recorded, so a draw can be described after the frame it belongs to.
// RecordDraws swaps into it rather than copying.
std::vector<DrawItem> LastItems;

// The bisect windows, inclusive. See SetDrawRange and SetDrawHide in VkDraw.h.
uint32_t DrawRangeFirst = 0;
uint32_t DrawRangeLast = UINT32_MAX;
// Empty by default, expressed as a window that cannot contain an index.
uint32_t DrawHideFirst = 1;
uint32_t DrawHideLast = 0;

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
  ViewportWidth = width;
  ViewportHeight = height;

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
  // See PipelineState::depth_clamp. Requested at device creation and near-universal on desktop,
  // but not required: without it a pre-transformed draw whose z leaves its slice is clipped
  // where D3D would have clamped it, which is the pre-§4.45 behaviour for that draw alone.
  raster.depthClampEnable =
      (state.depth_clamp != 0 && Caps().depth_clamp) ? VK_TRUE : VK_FALSE;

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

// Byte-comparable for the same reason PipelineState is: every member is a uint32_t, so there is
// no padding the compiler chose and memcmp cannot call two equal materials unequal. That holds
// for `tint` too, which is why the override can key on a material rather than on a draw: two
// draws of the same surface with the same tint intern to one entry, as they did before it
// existed.
bool GpuMaterial::operator<(const GpuMaterial &other) const {
  return std::memcmp(this, &other, sizeof(GpuMaterial)) < 0;
}

namespace {

// The frame's material table, and the intern map that deduplicates into it. Both live exactly
// as long as `Items` does - a scene's worth - because the table is in the scratch slice that
// scene writes, and RotateFrameScratch takes that slice away at the bottom of the frame.
//
// The map is rebuilt every frame rather than persisted, which is not a missed optimisation: a
// bindless index is only valid against the slice it was allocated from, so a material carried
// over from the previous scene would name an entry the shader can no longer see.
std::map<GpuMaterial, uint32_t> InternedMaterials;

// See SetShadeMode in VkDraw.h. Default on: it is a state the game sets, not a feature.
bool ShadeModeEnabled = true;

// See SetForceLod in VkDraw.h. Off by default and not a state the game has - purely the probe.
float ForcedLod = -1.0f;

// See WatchDrawVertices in VkDraw.h. Snapshotted at submit time into a fixed buffer, so reading
// it never races the scratch's rotation.
constexpr uint32_t kWatchVertices = 16;
constexpr uint32_t kWatchIndices = 24;
uint32_t WatchIndex = UINT32_MAX;
bool WatchValid = false;
DrawItem WatchItem;
CanonicalVertex WatchVertexData[kWatchVertices];
uint32_t WatchVertexCount = 0;
uint32_t WatchIndexData[kWatchIndices];
uint32_t WatchIndexCount = 0;

// Copies the head of a watched draw's geometry out of the scratch, while the slice it was
// written into is still the current one.
void SnapshotWatched(const DrawItem &item) {
  WatchValid = false;
  WatchVertexCount = 0;
  WatchIndexCount = 0;
  if (item.vertex_source != DrawSource::Scratch) {
    // Recorded anyway, so the reader is told "this draw is not a user-pointer draw" rather than
    // being given an empty answer that looks like "nothing was submitted".
    WatchItem = item;
    WatchValid = true;
    return;
  }
  const auto *vertices = static_cast<const CanonicalVertex *>(ScratchVertexMapped());
  if (vertices == nullptr) {
    return;
  }
  WatchItem = item;
  WatchVertexCount = (std::min)(kWatchVertices, item.count);
  std::memcpy(WatchVertexData, vertices + item.base_vertex,
              sizeof(CanonicalVertex) * WatchVertexCount);

  if (item.indexed && item.index_source == DrawSource::Scratch) {
    const auto *base = static_cast<const uint8_t *>(ScratchIndexMappedBase());
    if (base != nullptr) {
      WatchIndexCount = (std::min)(kWatchIndices, item.count);
      for (uint32_t i = 0; i < WatchIndexCount; ++i) {
        const size_t at = size_t(item.first_index + i) * item.index_stride;
        WatchIndexData[i] = item.index_stride == 4
                                ? *reinterpret_cast<const uint32_t *>(base + at)
                                : *reinterpret_cast<const uint16_t *>(base + at);
      }
      // The vertices a draw reads are the ones its INDICES name, not the first N in the slice.
      // Reading the head of the slice instead is how a wrong answer would look right: the first
      // vertex is usually fine and the degenerate ones are further in.
      WatchVertexCount = 0;
      for (uint32_t i = 0; i < WatchIndexCount && WatchVertexCount < kWatchVertices; ++i) {
        WatchVertexData[WatchVertexCount++] = vertices[item.base_vertex + WatchIndexData[i]];
      }
    }
  }
  WatchValid = true;
}

// --- the material override ----------------------------------------------------------------
//
// See SetMaterialOverride in VkDraw.h for what this is and why it is keyed by asset name. The
// table here is the registration; `Resolved` is that table projected onto bindless indices, which
// is the form the draw path can afford to consult.
struct OverrideEntry {
  std::string key;      // as given, for the readback
  std::string lowered;  // ... and folded, for matching
  MaterialOverride over;
  std::string lowered_texture;
};

// A vector rather than a map: insertion order decides which of two matching keys wins, and
// "first one registered" is the only rule a mod author can predict.
std::vector<OverrideEntry> Overrides;

struct ResolvedOverride {
  uint32_t texture = kNoTexture; // the image to sample instead, kNoTexture to keep the original
  uint32_t tint = 0xffffffffu;
  bool hide = false;
  bool any = false;
  size_t owner = 0; // which registration claimed this image, for the readback
};

// Indexed by bindless slot. Empty whenever no override is registered, which is what makes the
// whole feature free when nobody is using it.
std::vector<ResolvedOverride> Resolved;
uint64_t ResolvedGeneration = 0; // the TextureRegistryGeneration this was built against
// Live images the keys matched. NOT Resolved.size(), which is the highest matching slot plus one:
// the difference is invisible while one image matches and misleading as soon as two do.
size_t ResolvedMatches = 0;

std::string Lowered(const std::string &text) {
  std::string out = text;
  for (char &c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

uint32_t PackTint(const float (&rgba)[4]) {
  uint32_t packed = 0;
  for (int i = 0; i < 4; ++i) {
    const float clamped = rgba[i] < 0.0f ? 0.0f : (rgba[i] > 1.0f ? 1.0f : rgba[i]);
    // +0.5 and not truncation: 1.0 has to land on 255 exactly, or an identity tint would darken
    // every surface it touched by one step and read as a shading defect.
    packed |= static_cast<uint32_t>(clamped * 255.0f + 0.5f) << (8 * i);
  }
  return packed;
}

// The first image whose name contains `needle`, or kNoTexture. Substring and case-insensitive,
// which is `render.probe`'s rule (§4.35) and is what makes `"water"` a usable key against
// `bitmaps\water.rim`.
uint32_t FindImageByName(const std::vector<TextureImageInfo> &images, const std::string &needle) {
  if (needle.empty()) {
    return kNoTexture;
  }
  for (const TextureImageInfo &image : images) {
    if (Lowered(image.name).find(needle) != std::string::npos) {
      return image.index;
    }
  }
  return kNoTexture;
}

// Rebuilds `Resolved` from `Overrides` and the live image set. Called when either changes, never
// per draw and never per frame while both are unchanged - which is what TextureRegistryGeneration
// is for: an image created or named mid-level changes what a key matches, and nothing else does.
void ResolveMaterialOverrides() {
  Resolved.clear();
  ResolvedMatches = 0;
  ResolvedGeneration = TextureRegistryGeneration();
  if (Overrides.empty()) {
    return;
  }
  const std::vector<TextureImageInfo> images = TextureImages();
  for (const TextureImageInfo &image : images) {
    const std::string name = Lowered(image.name);
    if (name.empty()) {
      continue; // an image with no cache record behind it has no identity to key on
    }
    for (size_t i = 0; i < Overrides.size(); ++i) {
      const OverrideEntry &entry = Overrides[i];
      if (name.find(entry.lowered) == std::string::npos) {
        continue;
      }
      if (Resolved.size() <= image.index) {
        Resolved.resize(size_t(image.index) + 1);
      }
      ResolvedOverride &slot = Resolved[image.index];
      if (!slot.any) {
        ++ResolvedMatches;
      }
      slot.any = true;
      slot.owner = i;
      slot.tint = PackTint(entry.over.tint);
      slot.hide = entry.over.hide;
      slot.texture = FindImageByName(images, entry.lowered_texture);
      break; // first registration wins; see the note on Overrides
    }
  }
}

const ResolvedOverride *OverrideFor(uint32_t texture_index) {
  if (Resolved.empty() || texture_index >= Resolved.size() || !Resolved[texture_index].any) {
    return nullptr;
  }
  return &Resolved[texture_index];
}

// Cheap enough to call per draw: a comparison against a counter, and a rebuild only when the
// game has created, destroyed or named an image since the last one.
void EnsureOverridesResolved() {
  if (!Overrides.empty() && ResolvedGeneration != TextureRegistryGeneration()) {
    ResolveMaterialOverrides();
  }
}

// Interns one material into the frame's table and returns its index. kNoMaterial when the
// slice is full, which the caller turns into a draw with no stages rather than no draw.
constexpr uint32_t kNoMaterial = 0xffffffffu;

uint32_t InternMaterial(const DrawItem &item) {
  GpuMaterial material;
  material.stage0_texture = item.stages[0].texture_index;
  material.stage0_sampler = item.stages[0].sampler_index;
  material.stage0_color = item.stages[0].color;
  material.stage0_alpha = item.stages[0].alpha;
  material.stage1_texture = item.stages[1].texture_index;
  material.stage1_sampler = item.stages[1].sampler_index;
  material.stage1_color = item.stages[1].color;
  material.stage1_alpha = item.stages[1].alpha;
  material.stage_count = item.stage_count;
  material.flags = item.flags;
  if (item.shade_mode == kShadeFlat) {
    ++TheStats.flat_shaded_draws;
  }
  // Forced to GOURAUD rather than left alone when the toggle is off, so the whole table collapses
  // to the pre-§4.31 shape and the A/B is the feature and nothing else.
  material.shading = ShadeModeEnabled ? item.shade_mode : kShadeGouraud;
  // A stage past `stage_count` is never read, so two draws differing only there are the same
  // material - and would not be if the unused words went into the key. Zeroed rather than
  // masked out of the comparison, because the table is uploaded as it is compared.
  if (material.stage_count < 2) {
    material.stage1_texture = kNoTexture;
    material.stage1_sampler = 0;
    material.stage1_color = 0;
    material.stage1_alpha = 0;
  }
  if (material.stage_count < 1) {
    material.stage0_texture = kNoTexture;
    material.stage0_sampler = 0;
    material.stage0_color = 0;
    material.stage0_alpha = 0;
  }

  // The override, applied here because this is where a material becomes a table entry - one
  // rewrite per distinct surface rather than one per draw. Keyed on the ORIGINAL stage-0 index,
  // read before either stage is remapped, or an override naming its own replacement would key on
  // whatever it had just swapped in.
  if (!Resolved.empty()) {
    bool touched = false;
    if (const ResolvedOverride *over = OverrideFor(material.stage0_texture)) {
      material.tint = over->tint;
      touched = true;
    }
    for (uint32_t *stage : {&material.stage0_texture, &material.stage1_texture}) {
      if (const ResolvedOverride *over = OverrideFor(*stage)) {
        if (over->texture != kNoTexture) {
          *stage = over->texture;
          touched = true;
        }
      }
    }
    // Counted per draw and not per interned material, because the question it answers is "did
    // the frame draw anything with this" - a key that matches an asset the camera cannot see
    // resolves and reports its image exactly like one that is on screen, which cost most of a
    // session's confusion the first time (see the tint measured at 0.000 in §4.44).
    if (touched) {
      ++TheStats.overridden_draws;
    }
  }

  const auto found = InternedMaterials.find(material);
  if (found != InternedMaterials.end()) {
    return found->second;
  }
  const ScratchAlloc alloc = AllocateScratchMaterials(1);
  if (!alloc.valid) {
    return kNoMaterial;
  }
  *static_cast<GpuMaterial *>(alloc.mapped) = material;
  InternedMaterials.emplace(material, alloc.offset);
  return alloc.offset;
}

} // namespace

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

// See the note on SetHalfPixel in VkDraw.h. Default on: it is a convention difference between
// the two APIs, not a feature, so drawing without it is the wrong answer everywhere.
namespace {
bool HalfPixelEnabled = true;
// See the note on SetRhwDepthRaw in VkDraw.h. Default on for the same reason `half_pixel` is:
// D3D clamping a pre-transformed vertex's z instead of scaling it is a measured rule of the API
// being reproduced, not a choice. Read from the capture layer at record time, not here.
bool RhwDepthRawEnabled = true;
// See the note on SetViewportRect in VkDraw.h. Default on for the same reason, and read from the
// capture layer at record time for the same reason - the rectangle reaches both the DrawItem and
// BuildMvp's origin term from there, and a toggle read here would move only one of them.
bool ViewportRectEnabled = true;
} // namespace

void WatchDrawVertices(uint32_t index) {
  WatchIndex = index;
  WatchValid = false;
}

uint32_t WatchedDrawVertices() { return WatchIndex; }

std::string DescribeWatchedVertices() {
  if (WatchIndex == UINT32_MAX) {
    return "no draw is being watched; set render.draw_vertices = <index> and let a frame pass\n";
  }
  if (!WatchValid) {
    return "draw " + std::to_string(WatchIndex) +
           " has not been submitted since it was watched - let a frame pass, and note that a "
           "paused frame still submits\n";
  }
  std::string out;
  char line[256];
  auto add = [&](const char *fmt, auto... args) {
    std::snprintf(line, sizeof(line), fmt, args...);
    out += line;
  };
  add("draw %u: %s, %u %s from %s, base_vertex %u first_index %u\n", WatchIndex,
      WatchItem.indexed ? "indexed" : "non-indexed", WatchItem.count,
      WatchItem.indexed ? "indices" : "vertices",
      WatchItem.vertex_source == DrawSource::Scratch ? "scratch" : "arena",
      WatchItem.base_vertex, WatchItem.first_index);
  if (WatchItem.vertex_source != DrawSource::Scratch) {
    out += "  vertices are in the arena, which is never mapped - use render.verify_buffers()\n";
    return out;
  }
  if (WatchIndexCount != 0) {
    out += "  indices:";
    for (uint32_t i = 0; i < WatchIndexCount; ++i) {
      add(" %u", WatchIndexData[i]);
    }
    out += "\n";
  }
  out += "  vertices as the shader pulls them:\n";
  for (uint32_t i = 0; i < WatchVertexCount; ++i) {
    const CanonicalVertex &v = WatchVertexData[i];
    add("    %2u  pos %11.4f %11.4f %11.4f  w %9.5f  colour 0x%08x  uv %8.4f %8.4f\n", i,
        v.pos[0], v.pos[1], v.pos[2], v.pos[3], v.color, v.uv0[0], v.uv0[1]);
  }
  return out;
}

void SetForceLod(float lod) { ForcedLod = lod; }

float ForceLod() { return ForcedLod; }

void SetShadeMode(bool enabled) { ShadeModeEnabled = enabled; }

bool ShadeMode() { return ShadeModeEnabled; }

// --- the material override, from the outside --------------------------------------------------
//
// All four resolve immediately rather than marking the table dirty, so an override registered on
// a *paused* frame takes effect on the next one - which is where it will be judged, since a
// paused frame is the only comparison sharp enough to see a small change (§4.28).

void SetMaterialOverride(const std::string &name, const MaterialOverride &over) {
  const std::string lowered = Lowered(name);
  OverrideEntry entry;
  entry.key = name;
  entry.lowered = lowered;
  entry.over = over;
  entry.lowered_texture = Lowered(over.texture);
  for (OverrideEntry &existing : Overrides) {
    if (existing.lowered == lowered) {
      // Replaced in place, so re-registering a key does not move it to the back of the
      // first-match-wins order.
      existing = entry;
      ResolveMaterialOverrides();
      return;
    }
  }
  Overrides.push_back(entry);
  ResolveMaterialOverrides();
}

bool RemoveMaterialOverride(const std::string &name) {
  const std::string lowered = Lowered(name);
  for (size_t i = 0; i < Overrides.size(); ++i) {
    if (Overrides[i].lowered == lowered) {
      Overrides.erase(Overrides.begin() + i);
      ResolveMaterialOverrides();
      return true;
    }
  }
  return false;
}

void ClearMaterialOverrides() {
  Overrides.clear();
  ResolveMaterialOverrides();
}

std::string DescribeMaterialOverrides() {
  EnsureOverridesResolved();
  if (Overrides.empty()) {
    return "no material overrides registered\n"
           "  render.material_override(\"<.rim substring>\", {texture, tint, hide})\n";
  }
  std::string out;
  char line[512];
  auto add = [&](const char *fmt, auto... args) {
    std::snprintf(line, sizeof(line), fmt, args...);
    out += line;
  };
  const std::vector<TextureImageInfo> images = TextureImages();
  for (size_t index = 0; index < Overrides.size(); ++index) {
    const OverrideEntry &entry = Overrides[index];
    const uint32_t tint = PackTint(entry.over.tint);
    add("\"%s\"%s  tint %.3f %.3f %.3f %.3f%s\n", entry.key.c_str(),
        entry.over.hide ? "  HIDDEN" : "", entry.over.tint[0], entry.over.tint[1],
        entry.over.tint[2], entry.over.tint[3],
        tint == 0xffffffffu ? " (identity)" : "");
    if (!entry.over.texture.empty()) {
      const uint32_t replacement = FindImageByName(images, entry.lowered_texture);
      if (replacement == kNoTexture) {
        add("  -> texture \"%s\": NO LIVE IMAGE MATCHES - the original is drawn\n",
            entry.over.texture.c_str());
      } else {
        add("  -> texture \"%s\" = image %u\n", entry.over.texture.c_str(),
            (unsigned)replacement);
      }
    }
    // Every image the key matches, because a substring key matching nothing and one matching
    // half the level look identical from the call site.
    size_t matched = 0;
    for (const TextureImageInfo &image : images) {
      if (image.name.empty() || Lowered(image.name).find(entry.lowered) == std::string::npos) {
        continue;
      }
      const bool mine = image.index < Resolved.size() && Resolved[image.index].any &&
                        Resolved[image.index].owner == index;
      add("     image %-4u %s%s\n", (unsigned)image.index, image.name.c_str(),
          mine ? "" : "   (taken by an earlier key)");
      ++matched;
    }
    if (matched == 0) {
      out += "     MATCHES NOTHING - no live image's .rim path contains this\n";
    }
  }
  add("%llu draws overridden, %llu hidden since the last render.reset()\n",
      (unsigned long long)TheStats.overridden_draws,
      (unsigned long long)TheStats.hidden_draws);
  return out;
}

void SetHalfPixel(bool enabled) { HalfPixelEnabled = enabled; }

bool HalfPixel() { return HalfPixelEnabled; }

float ViewportOrigin() { return HalfPixelEnabled ? 0.5f : 0.0f; }

void SetRhwDepthRaw(bool enabled) { RhwDepthRawEnabled = enabled; }

bool RhwDepthRaw() { return RhwDepthRawEnabled; }

void SetViewportRect(bool enabled) { ViewportRectEnabled = enabled; }

bool ViewportRect() { return ViewportRectEnabled; }

bool ResizeDraw(uint32_t width, uint32_t height) {
  return Ready ? CreateDepth(width, height) : false;
}

uint32_t PendingDrawIndex() { return static_cast<uint32_t>(Items.size()); }

void SubmitDraw(const DrawItem &item) {
  if (Items.size() >= kMaxDrawsPerFrame) {
    ++TheStats.dropped_over_capacity;
    return;
  }
  // Before the draw joins the list, and before its material is interned: a hidden draw should
  // cost nothing and should leave no entry in a table that is read back as "what this frame
  // draws". `PendingDrawIndex` still advances the same way, because the index a diagnostic was
  // armed on is a position in the list, and skipping one shifts the rest either way.
  EnsureOverridesResolved();
  if (!Resolved.empty() && item.stage_count > 0) {
    if (const ResolvedOverride *over = OverrideFor(item.stages[0].texture_index)) {
      if (over->hide) {
        ++TheStats.hidden_draws;
        return;
      }
    }
  }
  Items.push_back(item);
  ++TheStats.submitted;
  // Interned here rather than in the capture layer, which is where the GpuDrawRecord is written:
  // a record is per draw and the capture layer is the only place that knows the matrices, but a
  // material is shared, and the table it is shared through belongs to the frame's draw list.
  // This is also the only place that sees every draw exactly once - the capture layer has three
  // entry points into a DrawItem and would have to remember to intern in all of them.
  const uint32_t material = InternMaterial(item);
  if (material == kNoMaterial) {
    --TheStats.submitted;
    // Structurally unreachable by capacity: at most one new material is interned per draw and
    // the slice holds kMaxDrawsPerFrame of them, which is the reason it is sized that way. So a
    // failure here means the scratch itself is not usable, and there is no entry to point at -
    // dropping the draw is the only option, and the counter is what says it happened.
    ++TheStats.dropped_materials;
    Items.pop_back();
    return;
  }
  Items.back().material = material;
  if (Items.size() - 1 == WatchIndex) {
    SnapshotWatched(Items.back());
  }
}

void ClearDraws() {
  Items.clear();
  InternedMaterials.clear();
}

void RecordDraws(void *command_buffer) {
  TheStats.items = Items.size();
  if (Items.size() > TheStats.max_items) {
    TheStats.max_items = Items.size();
  }
  TheStats.materials = InternedMaterials.size();
  if (InternedMaterials.size() > TheStats.max_materials) {
    TheStats.max_materials = InternedMaterials.size();
  }
  if (!Ready || Items.empty()) {
    ClearDraws();
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
  const uint64_t materials = ScratchMaterialAddress();
  if (arena_vertices == 0 || arena_indices == VK_NULL_HANDLE || draw_records == 0 ||
      materials == 0) {
    ClearDraws();
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
  // The viewport, tracked the same way. VkRenderer has already set a full-target one for the
  // frame; this reissues it whenever a draw wants a different depth slice - a handful of times a
  // frame on level03 rather than per draw - or a different rectangle, which on the upgrade screen
  // is once (§4.47). Started at values no draw can ask for, so the first draw always sets it.
  float set_min_depth = -1.0f, set_max_depth = -1.0f;
  int32_t set_x = INT32_MIN, set_y = INT32_MIN;
  uint32_t set_width = 0, set_height = 0;
  const float origin = ViewportOrigin();

  for (size_t index = 0; index < Items.size(); ++index) {
    const DrawItem &item = Items[index];
    // The bisect window. Skipped before anything else, including the pipeline lookup, so a
    // narrowed range costs nothing and cannot build a pipeline the full frame would not have.
    if (index < DrawRangeFirst || index > DrawRangeLast) {
      continue;
    }
    if (index >= DrawHideFirst && index <= DrawHideLast) {
      continue;
    }
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

    // A draw recorded before the game ever set a viewport has no rectangle; the render target is
    // what every draw got before the rectangle was tracked at all, so it stays the fallback.
    const int32_t item_x = item.viewport_width != 0 ? item.viewport_x : 0;
    const int32_t item_y = item.viewport_width != 0 ? item.viewport_y : 0;
    const uint32_t item_width = item.viewport_width != 0 ? item.viewport_width : ViewportWidth;
    const uint32_t item_height = item.viewport_height != 0 ? item.viewport_height : ViewportHeight;
    if (item.min_depth != set_min_depth || item.max_depth != set_max_depth ||
        item_x != set_x || item_y != set_y || item_width != set_width ||
        item_height != set_height) {
      const VkViewport viewport = {static_cast<float>(item_x) + origin,
                                   static_cast<float>(item_y) + origin,
                                   static_cast<float>(item_width),
                                   static_cast<float>(item_height),
                                   item.min_depth,
                                   item.max_depth};
      vkCmdSetViewport(cmd, 0, 1, &viewport);
      // D3D clips to the rectangle and a Vulkan viewport does not, so the scissor is the other
      // half of the same state - measured, not assumed (§4.47). Clamped to the render target
      // because Vulkan rejects a scissor reaching past the framebuffer, where D3D's own
      // validation would simply have refused the SetViewport.
      // Written out rather than with std::min/max: <windows.h> is included here and defines both
      // as macros, so the qualified calls do not even parse.
      const auto clamp_span = [](int64_t origin, int64_t extent, int64_t limit) -> uint32_t {
        const int64_t low = origin < 0 ? 0 : origin;
        int64_t high = origin + extent;
        if (high > limit) {
          high = limit;
        }
        return high > low ? static_cast<uint32_t>(high - low) : 0u;
      };
      const int32_t sx = item_x < 0 ? 0 : item_x;
      const int32_t sy = item_y < 0 ? 0 : item_y;
      const uint32_t sw = clamp_span(item_x, item_width, ViewportWidth);
      const uint32_t sh = clamp_span(item_y, item_height, ViewportHeight);
      const VkRect2D scissor = {{sx, sy}, {sw, sh}};
      vkCmdSetScissor(cmd, 0, 1, &scissor);
      set_min_depth = item.min_depth;
      set_max_depth = item.max_depth;
      set_x = item_x;
      set_y = item_y;
      set_width = item_width;
      set_height = item_height;
      ++TheStats.viewport_sets;
    }

    PushConstants push = {};
    push.vertices =
        item.vertex_source == DrawSource::Scratch ? scratch_vertices : arena_vertices;
    push.draws = draw_records;
    push.lights = lights;
    push.materials = materials;
    push.record = item.record;
    push.material = item.material;
    push.base_vertex = item.base_vertex;
    push.force_lod = ForcedLod;
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
  // Kept rather than dropped, so `render.draw_info(i)` can describe the frame that was just
  // recorded. A swap rather than a copy: the buffers trade places and neither allocates.
  LastItems.swap(Items);
  ClearDraws();
}

void SetDrawRange(uint32_t first, uint32_t last) {
  DrawRangeFirst = first;
  DrawRangeLast = last;
}

void GetDrawRange(uint32_t &first, uint32_t &last) {
  first = DrawRangeFirst;
  last = DrawRangeLast;
}

void SetDrawHide(uint32_t first, uint32_t last) {
  DrawHideFirst = first;
  DrawHideLast = last;
}

void GetDrawHide(uint32_t &first, uint32_t &last) {
  first = DrawHideFirst;
  last = DrawHideLast;
}

std::string DescribeDraw(uint32_t index) {
  if (index >= LastItems.size()) {
    return {};
  }
  const DrawItem &d = LastItems[index];
  const PipelineState &p = d.pipeline;
  char line[1024];
  std::snprintf(
      line, sizeof(line),
      "draw %u of %u\n"
      "  topology %u  %s  count %u  base_vertex %u  first_index %u  vertex_offset %d\n"
      "  vertices from %s, indices from %s, index stride %u\n"
      "  blend %u (src %u dst %u)  depth test %u write %u func %u  cull %u  colour write 0x%x\n"
      "  stencil %u (func %u fail %u zfail %u pass %u  ref %u mask 0x%x write 0x%x)\n"
      // The viewport depth slice belongs here with the rest of the state that decides whether a
      // draw is visible: it is per draw, the game uses six of them and never the default (§4.32),
      // and a draw in the wrong slice is drawn in front of things it should be behind - which
      // looks like a depth-test defect and is not one.
      "  alpha test func %u ref %u   shade %u   material %u   depth slice %.4f..%.4f\n"
      // ...and the rectangle, which is the other half of the same viewport and decides WHERE the
      // draw lands rather than how deep it is (§4.47).
      "  viewport rect %d,%d %ux%u\n"
      "  %u stage(s):  0: tex %d sampler %u colour 0x%08x alpha 0x%08x\n"
      "                1: tex %d sampler %u colour 0x%08x alpha 0x%08x\n",
      index, static_cast<unsigned>(LastItems.size()), p.topology,
      d.indexed ? "indexed" : "non-indexed", d.count, d.base_vertex, d.first_index,
      d.vertex_offset, d.vertex_source == DrawSource::Scratch ? "scratch" : "arena",
      d.index_source == DrawSource::Scratch ? "scratch" : "arena", d.index_stride,
      p.blend_enable, p.src_blend, p.dest_blend, p.depth_test, p.depth_write, p.depth_func,
      p.cull_mode, p.colour_write, p.stencil_enable, p.stencil_func, p.stencil_fail,
      p.stencil_zfail, p.stencil_pass, d.stencil_ref, d.stencil_mask, d.stencil_write_mask,
      d.flags & 0x0fu, (d.flags >> 8) & 0xffu, d.shade_mode, d.material, d.min_depth,
      d.max_depth, d.viewport_x, d.viewport_y, d.viewport_width, d.viewport_height,
      d.stage_count,
      d.stages[0].texture_index == kNoTexture ? -1 : (int)d.stages[0].texture_index,
      d.stages[0].sampler_index, d.stages[0].color, d.stages[0].alpha,
      d.stages[1].texture_index == kNoTexture ? -1 : (int)d.stages[1].texture_index,
      d.stages[1].sampler_index, d.stages[1].color, d.stages[1].alpha);
  return line;
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
  // The reconciliation. Everything the capture layer offered has to be either submitted or
  // skipped for a named reason; anything left over is a path that silently drops draws, which
  // is what §4.32 was. `skipped_no_slot` is the parent of its own breakdown, so the breakdown
  // is deliberately not added in again.
  //
  // `hidden_draws` is in the sum because a material override asking for a draw to be dropped is
  // a named reason like any other - leaving it out would make the invariant read as broken by
  // the one feature that drops draws on purpose, which is exactly how a real regression would
  // stop being noticed.
  const uint64_t accounted =
      TheStats.submitted + TheStats.skipped_topology + TheStats.skipped_no_slot +
      TheStats.skipped_no_transform + TheStats.skipped_unconvertible +
      TheStats.skipped_scratch_full + TheStats.skipped_no_record +
      TheStats.dropped_over_capacity + TheStats.dropped_materials + TheStats.hidden_draws;
  add("draw calls seen: %llu   submitted: %llu   unaccounted for: %lld (must be 0)\n",
      (unsigned long long)TheStats.seen, (unsigned long long)TheStats.submitted,
      (long long)TheStats.seen - (long long)accounted);
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
  add("materials: %llu this frame / %llu peak (%llu dropped - must be 0)   "
      "%llu flat-shaded draws%s\n",
      (unsigned long long)TheStats.materials, (unsigned long long)TheStats.max_materials,
      (unsigned long long)TheStats.dropped_materials,
      (unsigned long long)TheStats.flat_shaded_draws,
      ShadeModeEnabled ? "" : " (SHADEMODE ignored)");
  add("viewport depth-slice changes: %llu\n", (unsigned long long)TheStats.viewport_sets);
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
  // Printed only once something is registered, because 0/0/0 on every report would read as an
  // invariant rather than as "nobody asked for anything". `render.material_overrides` is the
  // reading that says what the keys matched; this one says whether the frame drew with them.
  if (!Overrides.empty()) {
    add("material overrides: %llu registered, %llu images matched, %llu draws overridden, "
        "%llu hidden\n",
        (unsigned long long)Overrides.size(), (unsigned long long)ResolvedMatches,
        (unsigned long long)TheStats.overridden_draws,
        (unsigned long long)TheStats.hidden_draws);
  }
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
