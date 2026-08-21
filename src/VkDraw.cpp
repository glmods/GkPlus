#include "VkDraw.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "Core.h"
#include "Camera.h"
#include "Map.h"
#include "MapLights.h"
#include "World.h"
// Angle brackets, and the reason is not style. Both `*.gen.inc.h` are generated into
// `<binary dir>/generated` and reached through the include path; the quoted form would search
// *this file's own directory first*, so a leftover `src/Shaders.gen.inc.h` from before they moved
// out of the tree - which every existing checkout will have as an untracked file - would silently
// shadow the freshly generated one. That is precisely the stale-SPIR-V failure of §4.46, brought
// back by a punctuation mark.
#include <Shaders.gen.inc.h>
#include "VkContext.h"
#include "VkInternal.h"
#include "VertexFormat.h"
#include "Camera.h"
#include "Map.h"
#include "MapLights.h"
#include "World.h"
#include "VkLighting.h"
#include "VkResources.h"

namespace gk {
namespace vulkan {
namespace {

// The push constant block, matching `Push` in src/shaders/world.slang field for field.
//
// **72 bytes of a guaranteed 128**, where it was 120 before the draw record (§4.26) and 72
// before the material table (§4.30) took it to 48. Everything per-draw is an index into an array
// reached by address, which is §2's design arrived at in full: four addresses, three indices,
// nothing that describes a draw.
//
// The last 28 bytes are not per-draw data at all - they are the LOD probe and the lighting-map
// knobs (§4.34, §4.48), which are the same for every draw in a frame. They ride here because a
// push is the cheapest way to deliver a frame-uniform float, not because a draw needs them: they
// would otherwise be a uniform buffer whose only reader is a feature nobody has switched on.
//
// `base_vertex` is the one index that is not into one of those arrays - it is where this draw's
// buffer starts inside the vertex arena, and it stays here because the vertex shader adds it to
// SV_VertexID before it has read anything at all.
struct PushConstants {
  uint64_t vertices;
  uint64_t draws;
  uint64_t lights;
  uint64_t materials;
  // Everything that is the same for every draw this frame (GpuFrameData in VkDraw.h). It is one
  // address here rather than forty bytes of knobs, which is what took this block from exactly
  // Vulkan's guaranteed 128 down to 56 - see the note below.
  uint64_t frame;
  uint32_t record;      // this draw's entry in `draws`
  uint32_t material;    // ... and in `materials`, shared with every draw of the same surface
  uint32_t base_vertex; // where this draw's buffer starts, in canonical vertices
  uint32_t pad0;
};
// **56 bytes of a guaranteed 128**, and the four fields that are not addresses are the only
// genuinely per-draw values there are. It reached exactly 128 with the light grid (§4.56), which
// is where the next feature would have had to displace something - a 64-byte shadow matrix does
// not fit in nothing. The frame-uniform data moved into a buffer instead, which is what §4.56's
// own note said should happen once there were more than a couple of knobs.
//
// The four hot addresses stay: the vertex shader reaches `vertices` before it has read anything
// at all, and putting it behind `frame` would make that a dependent load on every vertex.
static_assert(sizeof(PushConstants) == 56);

constexpr uint32_t kMaxDrawsPerFrame = 8192;

bool Ready = false;
std::string Error;
DrawStats TheStats;

VkPipelineLayout Layout = VK_NULL_HANDLE;
// What the WORLD pass draws into. Follows `render.hdr`, so it is not necessarily the swapchain's.
VkFormat ColourFormat = VK_FORMAT_UNDEFINED;
// ... and what the swapchain is, which never changes for the life of a device. The tonemap pass
// writes into that, so it needs its own record rather than reading `ColourFormat` - which is the
// one thing HDR moves.
VkFormat SwapchainFormat = VK_FORMAT_UNDEFINED;

// One VkPipeline per distinct fixed-function state, built on first use. Five of them on
// level01 (§4.19), and the map is walked once per draw - a linear scan over five entries would
// do, but the map costs nothing and does not have to be revisited on a level that needs more.
//
// VK_NULL_HANDLE is a legitimate value here: it means "this state was tried and would not
// build", cached so the failure costs one attempt rather than one per draw.
std::map<PipelineState, VkPipeline> Pipelines;
VkShaderModule VertexModule = VK_NULL_HANDLE;
VkShaderModule FragmentModule = VK_NULL_HANDLE;
// The PN-triangle amplification pass (§4.71). All three stay VK_NULL_HANDLE on a device with no
// `tessellationShader`, which is the one test `CreatePipelineFor` makes before building a
// tessellated variant - so "this device cannot" and "nothing asked for it" are the same state.
VkShaderModule TessVertexModule = VK_NULL_HANDLE;
VkShaderModule HullModule = VK_NULL_HANDLE;
VkShaderModule DomainModule = VK_NULL_HANDLE;

VkImage Depth = VK_NULL_HANDLE;
VkDeviceMemory DepthMemory = VK_NULL_HANDLE;
VkImageView DepthView = VK_NULL_HANDLE;
VkFormat DepthFormat = VK_FORMAT_UNDEFINED;
bool DepthStencil = false;

// See SetMsaa in VkDraw.h. Two values and not one, because "what was asked for" and "what the
// frame is drawn at" genuinely differ: a device may not support the count, and its target may
// fail to allocate. `SampleCount` is the second - it is what CreateDepth stamps on the depth
// image and what CreatePipelineFor writes into `rasterizationSamples`, so those two agree by
// construction rather than by two reads of the same knob.
uint32_t MsaaRequested = 1;
bool MsaaEnvRead = false;
VkSampleCountFlagBits SampleCount = VK_SAMPLE_COUNT_1_BIT;
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

// The count the device will actually take, from the one the caller asked for. Rounds DOWN twice:
// to a power of two, then to a bit `DeviceCaps::sample_counts` has - so an unsupported 8 lands on
// 4 rather than failing, and a request of 3 lands on 2 rather than being rejected as malformed.
// Cannot return zero: VK_SAMPLE_COUNT_1_BIT is always supported, and a caps mask that somehow did
// not contain it would still leave the `1` this starts from.
VkSampleCountFlagBits ClampSamples(uint32_t requested) {
  uint32_t best = 1;
  for (uint32_t candidate = 2; candidate <= 64; candidate *= 2) {
    if (candidate > requested || (Caps().sample_counts & candidate) == 0) {
      continue;
    }
    best = candidate;
  }
  return static_cast<VkSampleCountFlagBits>(best);
}

// `GKPLUS_VK_MSAA` - the value the first frame comes up at, read once and lazily for the reason
// LocalShadowsEnabled gives: DllMain is far too early to ask the environment anything. It writes
// `MsaaRequested` and then never runs again, so a later `render.msaa` is not fighting it.
void ReadMsaaEnvOnce() {
  if (MsaaEnvRead) {
    return;
  }
  MsaaEnvRead = true;
  char value[16] = {};
  const DWORD len = ::GetEnvironmentVariableA("GKPLUS_VK_MSAA", value, sizeof(value));
  if (len == 0 || len >= sizeof(value)) {
    return;
  }
  const uint32_t parsed = static_cast<uint32_t>(std::strtoul(std::string(value, len).c_str(),
                                                             nullptr, 10));
  if (parsed != 0) {
    MsaaRequested = parsed;
  }
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
  // The world pass's colour attachment, the depth image and every pipeline bound inside it must
  // agree on this, which is why all three read the one `SampleCount` rather than a knob each.
  info.samples = SampleCount;
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
  // A state asking for tessellation on a device that has none, or before the modules exist, is a
  // state that must not be built - PipelineFor caches the null and RecordDraws skips the draw,
  // which would drop the level. `WantsTessellation` is what keeps that from happening: it clears
  // the bit before the key is ever formed, so this is a belt-and-braces check rather than a path
  // anything reaches.
  const bool tessellate = state.tessellate != 0 && HullModule != VK_NULL_HANDLE &&
                          DomainModule != VK_NULL_HANDLE &&
                          TessVertexModule != VK_NULL_HANDLE;

  // The entry point names are the Slang ones, kept verbatim by -fvk-use-entrypoint-name rather
  // than being rewritten to "main". Worth the flag: two modules both called "main" is exactly
  // the sort of thing that goes unnoticed until the wrong stage is bound.
  //
  // The tessellated variant swaps the vertex stage as well as adding two: its vertex shader
  // outputs a ControlPoint where the ordinary one outputs a finished VertexOut. That is what
  // leaves `vertex_main` byte-for-byte the shader it was, so `render.tessellation = false` is
  // bit-identical by construction rather than by inspection.
  const VkPipelineShaderStageCreateInfo stages[] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_VERTEX_BIT, tessellate ? TessVertexModule : VertexModule,
       tessellate ? "tess_vertex_main" : "vertex_main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_FRAGMENT_BIT, FragmentModule, "fragment_main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, HullModule, "hull_main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, DomainModule, "domain_main", nullptr},
  };

  // Three control points, because a patch is a triangle. The index buffer is unchanged: a
  // triangle list and a 3-control-point patch list consume indices identically, which is why
  // this needs no second index layout and no re-upload.
  VkPipelineTessellationStateCreateInfo tessellation = {
      VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
  tessellation.patchControlPoints = 3;

  // No vertex input state at all: the vertex shader pulls from the arena by address. This is
  // the whole reason a draw binds nothing.
  VkPipelineVertexInputStateCreateInfo vertex_input = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

  VkPipelineInputAssemblyStateCreateInfo assembly = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  // PATCH_LIST and not the draw's own topology: with a tessellator in the pipeline the input
  // assembler produces patches, and the primitive the rasteriser eventually sees is the domain
  // shader's, not this one's. `WantsTessellation` has already required a triangle list, so this
  // never silently reinterprets a strip.
  assembly.topology = tessellate ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : ToTopology(state.topology);

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

  // Not dynamic - there is no VK_DYNAMIC_STATE for it in core - so this is baked, and the whole
  // cache has to go when the count moves. `ApplySampleCount` is what does that; nothing here can,
  // because a pipeline is only ever built on first sight of its state.
  //
  // `sampleShadingEnable` stays off deliberately: the fragment shader runs once per PIXEL and its
  // result is written to every covered sample. That is what makes this cost coverage and a resolve
  // rather than N times the shading, and it is also why the AO fetch in world.slang - a `Load` at
  // `input.position.xy` - keeps landing on the same texel it does at one sample.
  VkPipelineMultisampleStateCreateInfo multisample = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  // **The 2D pass is never multisampled**, whatever `render.msaa` says. Its target is the
  // single-sample LDR image and its content is screen-space quads and text - there is no geometric
  // edge in it to antialias, and the game drew it unmultisampled on every renderer it ever had. It
  // is also not optional: a pipeline's sample count must equal its attachment's, and validation
  // says so on every draw.
  multisample.rasterizationSamples =
      state.ldr_target != 0 ? VK_SAMPLE_COUNT_1_BIT : SampleCount;

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
  // The world pass's target, or the LDR one the 2D layers land in after the tonemap. The two are
  // the same format whenever HDR is off, and `ldr_target` is never set then, so this reduces to
  // what it always was.
  const VkFormat attachment_format = state.ldr_target != 0 ? SwapchainFormat : ColourFormat;
  rendering.pColorAttachmentFormats = &attachment_format;
  rendering.depthAttachmentFormat = DepthFormat;
  rendering.stencilAttachmentFormat = DepthStencil ? DepthFormat : VK_FORMAT_UNDEFINED;

  VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = tessellate ? 4 : 2;
  info.pStages = stages;
  info.pTessellationState = tessellate ? &tessellation : nullptr;
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

// Every world pipeline destroyed and the cache emptied, so the next draw of each state builds one
// against whatever the pipeline-wide state now is. Shutdown's use is the obvious one; the other is
// a sample-count change, which invalidates all of them at once.
//
// **The caller owns the wait.** Nothing here checks that the pipelines are idle, because both
// callers already hold the `vkDeviceWaitIdle` they need for their own reasons and taking a second
// one under the queue lock would be the deadlock, not the safety.
void DestroyPipelineCache() {
  for (const auto &[state, pipeline] : Pipelines) {
    if (pipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(GetDevice(), pipeline, nullptr);
    }
  }
  Pipelines.clear();
  TheStats.pipelines = 0;
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
  // Only where the device has the feature: a shader module for a stage the device cannot run is
  // accepted by vkCreateShaderModule and rejected by vkCreateGraphicsPipelines, so gating here
  // keeps the failure out of the per-draw path entirely rather than costing one attempt per
  // pipeline state.
  if (Caps().tessellation_shader) {
    TessVertexModule = CreateModule(kTessVertexMainSpv, sizeof(kTessVertexMainSpv));
    HullModule = CreateModule(kHullMainSpv, sizeof(kHullMainSpv));
    DomainModule = CreateModule(kDomainMainSpv, sizeof(kDomainMainSpv));
  }

  auto set_layout = reinterpret_cast<VkDescriptorSetLayout>(BindlessDescriptorSetLayout());
  // The two tessellation stages are in the range whether or not this device can run them: a push
  // constant block must match across every stage of a pipeline, and a range naming a stage no
  // pipeline uses costs nothing. Leaving them out would make every tessellated pipeline invalid
  // in a way that only shows up as validation noise at draw time.
  VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                   VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                   VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                               0, sizeof(PushConstants)};
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

// --- the sun's shadow map ---------------------------------------------------------------------
//
// A depth-only pass over the same draw list the world pass walks, from the sun's point of view.
// §2's design is what makes it cheap: a draw is an index into shared per-frame tables, so a second
// walk needs a pipeline and a push block and no new per-draw data at all.

// One tile per cascade, in a 2x2 atlas - so ONE image, one bindless slot and one sampler however
// many cascades are live (§4.59). A texture array would need a second declaration in the bindless
// set, whose last binding is pinned by VARIABLE_DESCRIPTOR_COUNT; the atlas costs a uv offset in
// the lookup and nothing else. 4096 is the `maxImageDimension2D` every Vulkan device guarantees,
// which is the reason the tile is 2048 and not larger.
constexpr uint32_t kShadowTile = 2048;
constexpr uint32_t kShadowAtlas = kShadowTile * 2;

// Must match ShadowPush in src/shaders/shadow.slang. One block for both of that file's entry
// points, because a stage may only have one: the sun's pass fills `record`/`base_vertex` and
// leaves `params` null, the map lights' indirect bake does the opposite (§4.62).
struct ShadowPushConstants {
  uint64_t vertices;
  uint64_t draws;
  uint64_t params;      // the indirect path only - {record, base_vertex} per command
  uint32_t record;      // the direct path only
  uint32_t base_vertex; // ... and so is this
  float light_matrix[16];
  // The PN pass (§4.71). Only the two that shape the patch, plus one uniform factor - see
  // shadow.slang for why the factors need not match the world pass's and the two knobs must.
  float pn_strength;
  float pn_flat_threshold;
  // The world-unit ceiling on a control point's offset (§4.74) shapes the patch, so it has to
  // travel with the two above - a shadow cast by a surface the colour pass no longer draws is the
  // same defect as the two knobs disagreeing. **This was `pad_shadow`**, the slot the alignment
  // note below created.
  float pn_max_offset;
  float tess_factor;
  // The split-corner table, which shapes the patch and therefore travels with the three knobs
  // above for exactly their reason: a seam the colour pass now closes would otherwise still cast
  // the shadow of one torn open (§4.71).
  uint64_t split_corners;
  uint32_t split_base;
  uint32_t split_count;
  // **Slang rounds the block up to its own alignment and this side must too.** The struct
  // contains a float4 array, so its alignment is 16 and its size rounds to a multiple of 16 -
  // 112 before the table below was added, not the 108 the fields then added up to. Caught by
  // `src/gen-shader-abi.py`, which reported the Slang side as 112 bytes against a 108-byte
  // assert; without it the range and the push would have disagreed by four bytes.
};
// 128: three addresses, two words, the matrix, the four PN scalars and the split-corner table.
// **Exactly the guaranteed minimum**, which is the ceiling this block now sits on - anything
// further needs a descriptor rather than a push, or `maxPushConstantsSize` checked at startup.
static_assert(sizeof(ShadowPushConstants) == 128);

VkImage ShadowImage = VK_NULL_HANDLE;
VkFormat ShadowFormat = VK_FORMAT_UNDEFINED;
bool ShadowStencilAspect = false;
VkDeviceSize ShadowBytes = 0;
VkDeviceMemory ShadowMemory = VK_NULL_HANDLE;
VkImageView ShadowAttachmentView = VK_NULL_HANDLE; // every aspect, for rendering into
VkImageView ShadowSampleView = VK_NULL_HANDLE;     // depth only, for sampling
VkPipelineLayout ShadowLayout = VK_NULL_HANDLE;
VkPipeline ShadowPipeline = VK_NULL_HANDLE;
VkShaderModule ShadowModule = VK_NULL_HANDLE;
// The PN-triangle twins (§4.71). One hull and one domain serve all four shadow pipelines,
// because the draw record rides in the control point and that is the only thing the direct and
// indirect paths disagree about. All of these stay null on a device with no `tessellationShader`,
// and `ShadowStages` then reports one stage - which is the untessellated pipeline exactly.
VkShaderModule ShadowTessVertexModule = VK_NULL_HANDLE;
// The indirect entry points' modules. Declared with the sun's rather than with the map bake's,
// because all three passes that submit indirectly take them and the sun's is the one that comes
// up first - it must not depend on an atlas that may never be created.
VkShaderModule MapShadowModule = VK_NULL_HANDLE;
VkShaderModule MapShadowTessVertexModule = VK_NULL_HANDLE;
VkShaderModule ShadowHullModule = VK_NULL_HANDLE;
VkShaderModule ShadowDomainModule = VK_NULL_HANDLE;
VkPipeline ShadowPipelineTess = VK_NULL_HANDLE;
// The indirect twins (§4.77), taking `map_shadow_vertex` / `map_shadow_tess_vertex` against the
// sun's own D32 format. Its own pipelines rather than the per-frame bake's for exactly the reason
// that one has its own rather than the map bake's: the attachment format differs, and under
// dynamic rendering that is part of the pipeline.
VkPipeline ShadowPipelineIndirect = VK_NULL_HANDLE;
VkPipeline ShadowPipelineIndirectTess = VK_NULL_HANDLE;
// ... and its own batch buffer, because the sun's pass and the two bakes all record into one
// command buffer and `vkCmdUpdateBuffer` writes its bytes in order - sharing a slice would have
// whichever ran second overwrite the first.
VkBuffer SunIndirectBuffer = VK_NULL_HANDLE;
VkDeviceMemory SunIndirectMemory = VK_NULL_HANDLE;
uint64_t SunIndirectAddress = 0;
uint64_t SunRingSerial = 0;

// **A feature, on**: cull the caster set per cascade, and submit what survives as one indirect
// command run per (cascade, bucket) instead of a draw call per caster per cascade.
bool SunCullEnabled = true;
bool SunIndirectEnabled = true;
uint32_t SunCasterCount = 0;      // distinct casters the frame offered, last pass
uint32_t SunBucketCount = 0;
uint32_t SunCastersDropped = 0;   // past the cap
uint32_t SunCascadeSubmits = 0;   // (caster, cascade) pairs drawn
uint32_t SunCascadeCulled = 0;    // ... rejected by the cascade's own box
uint32_t SunUnbounded = 0;        // casters with no world box, drawn into every cascade
uint32_t SunCommandsDropped = 0;  // (caster, cascade) pairs past kShadowMaxCommands
uint32_t SunDrawCalls = 0;        // what actually reached the command buffer, last pass
// Per cascade, so the halving is visible rather than inferred: cascade 0 is the sharp near box
// and should keep almost nothing on a level of any size.
uint32_t SunCascadeDrawn[kMaxShadowCascades] = {};

// Whether a tessellated shadow pipeline can be built at all. Separate from the runtime knob: this
// is about the device and the modules, and it is what makes every `*Tess` pipeline below simply
// absent rather than a failed create.
bool ShadowTessAvailable() {
  return ShadowHullModule != VK_NULL_HANDLE && ShadowDomainModule != VK_NULL_HANDLE;
}

// The stage list for one shadow pipeline, tessellated or not. `stages` must hold 3.
//
// One place for all four pipelines, so a change to the tessellated stage set cannot reach three of
// them and miss the fourth - which is exactly the shape of defect §4.67 cost two sections to find.
uint32_t ShadowStages(VkPipelineShaderStageCreateInfo *stages, VkShaderModule vertex_module,
                      const char *vertex_entry, bool tessellate) {
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
               VK_SHADER_STAGE_VERTEX_BIT, vertex_module, vertex_entry, nullptr};
  if (!tessellate) {
    return 1;
  }
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
               VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, ShadowHullModule, "shadow_hull",
               nullptr};
  stages[2] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
               VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, ShadowDomainModule, "shadow_domain",
               nullptr};
  return 3;
}

// Every stage the shadow push range names. A push must cover the whole overlapping range and not
// merely the stages that will read it, so this is one constant rather than a literal at each of
// the five push sites - which is how the world pass's equivalent came to be wrong for a build.
constexpr VkShaderStageFlags kShadowPushStages = VK_SHADER_STAGE_VERTEX_BIT |
                                                 VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                                 VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;

// Whether the shadow passes take their tessellated pipelines this frame. Three conditions, and
// `render.tess_shadows` is the separable one: the bake is where the cost is, so a frame-time
// regression has to be attributable to one half of the feature rather than to the whole of it.
bool ShadowTessellating() {
  return TessellationEnabled() && TessellationShadows() && ShadowTessAvailable();
}

// The four PN fields every shadow push carries.
//
// **The two knobs are filled whether or not this pass is tessellating**, and the factor is what
// switches: they have to be the ones the colour pass used, because the patch is a function of them
// and the shadow has to be cast by the surface actually drawn. A factor of 1 makes the pass sample
// that same patch at its corners, which is the untessellated triangle.
void SplitCornerTableFor(uint64_t &address, uint32_t &base, uint32_t &count);

void FillShadowTessPush(ShadowPushConstants &push) {
  const TessellationParams &tess = TessParams();
  push.pn_strength = tess.pn_strength;
  push.pn_flat_threshold = tess.pn_flat_threshold;
  // Negative would flip the clamp's own bounds and make it return the wrong endpoint, so it is
  // floored here rather than trusted from the knob.
  push.pn_max_offset = tess.pn_max_offset < 0.0f ? 0.0f : tess.pn_max_offset;
  push.tess_factor = tess.shadow_factor < 1.0f ? 1.0f : tess.shadow_factor;
  // The same table the colour pass reads, from the same place, for the reason the three knobs
  // above are here: a seam the world pass now closes must not still cast a torn shadow.
  SplitCornerTableFor(push.split_corners, push.split_base, push.split_count);
}
bool ShadowReady = false;
bool SunShadowsEnabled = true;
// See SetStencilShadow in VkDraw.h. Draw the game's own blob shadow as well as the sun's map.
bool StencilShadowEnabled = false;
// In shadow texels - see SetShadowBias. 2.5 is where level04's self-shadowing collapses and
// level02's is already gone; below 2 both levels shadow themselves everywhere (§4.59).
float ShadowBiasValue = 2.5f;
// 1.0 is the *physically* correct value - the shadow attenuates only the direct terms, so 1 is
// "no sunlight arrives here" - and 0.7 is the measured one (§4.59). Two frames decide it: on
// level04's outdoor start §4.58's 0.55 leaves the unit shadows reading as a smudge, and on
// level02's covered start 1.0 takes the ground to 36% of its authored brightness where 0.7 leaves
// it at 65%. Level02 is genuinely under cover and the shadow map is right about it; the level's
// own bake is what disagrees, and there is no arguing with that from here.
float ShadowStrengthValue = 0.7f;
float ShadowExtentValue = 70.0f;
int ShadowCascadeCount = static_cast<int>(kMaxShadowCascades);

// Built once a frame by BuildSunCascades and read by both the shadow pass and UploadFrameData.
float FrameSunMatrix[16] = {};                          // world -> light space, rotation only
float FrameCascade[kMaxShadowCascades][4] = {};         // centre.xy, 1/extent, bias in ndc depth
float FrameCascadeMatrix[kMaxShadowCascades][16] = {};  // ... and the clip matrix that rasterises it
float FrameSunZNear = 0.0f;
float FrameSunZSpan = 1.0f;
uint32_t FrameCascadeCount = 0;
bool FrameSunValid = false;

// --- the geometry every shadow pass culls with -------------------------------------------------
//
// Shared by the sun's cascades and by the per-frame cube atlas, which is the point: both project
// with a row-vector matrix whose clip z runs 0..w, so one plane extraction serves both and the
// two cannot drift into disagreeing about a projection.

// The six clip planes of a projection matrix, in world space, as `dot(n, p) + d >= 0` for a point
// inside.
//
// Gribb-Hartmann: `clip.j` is `dot(v, column(j)) + m[12 + j]`, so `w + x >= 0` is the left plane
// and the rest follow. **The z column alone is the near plane** because these projections put
// clip z in 0..w, not -w..w - the same convention the shader depends on, and the one thing here
// that would be wrong if it were copied from an OpenGL derivation.
//
// Extracted from the matrix rather than rebuilt from the light's axes, deliberately: whatever
// BuildCubeFaceMatrix or BuildSunCascades does, the cull and the rasteriser then cannot disagree
// about it. For a cube face that includes kMapShadowNear, which clips geometry closer to the
// light than `range / 64`; for a cascade it includes the deliberately generous near and far,
// which is what makes the plane test *exact* there rather than an approximation - a caster behind
// the box along the light direction is inside the z range by construction, so there is no
// "occluder outside the frustum still casts into it" case to handle.
//
// The planes come out unnormalised. Nothing below divides by their length, so it does not matter.
void BuildFrustumPlanes(const float *m, float planes[6][4]) {
  float column[4][4];
  for (int j = 0; j < 4; ++j) {
    for (int i = 0; i < 4; ++i) {
      column[j][i] = m[i * 4 + j];
    }
  }
  for (int i = 0; i < 4; ++i) {
    planes[0][i] = column[3][i] + column[0][i]; // left
    planes[1][i] = column[3][i] - column[0][i]; // right
    planes[2][i] = column[3][i] + column[1][i]; // bottom
    planes[3][i] = column[3][i] - column[1][i]; // top
    planes[4][i] = column[2][i];                // near
    planes[5][i] = column[3][i] - column[2][i]; // far
  }
}

// Whether a box lies entirely outside one of the planes, which is the only thing a cull may act
// on. A box that fails no plane may still miss the frustum - the classic corner case - and
// drawing it is the conservative answer, so this returns false there and costs one caster.
bool BoxOutsideFrustum(const float planes[6][4], const float *lo, const float *hi) {
  for (int p = 0; p < 6; ++p) {
    const float *n = planes[p];
    // The corner furthest along the plane's normal. If even that one is behind the plane, all
    // eight are, and no part of the box can be inside.
    const float x = n[0] >= 0.0f ? hi[0] : lo[0];
    const float y = n[1] >= 0.0f ? hi[1] : lo[1];
    const float z = n[2] >= 0.0f ? hi[2] : lo[2];
    if (n[0] * x + n[1] * y + n[2] * z + n[3] < 0.0f) {
      return true;
    }
  }
  return false;
}

// Box against a light's sphere of influence, by the closest point on the box to the centre.
// Cheaper than six planes and it answers for all six cube faces at once, which is why the
// per-frame bake runs it first. The sun has no such test - a directional light reaches
// everything, and its cascade box is the whole of its extent.
bool BoxOutsideSphere(const float *lo, const float *hi, const Vec3 &centre, float radius) {
  const float c[3] = {centre.x, centre.y, centre.z};
  float distance_squared = 0.0f;
  for (int i = 0; i < 3; ++i) {
    const float away = c[i] < lo[i] ? lo[i] - c[i] : (c[i] > hi[i] ? c[i] - hi[i] : 0.0f);
    distance_squared += away * away;
  }
  return distance_squared > radius * radius;
}

// `vkCmdUpdateBuffer` takes at most 64 KB in one call, and both culled batches are deliberately
// allowed to be larger - so they go in pieces. Every offset and every size stays a multiple of 4,
// which the API requires too: 65536 is one, and both arrays are a whole number of 20- or 8-byte
// records.
void UpdateBufferChunked(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset,
                         const void *data, size_t bytes) {
  constexpr size_t kChunk = 65536;
  const auto *src = static_cast<const uint8_t *>(data);
  while (bytes > 0) {
    const size_t take = bytes < kChunk ? bytes : kChunk;
    vkCmdUpdateBuffer(cmd, buffer, offset, take, src);
    offset += take;
    src += take;
    bytes -= take;
  }
}

// The indirect batch's shape, shared by the two passes that build one per frame. 8192 commands is
// 160 KB of commands and 64 KB of parameters; four slices against two frames in flight, for the
// hazard `kMapIndirectRing` documents.
constexpr uint32_t kShadowIndirectStride = 20; // sizeof(VkDrawIndexedIndirectCommand)
constexpr uint32_t kShadowMaxCommands = 8192;
constexpr uint32_t kShadowParamOffset = kShadowMaxCommands * kShadowIndirectStride;
static_assert(kShadowParamOffset % 16 == 0, "the parameter array is read as uint2 by address");
constexpr uint32_t kShadowIndirectSlice = kShadowParamOffset + kShadowMaxCommands * 8;
static_assert(kShadowIndirectSlice % 16 == 0,
              "a slice must keep the parameter array's alignment, since it is read by address");
constexpr uint32_t kShadowIndirectRing = 4;

// --- the caster set, collected once and shared -------------------------------------------------
//
// One group of casters that can share a single indirect batch. **A batch has one bound index
// buffer and one `vertices` address**, so a draw pulling its vertices from the frame's scratch
// cannot join one pulling from the arena - and units do exactly that (§4.18). §4.61's map bake
// sidesteps this by taking only arena-sourced map geometry; the two per-frame passes have to
// carry whatever the frame holds, so they bucket instead of dropping.
struct CasterBucket {
  DrawSource vertex_source = DrawSource::Arena;
  DrawSource index_source = DrawSource::Arena;
  uint32_t index_stride = 2;
  uint32_t first = 0; // where this bucket's casters start, in entries into `ordered`
  uint32_t count = 0;
};

// **Opaque, depth-writing, indexed geometry.** A blended draw is an effect layer or a decal and
// would cast a solid shadow it does not have; a draw that does not write depth is by the game's
// own account not part of the scene's occlusion. The shader rejects anything that is not a lit 3D
// draw as well, which is the test the CPU cannot make - the flags live in the record.
//
// **One definition for both passes**, which they did not have: the sun's pass spelled the test out
// inline and the per-frame bake reached it through `IsDynamicCaster`. Identical, and nothing said
// so or would have caught them drifting.
bool IsShadowCaster(const DrawItem &item) {
  return item.indexed && !item.pipeline.blend_enable && item.pipeline.depth_write;
}

// Collects the frame's casters into `ordered`, **sorted so that a bucket is a contiguous run of
// it**, and describes the runs in `buckets`. `dropped` counts what the cap refused.
//
// The sort is the thing to be careful about. It reorders the draws, which is only safe because
// this feeds a depth-only pass: the result is the minimum depth over the set and a minimum does
// not care what order it was taken in. Nothing here may be reused for a colour pass on that
// basis.
void CollectCasters(bool (*accept)(const DrawItem &), uint32_t limit,
                    std::vector<const DrawItem *> &ordered, std::vector<CasterBucket> &buckets,
                    uint32_t &dropped);

// Defined beside the per-frame atlas, where the memory helper it needs lives, and called from
// CreateShadowPass - which runs first and must not depend on that atlas existing.
bool CreateSunIndirectBuffer();

bool CreateShadowPass() {
  ShadowModule = CreateModule(kShadowVertexSpv, sizeof(kShadowVertexSpv));
  if (ShadowModule == VK_NULL_HANDLE) {
    return Fail("could not create the shadow shader module");
  }
  // The PN twins, only where the device can run them (§4.71). Not a failure if they are absent:
  // the shadow passes then draw the untessellated caster set, which is what they did before.
  if (Caps().tessellation_shader) {
    ShadowTessVertexModule = CreateModule(kShadowTessVertexSpv, sizeof(kShadowTessVertexSpv));
    ShadowHullModule = CreateModule(kShadowHullSpv, sizeof(kShadowHullSpv));
    ShadowDomainModule = CreateModule(kShadowDomainSpv, sizeof(kShadowDomainSpv));
  }

  // **D32_SFLOAT where the device has it, not the world pass's format.** Nothing here reads a
  // stencil aspect, and the format §4.27 chose carries one - which costs a byte a texel over an
  // atlas four times the old map's area, and forces the two-view dance below. The fallback is
  // that same format, so a device without it is merely fatter rather than shadowless.
  VkFormatProperties properties = {};
  vkGetPhysicalDeviceFormatProperties(GetPhysicalDevice(), VK_FORMAT_D32_SFLOAT, &properties);
  constexpr VkFormatFeatureFlags kNeeded = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                           VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  ShadowFormat = (properties.optimalTilingFeatures & kNeeded) == kNeeded ? VK_FORMAT_D32_SFLOAT
                                                                        : DepthFormat;
  ShadowStencilAspect = ShadowFormat != VK_FORMAT_D32_SFLOAT && DepthStencil;

  VkImageCreateInfo image = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image.imageType = VK_IMAGE_TYPE_2D;
  image.format = ShadowFormat;
  image.extent = {kShadowAtlas, kShadowAtlas, 1};
  image.mipLevels = 1;
  image.arrayLayers = 1;
  image.samples = VK_SAMPLE_COUNT_1_BIT;
  image.tiling = VK_IMAGE_TILING_OPTIMAL;
  // SAMPLED as well as the attachment bit - which the main depth buffer deliberately lacks,
  // because nothing reads it. This one exists to be read.
  image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(GetDevice(), &image, nullptr, &ShadowImage) != VK_SUCCESS) {
    return Fail("could not create the shadow image");
  }

  VkMemoryRequirements requirements = {};
  vkGetImageMemoryRequirements(GetDevice(), ShadowImage, &requirements);
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
    return Fail("no device-local memory type for the shadow image");
  }
  ShadowBytes = requirements.size;
  VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = type;
  if (vkAllocateMemory(GetDevice(), &allocate, nullptr, &ShadowMemory) != VK_SUCCESS ||
      vkBindImageMemory(GetDevice(), ShadowImage, ShadowMemory, 0) != VK_SUCCESS) {
    return Fail("could not back the shadow image");
  }

  // **Two views of one image**, and only where the fallback format is in force. The attachment
  // needs every aspect the format carries; the sampled one must be DEPTH ONLY, because a view
  // with a stencil aspect cannot be a sampled image. With D32_SFLOAT the two are identical and
  // this creates the same view twice, which is cheaper to read than a branch.
  VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = ShadowImage;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = ShadowFormat;
  view.subresourceRange.aspectMask =
      VK_IMAGE_ASPECT_DEPTH_BIT | (ShadowStencilAspect ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
  view.subresourceRange.levelCount = 1;
  view.subresourceRange.layerCount = 1;
  if (vkCreateImageView(GetDevice(), &view, nullptr, &ShadowAttachmentView) != VK_SUCCESS) {
    return Fail("could not create the shadow attachment view");
  }
  view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  if (vkCreateImageView(GetDevice(), &view, nullptr, &ShadowSampleView) != VK_SUCCESS) {
    return Fail("could not create the shadow sample view");
  }
  WriteBindlessView(kShadowMapSlot, reinterpret_cast<uint64_t>(ShadowSampleView));

  VkPushConstantRange range = {kShadowPushStages, 0, sizeof(ShadowPushConstants)};
  VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &range;
  if (vkCreatePipelineLayout(GetDevice(), &layout, nullptr, &ShadowLayout) != VK_SUCCESS) {
    return Fail("could not create the shadow pipeline layout");
  }

  VkPipelineShaderStageCreateInfo stages[3] = {};

  VkPipelineVertexInputStateCreateInfo vertex_input = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineTessellationStateCreateInfo tessellation = {
      VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
  tessellation.patchControlPoints = 3;
  VkPipelineViewportStateCreateInfo viewport = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  // **No culling.** Front-face culling is the usual trick for hiding acne, and it assumes closed,
  // consistently-wound geometry; Gunlok has neither across the map object and its props, so it
  // would open holes in the caster set instead. The depth bias is the knob for acne.
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = VK_TRUE;
  depth.depthWriteEnable = VK_TRUE;
  depth.depthCompareOp = VK_COMPARE_OP_LESS;
  VkPipelineColorBlendStateCreateInfo blend = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;

  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.depthAttachmentFormat = ShadowFormat;
  if (ShadowStencilAspect) {
    rendering.stencilAttachmentFormat = ShadowFormat;
  }

  VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  // **No fragment shader at all**, which is legal with no colour attachment and is what makes this
  // pass nearly free per fragment. The tessellated twin adds two stages in front of that, not
  // behind it.
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = ShadowLayout;

  info.stageCount = ShadowStages(stages, ShadowModule, "shadow_vertex", false);
  if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr, &ShadowPipeline) !=
      VK_SUCCESS) {
    return Fail("could not create the shadow pipeline");
  }
  // Built up front beside it rather than on demand, so `render.tess_shadows` is a knob that can be
  // flipped mid-level without a pipeline compile inside a frame - the same reason the two
  // per-frame bake pipelines are both built eagerly.
  if (ShadowTessAvailable() && ShadowTessVertexModule != VK_NULL_HANDLE) {
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    info.pTessellationState = &tessellation;
    info.stageCount = ShadowStages(stages, ShadowTessVertexModule, "shadow_tess_vertex", true);
    if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr,
                                  &ShadowPipelineTess) != VK_SUCCESS) {
      // Not fatal, and deliberately: the untessellated pipeline above is already built, so the
      // pass keeps working and only the knob becomes inert.
      ShadowPipelineTess = VK_NULL_HANDLE;
    }
  }

  // The indirect twins (§4.77). Built here and not on demand, and **built whether or not the
  // device has `multiDrawIndirect`** - one `vkCmdDrawIndexedIndirect` of N commands is what needs
  // that feature, so a device without it simply keeps `render.sun_shadow_indirect` false and
  // takes the direct path, which is the same pass over the same culled set.
  //
  // The modules are the map bake's entry points, created here rather than there so the sun's pass
  // never depends on an atlas that may not exist.
  if (MapShadowModule == VK_NULL_HANDLE) {
    MapShadowModule = CreateModule(kMapShadowVertexSpv, sizeof(kMapShadowVertexSpv));
  }
  if (Caps().tessellation_shader && MapShadowTessVertexModule == VK_NULL_HANDLE) {
    MapShadowTessVertexModule =
        CreateModule(kMapShadowTessVertexSpv, sizeof(kMapShadowTessVertexSpv));
  }
  if (Caps().multi_draw_indirect && MapShadowModule != VK_NULL_HANDLE &&
      CreateSunIndirectBuffer()) {
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    info.pTessellationState = nullptr;
    info.stageCount = ShadowStages(stages, MapShadowModule, "map_shadow_vertex", false);
    if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr,
                                  &ShadowPipelineIndirect) != VK_SUCCESS) {
      ShadowPipelineIndirect = VK_NULL_HANDLE;
    }
    if (ShadowPipelineIndirect != VK_NULL_HANDLE && ShadowTessAvailable() &&
        MapShadowTessVertexModule != VK_NULL_HANDLE) {
      assembly.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
      info.pTessellationState = &tessellation;
      info.stageCount =
          ShadowStages(stages, MapShadowTessVertexModule, "map_shadow_tess_vertex", true);
      if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr,
                                    &ShadowPipelineIndirectTess) != VK_SUCCESS) {
        ShadowPipelineIndirectTess = VK_NULL_HANDLE;
      }
    }
  }
  // Read back rather than assumed, for the same reason `map_shadow_indirect` is: the two paths
  // produce the same atlas, so nothing on screen would ever say which one ran.
  SunIndirectEnabled = ShadowPipelineIndirect != VK_NULL_HANDLE;
  ShadowReady = true;
  return true;
}

// --- the map lights' static shadow atlas ------------------------------------------------------
//
// One cube per map light, baked from the map's own geometry once per level. Everything about it
// that differs from the sun's pass follows from one fact: **neither the lights nor the world ever
// move**, so this is authored data rather than a per-frame quantity (§4.61).
//
// It shares the sun's vertex shader unchanged - `shadow_vertex` multiplies a world position by
// whatever matrix it is pushed, and a perspective one works exactly as an orthographic one does
// there. What it cannot share is the pipeline, because the atlas is `D16_UNORM` where the sun's
// is `D32_SFLOAT`, and the depth format is part of a pipeline's rendering info.

VkImage MapShadowImage = VK_NULL_HANDLE;
VkDeviceMemory MapShadowMemory = VK_NULL_HANDLE;
VkImageView MapShadowView = VK_NULL_HANDLE;
VkPipeline MapShadowPipeline = VK_NULL_HANDLE;
VkPipeline MapShadowPipelineTess = VK_NULL_HANDLE; // §4.71's twin
VkFormat MapShadowFormat = VK_FORMAT_UNDEFINED;
VkDeviceSize MapShadowBytes = 0;
bool MapShadowReady = false;

// The indirect batch (§4.62). One `VkDrawIndexedIndirectCommand` per caster plus a parallel
// {record, base_vertex} for the shader, in one buffer - the commands first, the parameters after
// them at a 16-byte-aligned offset.
//
// **Both regions are rewritten every bake frame**, and that is not laziness: `record` is an index
// into the frame's own scratch, which rotates, so a buffer built once and reused across the
// bake's hundred-odd frames would address the wrong records on all but the first. 213 casters is
// 4.3 KB, written with `vkCmdUpdateBuffer` inline in the command buffer.
//
// **And because it is rewritten every bake frame, it is a RING** - the same defect §4.66 hit head
// on, one section earlier and one object over. The write and the `vkCmdDrawIndexedIndirect` that
// reads it are in the same command buffer, so with frames in flight a bake that spans more than
// one frame has frame N+1's transfer landing on bytes frame N's indirect draws are still reading,
// and a draw picks up a half-written `indexCount`. A single-frame bake never sees it - which is
// why this was latent rather than observed: at `map_shadow_rate` 256 level02's 51 lights write
// once, where level01's 686 take three frames and race exactly like this.
constexpr uint32_t kMaxMapCasters = 2048;
constexpr uint32_t kMapIndirectStride = 20; // sizeof(VkDrawIndexedIndirectCommand)
constexpr uint32_t kMapParamOffset = kMaxMapCasters * kMapIndirectStride;
static_assert(kMapIndirectStride * kMaxMapCasters <= 65536,
              "vkCmdUpdateBuffer takes at most 64 KB in one call");
static_assert(kMapParamOffset % 16 == 0, "the parameter array is read as uint2 by address");
// Commands then parameters, and the whole thing is one slice of the ring. Four slices against two
// frames in flight, the same margin (and the same 56 KB a slice) the per-frame bake's ring takes.
constexpr uint32_t kMapIndirectSlice = kMapParamOffset + kMaxMapCasters * 8;
constexpr uint32_t kMapIndirectRing = 4;
static_assert(kMapIndirectRing >= 2, "at least as many slices as there are frames in flight");
static_assert(kMapIndirectSlice % 16 == 0,
              "a slice must keep the parameter array's alignment, since it is read by address");
// Advanced once per bake that writes the batch, so consecutive bakes write disjoint regions. It is
// deliberately not the frame index: the local half (§4.65) can bake on a frame the map half does
// not, and what has to be disjoint is consecutive *writes*, not consecutive frames.
uint64_t MapRingSerial = 0;
VkBuffer MapIndirectBuffer = VK_NULL_HANDLE;
VkDeviceMemory MapIndirectMemory = VK_NULL_HANDLE;
uint64_t MapIndirectAddress = 0;
// Whether the bake is issuing one command per face or one draw call per caster per face. Read
// back rather than assumed, because the fallback is a device feature away and produces the same
// atlas - so nothing on screen would ever say which ran.
bool MapShadowIndirect = false;
uint32_t MapShadowCastersDropped = 0; // casters past kMaxMapCasters, or of the wrong index width
uint32_t MapShadowLastCasters = 0;    // how many the last baked slice submitted

// **On since §4.64, and it was play that settled it.** §4.61 left it off because no measurement
// could say whether the picture with these shadows was the right one - the game never had them -
// and then the first report from actually playing was that the map lights "don't cast shadows".
// A feature nobody can see is not a fidelity question, and 0.50 ms on the level with the most map
// lights in the game was never the objection.
// See SetLocalLights in VkDraw.h. A diagnostic, on by default: off drops D3D's point and spot
// lights from the sum, which measures the ceiling on what shadowing them could ever be worth.
bool LocalLightsEnabled = true;
// See SetLocalLightWindow in VkDraw.h. On by default: off restores D3D8's hard cutoff at Range,
// which is what this renderer did before §4.70.
bool LocalLightWindowEnabled = true;

bool MapShadowsEnabled = true;
// In atlas texels at the fragment's own distance from the light. 1.0 is the larger of two knees
// (§4.61): level02's acne is gone by 0.25 and level04's needs about 1, and above 1 the real
// occlusion starts going with it.
float MapShadowBiasValue = 1.0f;
// Lights per frame. **Set from the submission path at atlas creation** - 256 with indirect
// drawing and 4 without - because §4.62 changed what it is for. It used to spread 1.9 seconds of
// bake across frames; with one command a face that 1.9 seconds turns out to have been almost
// entirely draw-call submission, the whole bake is a few milliseconds of GPU work, and level01's
// 682 lights land in three frames nobody can see. 256 rather than the whole set so that a mod
// with far more lights than any shipped level is still bounded.
int MapShadowRateValue = 4;

// See SetLocalShadows in VkDraw.h. Whether D3D's own point and spot lights sample the atlas -
// independent of MapShadowsEnabled, because they are a different light system sharing one image.
//
// **`GKPLUS_VK_LOCAL_SHADOWS=0` is the launch-time form, and it exists for a specific reason**:
// `render.local_shadows` is reachable only through the REPL, and the REPL is reachable only from a
// running game on a usable display. A GPU feature that is suspected of wedging the display cannot
// be switched off by the one instrument that needs the display to work, so it needs a switch that
// is decided before the device exists. Read once, lazily - `DllMain` is far too early to ask the
// environment anything.
bool LocalShadowsWanted = true;
bool LocalShadowsRead = false;

bool LocalShadowsEnabled() {
  if (!LocalShadowsRead) {
    LocalShadowsRead = true;
    char value[16] = {};
    const DWORD len = ::GetEnvironmentVariableA("GKPLUS_VK_LOCAL_SHADOWS", value, sizeof(value));
    if (len > 0 && len < sizeof(value)) {
      const std::string text(value, len);
      LocalShadowsWanted = !(text == "0" || text == "off" || text == "no");
    }
  }
  return LocalShadowsWanted;
}

// --- the local half of that atlas (§4.65) ------------------------------------------------------
//
// D3D's point and spot lights, keyed on their occlusion geometry. Everything here exists because
// **a D3D light has no identity across frames**: `SetLight` reuses indices, and a `GpuLight` is
// deduplicated by enable mask within one frame and thrown away with the frame's scratch. The key
// is the identity, and the stability gate below is what keeps that honest for a light that moves.

// How many consecutive frames a key must survive before it claims a slot. **The whole handling of
// moving lights**, and it costs nothing: a light on a track or a mod's light on a projectile makes
// a new key every frame, so no key ever reaches the threshold, no slot is claimed and no cube is
// baked. It is unshadowed, which is the state every D3D light was in before this existed.
//
// Four, because the cost of being wrong is asymmetric: a static light waits four frames for its
// shadow at level start and nobody sees it, where a light that stutters for four frames and then
// stops would otherwise re-bake six faces for nothing.
constexpr uint32_t kLocalShadowStableFrames = 4;

struct LocalShadowEntry {
  int32_t slot = -1;       // its slot in the LOCAL range, or -1 while it holds none
  uint64_t last_seen = 0;  // the frame it was last asked for
  uint32_t stable = 0;     // distinct frames it has been asked for in a row
  bool baked = false;      // its cube is in the atlas, so it may be sampled
  LocalShadowKey key;
};

std::map<LocalShadowKey, LocalShadowEntry> LocalShadowKeys;
// Which key owns each local slot, so an eviction can find the entry to take it from. A pointer
// into a `std::map` node, which never moves - the one property that makes this safe.
const LocalShadowKey *LocalShadowOwner[kLocalShadowSlots] = {};
// Slots whose cube still has to be rendered. Drained by the bake, a few a frame like the map's.
std::vector<uint32_t> LocalShadowPending;
uint32_t LocalShadowBuiltForGeneration = 0;
uint64_t LocalShadowFrame = 0;      // the frame the current pass is resolving lights for
// Keys dropped for going stale - a light that moved away from its own contents and never came
// back. **Cumulative, and it counts KEYS rather than calls**, which is the distinction that makes
// it readable: `AcquireLocalShadowSlot` runs once per distinct light run per frame, so anything
// counted per call reports the frame rate rather than the fact. The first draft of this counted
// refusals per call and read 47,759 for seventeen unslotted lights.
uint32_t LocalShadowForgotten = 0;
uint64_t LocalShadowBakes = 0; // cubes rendered for this level, evictions included

// Light index -> atlas slot, and its inverse. Rebuilt whenever the level's light set changes,
// which is the same test the light grid uses.
std::vector<int32_t> MapShadowSlotForLight;
std::vector<uint32_t> MapShadowLightForSlot;
// The atlas is cleared **once per level**, by whichever bake gets there first. A bool rather than
// `cursor == 0`, because two independent producers write into one image now: with the map half
// switched off the local half would otherwise never clear, and with it on a local slice arriving
// first would clear the map tiles out from under it.
//
// **Anything that sets this back to false owes BOTH halves a re-bake**, because the clear takes the
// whole 4096x4096 image and a tile nobody re-queues keeps the clear value - depth 1, "nothing
// occludes" - for the rest of the level. `MapShadowCursor = 0` is the map half's re-queue and
// `RequeueLocalShadows()` is the local half's; the second was missing, and §4.62 records what that
// cost.
bool MapShadowAtlasCleared = false;
// 0 is "nothing baked yet" and MapLightsGeneration() never returns it for a loaded set, so a
// fresh process re-bakes rather than trusting an empty table.
uint32_t MapShadowBuiltForGeneration = 0;
uint32_t MapShadowCursor = 0;   // the next slot to bake; == size() when the bake is finished
uint32_t MapShadowRefused = 0;  // lights the atlas had no room for
uint64_t MapShadowDraws = 0;    // draw calls the bake has issued for this level

// The six cube faces, as an orthonormal basis each. **This table is duplicated in world.slang and
// the two must agree exactly** - the bake rasterises with it and the lookup projects with it, so a
// disagreement is a shadow that lands on the wrong face rather than anything that looks like a
// bug. Forward is the face's axis; right and up are chosen so that `u = (d.R)/(d.F)` and
// `v = (d.U)/(d.F)` both land in -1..1 across the face.
const float kFaceForward[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
const float kFaceRight[6][3] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
const float kFaceUp[6][3] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};

// The near plane, as a fraction of the light's range. It is a compromise between depth precision
// and how close geometry may be: a standard perspective spends its depth near the near plane, so
// the resolvable distance at the far plane is `range / (65536 * fraction)` - 0.1 world units at
// range 111 with this value - while geometry closer to the light than `range * fraction` is
// clipped and stops occluding. **Also duplicated in world.slang.**
constexpr float kMapShadowNear = 1.0f / 64.0f;

// --- the per-frame atlas (§4.66) ---------------------------------------------------------------
//
// Everything §4.65 could not do, and it does it by giving up the thing that made §4.65 cheap: this
// one is rebuilt from nothing every frame. No key held across frames, no stability gate and no
// eviction - so a light that moves is not a special case, it is the only case.

VkImage DynShadowImage[kDynShadowRing] = {};
VkDeviceMemory DynShadowMemory[kDynShadowRing] = {};
VkImageView DynShadowView[kDynShadowRing] = {};
VkDeviceSize DynShadowBytes = 0; // per slice
// **Its own pipelines, not `MapShadowPipeline`.** The three want the same format, but
// `render.map_shadow_indirect` rebuilds that one to switch entry points (§4.62) - so sharing would
// silently change which entry point this bake ran the moment that knob was touched. Both are built
// up front rather than on demand, which is what makes `render.dynamic_shadow_indirect` a knob that
// can be flipped mid-level without a pipeline compile inside a frame.
VkPipeline DynShadowPipeline = VK_NULL_HANDLE;       // `map_shadow_vertex`, one batch per bucket
VkPipeline DynShadowDirectPipeline = VK_NULL_HANDLE; // `shadow_vertex`, one draw per caster
// §4.71's twins of both. Two axes - submission path, and whether the patch is amplified.
VkPipeline DynShadowPipelineTess = VK_NULL_HANDLE;
VkPipeline DynShadowDirectPipelineTess = VK_NULL_HANDLE;
VkBuffer DynIndirectBuffer = VK_NULL_HANDLE;
VkDeviceMemory DynIndirectMemory = VK_NULL_HANDLE;
uint64_t DynIndirectAddress = 0;
// **The batch is a RING, and that is not an optimisation.**
//
// This bake rewrites its commands every frame with `vkCmdUpdateBuffer` and reads them back with
// `vkCmdDrawIndexedIndirect` in the same command buffer. With frames in flight, frame N+1's
// transfer executes while frame N's indirect draws are still reading the same bytes - so a draw
// picks up a half-written `indexCount`, which is the classic way to lose a device on an indirect
// draw.
//
// **It was never observed doing so, and the comment here used to say it was.** The device loss
// this ring was written to fix was §4.67's field permutation (see `DynamicShadowsEnabled` below),
// and adding the ring left that failure completely unchanged - which was recorded at the time and
// read as "the hazard was real but not the one biting". Only the second half of that is
// established: the hazard is real by construction and the ring is kept for it, but nothing here
// has ever seen it fire. Do not cite it as measured.
//
// §4.61's map bake has the same shape and got away with it by accident: at `map_shadow_rate` 256
// a level of 51 lights bakes in one frame, so there is only ever one write. Level01's 686 take
// three frames and raced exactly like this - which is where this ring came from, and it now has
// one of its own (`kMapIndirectRing`).
//
// Four slices against two frames in flight, because the cost is 224 KB a slice and the failure is
// a hung GPU.
// **The batch is per (light, face, bucket), not per caster** - which is what culling costs and
// what it buys. `vkCmdDrawIndexedIndirect` reads a CONTIGUOUS run of commands, and the survivors
// of a cull are a different subset on every face, so a caster that survives on four of a light's
// six faces is emitted four times. Unculled, that would be casters x faces: 304 x 54 = 16,416 on
// the frame this was written against. Culled, it is the number that actually reaches an atlas
// tile.
//
// The cap is `kShadowMaxCommands`, shared with the sun's pass, and a frame that hits it is one
// where the cull found nothing to remove - so `dynamic_shadow_report` states the drop outright
// rather than leaving it to be read off a frame time. It is not sized to hold the unculled worst
// case on purpose: a batch that large is the thing this exists to prevent.
constexpr uint32_t kDynMaxCommands = kShadowMaxCommands;
constexpr uint32_t kDynParamOffset = kShadowParamOffset;
constexpr uint32_t kDynIndirectSlice = kShadowIndirectSlice;
constexpr uint32_t kDynIndirectRing = kShadowIndirectRing;
// One counter for both rings, advanced once per bake: the batch's slice is `% kDynIndirectRing`
// and the atlas image's is `% kDynShadowRing`. Two rings rather than one depth because they guard
// different hazards - the batch against an in-flight *indirect read*, the image against an
// in-flight *sample* - and only the second is bounded by frames in flight.
uint64_t DynRingSerial = 0;
uint32_t DynImageSlot = 0; // which atlas slice the last bake wrote, for the frame block to publish
// A bisect, not a design decision: restrict the caster set to arena-sourced draws, so the
// user-pointer half (§4.18) can be taken out of the pass. `render.dynamic_shadow_arena_only`.
// **It is not the test for "is a unit a caster"** - a unit draws from the arena as often as not
// (level02's fires: 154 casters, one bucket, all arena), so this separates by *storage* and not by
// what a caster is. `map_only` below is that test.
bool DynCasterArenaOnly = false;
// `render.dynamic_shadow_map_only` - restrict the caster set to `IsMapGeometry`, which is §4.65's
// exactly. The A/B against it is what the props and the units are worth, and it is the measurement
// `arena_only` cannot make: the two tests differ in what a caster *is*, not in where it is stored.
bool DynCasterMapOnly = false;
// Bake the atlas but never advertise it, so the world pass cannot sample it.
// `render.dynamic_shadow_sample`.
//
// **This knob was broken for the whole of the section it was written for, and its clean negative
// result is what sent §4.66 chasing the bake.** It clears `dyn_shadow_texture` - and under §4.67's
// field permutation the shader read *that* word as `light_flags` and took its own
// `dyn_shadow_texture` from `dyn_shadow_sampler`, which is never `kNoTexture`. So the gate it aims
// at passed every frame and the sampling never stopped. A bisect knob is code, and it can be
// disabled by the defect it is bisecting for; the guard is to check it changes something
// observable before believing what it says.
bool DynShadowSample = true;
bool DynShadowReady = false;
// The bisect caps (§4.66). 0 is "no cap" on all three, which is the shipped configuration; they
// exist to walk the bake DOWN to something that survives, since a capture of the failing frame
// cannot be taken. Applied at submission and not at collection, so the report and the range check
// keep describing the whole set.
int DynMaxLights = 0;
int DynMaxFaces = 0;
int DynMaxCasters = 0;
// One `vkCmdDrawIndexed` per caster per face instead of one indirect batch per bucket per face -
// the fallback §4.61's map bake keeps for a device without `multiDrawIndirect`, and here the bisect
// that splits the indirect machinery from the pass. The direct path touches no indirect buffer, no
// device address for its parameters and no `SV_DrawIndex`.
bool DynShadowIndirect = true;
// **A feature, on by default.** It was off for one section (§4.66) because enabling it took the
// device down - `VK_ERROR_DEVICE_LOST` within about four bakes, reproducibly - and every hypothesis
// in that section was aimed at this bake.
//
// **The bake was never at fault.** It was §4.67's field permutation: `GpuFrameData` had
// `light_flags` in a different place in `src/VkDraw.h` and in `world.slang`, so the fragment
// shader's `dyn_shadow_sampler` read the word the CPU fills with `dyn_shadow_offset` - a *float*,
// `DynShadowBiasValue` 1.0f, which is 0x3f800000 = **1,065,353,216** - and used it to index the
// bindless `samplers[]` array, which holds five. An unbounded descriptor read is a GPU page fault,
// which is a lost device.
//
// Two things about that are worth keeping, because they are why the bake looked guilty for a whole
// section. The shader's gate is `light.position.w >= 0`, the per-frame slot, and
// `RegisterDynamicShadowLight` returns -1 whenever this flag is false - so the control was clean by
// construction and enabling the feature was what armed the fault. And
// `render.dynamic_shadow_sample`, the knob written specifically to split "the bake hangs" from
// "sampling hangs", sets `dyn_shadow_texture`, which under the same permutation was read as
// `light_flags` - so it never stopped the sampling at all, and the measurement that appeared to
// prove "not the sampling" was defeated by the very permutation being hunted.
bool DynamicShadowsEnabled = true;
// In atlas texels at the fragment's distance. Its own knob because the face is 256 texels where
// the static atlas's is 64, so one texel is a quarter of the world distance.
float DynShadowBiasValue = 1.0f;
// The PCF radius for D3D's point and spot lights: 0 a single tap, 1 a 3x3, 2 a 5x5.
// `render.local_shadow_taps`.
//
// **1 by default, and the map lights deliberately stay at 0.** The single tap this replaces was a
// measurement about the STDLIGHT rig, where a fragment is in range of a mean of 11.5 lights
// (§4.54) and the sum does the filtering - so nine taps there would be a hundred a fragment for
// nothing. One or two D3D lights reach a fragment, nothing averages them, and the hard 0/1 compare
// is what play reported as jagged (§4.69). Same body, different callers, so the radius rides in
// `GpuFrameData` rather than in either atlas.
int LocalShadowTapsValue = 1;

// This frame's lights, in registration order; the slot IS the index. Cleared when the frame moves
// on, which is the only bookkeeping the whole design needs.
std::vector<LocalShadowKey> DynLights;
std::map<LocalShadowKey, int32_t> DynLightSlots;
uint64_t DynFrame = UINT64_MAX;      // the frame `DynLights` describes
uint64_t DynBakedFrame = UINT64_MAX; // ... and the last one actually baked
uint32_t DynRefused = 0;             // lights past the 42 slots, last frame
uint32_t DynCasters = 0;             // casters submitted, last frame
uint32_t DynCastersDropped = 0;      // past the cap, or of an index width their bucket cannot take
uint32_t DynBuckets = 0;             // distinct (vertex source, index source, index width) groups
uint64_t DynIndirectCommands = 0;    // cumulative, so the submission cost is visible
// The batch's own arithmetic, checked on the CPU because a capture of this bake cannot exist -
// the device is lost before RenderDoc can write the file.
struct DynSampleEntry {
  uint32_t index_count, first_index, vertex_offset, record, base_vertex, stride;
  bool arena;
};
std::vector<DynSampleEntry> DynSample;
uint64_t DynCalls = 0;          // BakeDynamicShadows entered
uint64_t DynSkipped = 0;        // ... and returned early because nothing was new
uint32_t DynBadRanges = 0;      // commands whose index range runs past the arena
uint64_t DynWorstIndexEnd = 0;  // the furthest byte any command reads, for comparison with it

// --- the cull (see BakeDynamicShadows) --------------------------------------------------------
//
// **A feature, on.** Everything below is counted in *(caster, face)* pairs rather than in casters,
// because that is the unit the bake's cost is actually in: the same caster is a separate piece of
// work on every one of a light's six faces, and what the cull removes is pairs.
bool DynCullEnabled = true;
uint32_t DynFaceSubmits = 0;       // (caster, face) pairs that reached a tile, last bake
uint32_t DynFaceCulledRange = 0;   // ... dropped because the light's sphere does not reach them
uint32_t DynFaceCulledFrustum = 0; // ... dropped because the face's frustum does not contain them
// Casters with no world box at all, which are drawn on every face of every light because there is
// nothing to test them with. **The number to watch**: it is the part of the frame the cull cannot
// see, and it moving is how a change upstream in DrawItem::has_bounds would show up here rather
// than as an unexplained frame time. Counted in casters, not pairs.
uint32_t DynUnbounded = 0;
uint32_t DynCommandsDropped = 0; // (caster, face) pairs past kDynMaxCommands
// Where the unbounded casters actually are, per bucket, because "93 of 171 have no box" is not
// actionable and "the scratch bucket is all of them" is. Rebuilt every bake.
struct DynBucketReport {
  bool vertex_arena = true;
  bool index_arena = true;
  uint32_t stride = 2;
  uint32_t count = 0;
  uint32_t unbounded = 0;
};
std::vector<DynBucketReport> DynBucketReports;

bool CreateMapShadowPipeline();

bool CreateMapShadowAtlas() {
  // D16_UNORM is mandatory for a depth attachment, so this is a query for the sampled bit rather
  // than for the format. Failing it is not fatal - the atlas simply does not exist and every
  // light stays unshadowed, which is the state the build before this was always in.
  VkFormatProperties properties = {};
  vkGetPhysicalDeviceFormatProperties(GetPhysicalDevice(), VK_FORMAT_D16_UNORM, &properties);
  constexpr VkFormatFeatureFlags kNeeded = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                           VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  if ((properties.optimalTilingFeatures & kNeeded) != kNeeded) {
    return Fail("no D16_UNORM depth format for the map shadow atlas");
  }
  MapShadowFormat = VK_FORMAT_D16_UNORM;

  VkImageCreateInfo image = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image.imageType = VK_IMAGE_TYPE_2D;
  image.format = MapShadowFormat;
  image.extent = {kMapShadowAtlas, kMapShadowAtlas, 1};
  image.mipLevels = 1;
  image.arrayLayers = 1;
  image.samples = VK_SAMPLE_COUNT_1_BIT;
  image.tiling = VK_IMAGE_TILING_OPTIMAL;
  image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(GetDevice(), &image, nullptr, &MapShadowImage) != VK_SUCCESS) {
    return Fail("could not create the map shadow atlas");
  }

  VkMemoryRequirements requirements = {};
  vkGetImageMemoryRequirements(GetDevice(), MapShadowImage, &requirements);
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
    return Fail("no device-local memory type for the map shadow atlas");
  }
  MapShadowBytes = requirements.size;
  VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = type;
  if (vkAllocateMemory(GetDevice(), &allocate, nullptr, &MapShadowMemory) != VK_SUCCESS ||
      vkBindImageMemory(GetDevice(), MapShadowImage, MapShadowMemory, 0) != VK_SUCCESS) {
    return Fail("could not back the map shadow atlas");
  }

  // One view: D16_UNORM has no stencil aspect, so the attachment and the sampled view are the
  // same thing - which is the second reason this atlas does not use the sun's format.
  VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = MapShadowImage;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = MapShadowFormat;
  view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  view.subresourceRange.levelCount = 1;
  view.subresourceRange.layerCount = 1;
  if (vkCreateImageView(GetDevice(), &view, nullptr, &MapShadowView) != VK_SUCCESS) {
    return Fail("could not create the map shadow atlas view");
  }
  WriteBindlessView(kMapShadowMapSlot, reinterpret_cast<uint64_t>(MapShadowView));

  // The batch, and the parameters the shader reads beside it. Device-local and never mapped: it
  // is written with `vkCmdUpdateBuffer`, which puts the bytes in the command buffer itself.
  // BUFFER_DEVICE_ADDRESS as well as INDIRECT_BUFFER, because the parameter half is reached by
  // address from the vertex shader rather than bound.
  VkBufferCreateInfo buffer = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer.size = static_cast<VkDeviceSize>(kMapIndirectSlice) * kMapIndirectRing;
  buffer.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(GetDevice(), &buffer, nullptr, &MapIndirectBuffer) != VK_SUCCESS) {
    return Fail("could not create the map shadow indirect buffer");
  }
  VkMemoryRequirements buffer_requirements = {};
  vkGetBufferMemoryRequirements(GetDevice(), MapIndirectBuffer, &buffer_requirements);
  uint32_t buffer_type = UINT32_MAX;
  for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    if ((buffer_requirements.memoryTypeBits & (1u << i)) != 0 &&
        (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
      buffer_type = i;
      break;
    }
  }
  if (buffer_type == UINT32_MAX) {
    return Fail("no device-local memory type for the map shadow indirect buffer");
  }
  VkMemoryAllocateFlagsInfo flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo buffer_allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
  buffer_allocate.allocationSize = buffer_requirements.size;
  buffer_allocate.memoryTypeIndex = buffer_type;
  if (vkAllocateMemory(GetDevice(), &buffer_allocate, nullptr, &MapIndirectMemory) != VK_SUCCESS ||
      vkBindBufferMemory(GetDevice(), MapIndirectBuffer, MapIndirectMemory, 0) != VK_SUCCESS) {
    return Fail("could not back the map shadow indirect buffer");
  }
  VkBufferDeviceAddressInfo address = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  address.buffer = MapIndirectBuffer;
  MapIndirectAddress = vkGetBufferDeviceAddress(GetDevice(), &address);

  // **`multiDrawIndirect` is what decides which path runs**, not the buffer existing: without it
  // `vkCmdDrawIndexedIndirect` is limited to a `drawCount` of 1, which is the same thing as not
  // having it. The fallback issues a draw call per caster per face and produces the same atlas.
  MapShadowIndirect = Caps().multi_draw_indirect;
  MapShadowRateValue = MapShadowIndirect ? 256 : 4;
  // Guarded, because `CreateShadowPass` already creates these for the sun's own indirect
  // pipelines and runs first. Creating them twice would leak the first pair - the teardown holds
  // one handle each - and the second pair would be identical, so nothing would ever show it.
  if (MapShadowModule == VK_NULL_HANDLE) {
    MapShadowModule = CreateModule(kMapShadowVertexSpv, sizeof(kMapShadowVertexSpv));
  }
  if (MapShadowModule == VK_NULL_HANDLE) {
    return Fail("could not create the map shadow shader module");
  }
  if (Caps().tessellation_shader && MapShadowTessVertexModule == VK_NULL_HANDLE) {
    MapShadowTessVertexModule =
        CreateModule(kMapShadowTessVertexSpv, sizeof(kMapShadowTessVertexSpv));
  }
  return CreateMapShadowPipeline();
}

// The bake's pipeline, which is the sun's with two fields changed. Its own function because
// `render.map_shadow_indirect` rebuilds it: the two submission paths differ only in the entry
// point, and having both reachable at run time is what makes "the atlas is the same either way"
// a measurement rather than an assertion (§4.62).
bool CreateMapShadowPipeline() {
  VkPipelineShaderStageCreateInfo stages[3] = {};
  // The two submission paths take different entry points, and so do their tessellated twins - so
  // this is a pair of choices, not one. Getting it wrong would bake the atlas with the direct
  // path's record while the indirect path's parameters were bound.
  VkShaderModule vertex_module = MapShadowIndirect ? MapShadowModule : ShadowModule;
  const char *vertex_entry = MapShadowIndirect ? "map_shadow_vertex" : "shadow_vertex";
  VkShaderModule tess_vertex_module =
      MapShadowIndirect ? MapShadowTessVertexModule : ShadowTessVertexModule;
  const char *tess_vertex_entry =
      MapShadowIndirect ? "map_shadow_tess_vertex" : "shadow_tess_vertex";

  VkPipelineVertexInputStateCreateInfo vertex_input = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineTessellationStateCreateInfo tessellation = {
      VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
  tessellation.patchControlPoints = 3;
  VkPipelineViewportStateCreateInfo viewport = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  // No culling, for §4.58's reason and one more of its own: a terrain mesh is a single-sided
  // surface, so front-face culling - the usual way to hide acne - would leave the ground with no
  // occluder at all rather than with a biased one.
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = VK_TRUE;
  depth.depthWriteEnable = VK_TRUE;
  depth.depthCompareOp = VK_COMPARE_OP_LESS;
  VkPipelineColorBlendStateCreateInfo blend = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;

  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.depthAttachmentFormat = MapShadowFormat;

  VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = ShadowLayout;

  info.stageCount = ShadowStages(stages, vertex_module, vertex_entry, false);
  if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr,
                                &MapShadowPipeline) != VK_SUCCESS) {
    return Fail("could not create the map shadow pipeline");
  }
  // Rebuilt from scratch by `render.map_shadow_indirect`, so the twin is cleared first: this
  // function runs again on that knob, and a stale handle from the previous entry point would bake
  // the atlas through the wrong parameter source.
  if (MapShadowPipelineTess != VK_NULL_HANDLE) {
    vkDestroyPipeline(GetDevice(), MapShadowPipelineTess, nullptr);
    MapShadowPipelineTess = VK_NULL_HANDLE;
  }
  if (ShadowTessAvailable() && tess_vertex_module != VK_NULL_HANDLE) {
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    info.pTessellationState = &tessellation;
    info.stageCount = ShadowStages(stages, tess_vertex_module, tess_vertex_entry, true);
    if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr,
                                  &MapShadowPipelineTess) != VK_SUCCESS) {
      MapShadowPipelineTess = VK_NULL_HANDLE;
    }
  }
  MapShadowReady = true;
  return true;
}

// A device-local allocation of the right memory type, which both atlases and both indirect buffers
// want and which was open-coded four times before this existed.
bool AllocateDeviceLocal(const VkMemoryRequirements &requirements, bool device_address,
                         VkDeviceMemory *out) {
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
    return false;
  }
  VkMemoryAllocateFlagsInfo flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   device_address ? &flags : nullptr};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = type;
  return vkAllocateMemory(GetDevice(), &allocate, nullptr, out) == VK_SUCCESS;
}

// The sun pass's indirect batch (§4.77). Declared before CreateShadowPass and defined here, where
// AllocateDeviceLocal is - the only reason it is not written inline up there.
bool CreateSunIndirectBuffer() {
  if (SunIndirectBuffer != VK_NULL_HANDLE) {
    return true;
  }
  VkBufferCreateInfo buffer = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer.size = static_cast<VkDeviceSize>(kShadowIndirectSlice) * kShadowIndirectRing;
  buffer.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(GetDevice(), &buffer, nullptr, &SunIndirectBuffer) != VK_SUCCESS) {
    SunIndirectBuffer = VK_NULL_HANDLE;
    return false;
  }
  VkMemoryRequirements requirements = {};
  vkGetBufferMemoryRequirements(GetDevice(), SunIndirectBuffer, &requirements);
  if (!AllocateDeviceLocal(requirements, true, &SunIndirectMemory) ||
      vkBindBufferMemory(GetDevice(), SunIndirectBuffer, SunIndirectMemory, 0) != VK_SUCCESS) {
    vkDestroyBuffer(GetDevice(), SunIndirectBuffer, nullptr);
    SunIndirectBuffer = VK_NULL_HANDLE;
    return false;
  }
  VkBufferDeviceAddressInfo address = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  address.buffer = SunIndirectBuffer;
  SunIndirectAddress = vkGetBufferDeviceAddress(GetDevice(), &address);
  return true;
}

// The per-frame atlas. Built beside the static one and gated on the same two things: `D16_UNORM`
// being sampleable, and `multiDrawIndirect`.
//
// **`multiDrawIndirect` is a hard requirement here where it is only a preference there.** The
// static atlas has a direct fallback because it bakes once per level and can afford a draw call
// per caster per face; this one would need one every frame - 9 lights x 6 faces x 180 casters is
// ~9,700 calls a frame, which at §4.62's measured ~2.4 us a call is 23 ms. So without the feature
// the atlas is simply not created and every local light falls back to the static one.
bool CreateDynShadowAtlas() {
  if (!Caps().multi_draw_indirect) {
    DebugWrite("gkplus: no multiDrawIndirect, the per-frame shadow atlas is off\n");
    return false;
  }
  VkFormatProperties properties = {};
  vkGetPhysicalDeviceFormatProperties(GetPhysicalDevice(), VK_FORMAT_D16_UNORM, &properties);
  constexpr VkFormatFeatureFlags kNeeded = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                           VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  if ((properties.optimalTilingFeatures & kNeeded) != kNeeded) {
    return Fail("no D16_UNORM depth format for the per-frame shadow atlas");
  }

  VkImageCreateInfo image = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image.imageType = VK_IMAGE_TYPE_2D;
  image.format = VK_FORMAT_D16_UNORM;
  image.extent = {kDynShadowAtlas, kDynShadowAtlas, 1};
  image.mipLevels = 1;
  image.arrayLayers = 1;
  image.samples = VK_SAMPLE_COUNT_1_BIT;
  image.tiling = VK_IMAGE_TILING_OPTIMAL;
  image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  // One image per ring slice, each with its own bindless slot counting down from
  // `kDynShadowMapSlot`. That is the whole cost of the ring on this side, and it is zero on the
  // shader's: the frame block publishes whichever index the bake last wrote.
  for (uint32_t i = 0; i < kDynShadowRing; ++i) {
    if (vkCreateImage(GetDevice(), &image, nullptr, &DynShadowImage[i]) != VK_SUCCESS) {
      return Fail("could not create the per-frame shadow atlas");
    }
    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(GetDevice(), DynShadowImage[i], &requirements);
    DynShadowBytes = requirements.size;
    if (!AllocateDeviceLocal(requirements, false, &DynShadowMemory[i]) ||
        vkBindImageMemory(GetDevice(), DynShadowImage[i], DynShadowMemory[i], 0) != VK_SUCCESS) {
      return Fail("could not back the per-frame shadow atlas");
    }

    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = DynShadowImage[i];
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_D16_UNORM;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(GetDevice(), &view, nullptr, &DynShadowView[i]) != VK_SUCCESS) {
      return Fail("could not create the per-frame shadow atlas view");
    }
    WriteBindlessView(kDynShadowMapSlot - i, reinterpret_cast<uint64_t>(DynShadowView[i]));
  }

  // **Its own buffer as well as its own pipeline**, because both bakes can run in the same command
  // buffer and `vkCmdUpdateBuffer` records its bytes in order - sharing one would have the map
  // bake's slice overwrite this frame's batch, or the reverse, depending on which ran first.
  VkBufferCreateInfo buffer = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer.size = static_cast<VkDeviceSize>(kDynIndirectSlice) * kDynIndirectRing;
  buffer.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(GetDevice(), &buffer, nullptr, &DynIndirectBuffer) != VK_SUCCESS) {
    return Fail("could not create the per-frame shadow indirect buffer");
  }
  VkMemoryRequirements buffer_requirements = {};
  vkGetBufferMemoryRequirements(GetDevice(), DynIndirectBuffer, &buffer_requirements);
  if (!AllocateDeviceLocal(buffer_requirements, true, &DynIndirectMemory) ||
      vkBindBufferMemory(GetDevice(), DynIndirectBuffer, DynIndirectMemory, 0) != VK_SUCCESS) {
    return Fail("could not back the per-frame shadow indirect buffer");
  }
  VkBufferDeviceAddressInfo address = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  address.buffer = DynIndirectBuffer;
  DynIndirectAddress = vkGetBufferDeviceAddress(GetDevice(), &address);

  VkPipelineShaderStageCreateInfo stages[3] = {};

  VkPipelineVertexInputStateCreateInfo vertex_input = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineTessellationStateCreateInfo tessellation = {
      VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
  tessellation.patchControlPoints = 3;
  VkPipelineViewportStateCreateInfo viewport = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  // No culling, for §4.58's reason: Gunlok's geometry is neither closed nor consistently wound
  // across the map object, its props and its units, so front-face culling would open holes in the
  // caster set rather than hide acne. The bias is the knob.
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = VK_TRUE;
  depth.depthWriteEnable = VK_TRUE;
  depth.depthCompareOp = VK_COMPARE_OP_LESS;
  VkPipelineColorBlendStateCreateInfo blend = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.depthAttachmentFormat = VK_FORMAT_D16_UNORM;

  VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = ShadowLayout;

  info.stageCount = ShadowStages(stages, MapShadowModule, "map_shadow_vertex", false);
  if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr,
                                &DynShadowPipeline) != VK_SUCCESS) {
    return Fail("could not create the per-frame shadow pipeline");
  }
  // The direct path's twin: the same state, the sun's own entry point. `render.dynamic_shadow_
  // indirect` selects between them, which is the bisect that splits the indirect machinery from
  // everything else in the pass.
  info.stageCount = ShadowStages(stages, ShadowModule, "shadow_vertex", false);
  if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr,
                                &DynShadowDirectPipeline) != VK_SUCCESS) {
    return Fail("could not create the per-frame shadow direct pipeline");
  }
  // ...and both again, tessellated (§4.71). Four pipelines for one pass looks like a lot and is
  // the same two axes it already had: which submission path, and whether the patch is amplified.
  if (ShadowTessAvailable() && MapShadowTessVertexModule != VK_NULL_HANDLE &&
      ShadowTessVertexModule != VK_NULL_HANDLE) {
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    info.pTessellationState = &tessellation;
    info.stageCount =
        ShadowStages(stages, MapShadowTessVertexModule, "map_shadow_tess_vertex", true);
    if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr,
                                  &DynShadowPipelineTess) != VK_SUCCESS) {
      DynShadowPipelineTess = VK_NULL_HANDLE;
    }
    info.stageCount = ShadowStages(stages, ShadowTessVertexModule, "shadow_tess_vertex", true);
    if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr,
                                  &DynShadowDirectPipelineTess) != VK_SUCCESS) {
      DynShadowDirectPipelineTess = VK_NULL_HANDLE;
    }
  }
  DynShadowReady = true;
  return true;
}

void DestroyDynShadowAtlas() {
  DynShadowReady = false;
  if (DynShadowPipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(GetDevice(), DynShadowPipeline, nullptr);
    DynShadowPipeline = VK_NULL_HANDLE;
  }
  if (DynShadowPipelineTess != VK_NULL_HANDLE) {
    vkDestroyPipeline(GetDevice(), DynShadowPipelineTess, nullptr);
    DynShadowPipelineTess = VK_NULL_HANDLE;
  }
  if (DynShadowDirectPipelineTess != VK_NULL_HANDLE) {
    vkDestroyPipeline(GetDevice(), DynShadowDirectPipelineTess, nullptr);
    DynShadowDirectPipelineTess = VK_NULL_HANDLE;
  }
  if (DynShadowDirectPipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(GetDevice(), DynShadowDirectPipeline, nullptr);
    DynShadowDirectPipeline = VK_NULL_HANDLE;
  }
  if (DynIndirectBuffer != VK_NULL_HANDLE) {
    vkDestroyBuffer(GetDevice(), DynIndirectBuffer, nullptr);
    DynIndirectBuffer = VK_NULL_HANDLE;
  }
  if (DynIndirectMemory != VK_NULL_HANDLE) {
    vkFreeMemory(GetDevice(), DynIndirectMemory, nullptr);
    DynIndirectMemory = VK_NULL_HANDLE;
  }
  DynIndirectAddress = 0;
  for (uint32_t i = 0; i < kDynShadowRing; ++i) {
    if (DynShadowView[i] != VK_NULL_HANDLE) {
      vkDestroyImageView(GetDevice(), DynShadowView[i], nullptr);
      DynShadowView[i] = VK_NULL_HANDLE;
    }
    if (DynShadowImage[i] != VK_NULL_HANDLE) {
      vkDestroyImage(GetDevice(), DynShadowImage[i], nullptr);
      DynShadowImage[i] = VK_NULL_HANDLE;
    }
    if (DynShadowMemory[i] != VK_NULL_HANDLE) {
      vkFreeMemory(GetDevice(), DynShadowMemory[i], nullptr);
      DynShadowMemory[i] = VK_NULL_HANDLE;
    }
  }
}

void DestroyMapShadowAtlas() {
  MapShadowReady = false;
  if (MapShadowPipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(GetDevice(), MapShadowPipeline, nullptr);
    MapShadowPipeline = VK_NULL_HANDLE;
  }
  if (MapShadowPipelineTess != VK_NULL_HANDLE) {
    vkDestroyPipeline(GetDevice(), MapShadowPipelineTess, nullptr);
    MapShadowPipelineTess = VK_NULL_HANDLE;
  }
  if (MapShadowModule != VK_NULL_HANDLE) {
    vkDestroyShaderModule(GetDevice(), MapShadowModule, nullptr);
    MapShadowModule = VK_NULL_HANDLE;
  }
  // §4.71's modules. Destroyed beside the ones they twin rather than in a block of their own, so
  // a subsystem torn down without the others cannot leave one behind.
  for (VkShaderModule *module : {&ShadowTessVertexModule, &MapShadowTessVertexModule,
                                 &ShadowHullModule, &ShadowDomainModule}) {
    if (*module != VK_NULL_HANDLE) {
      vkDestroyShaderModule(GetDevice(), *module, nullptr);
      *module = VK_NULL_HANDLE;
    }
  }
  if (MapIndirectBuffer != VK_NULL_HANDLE) {
    vkDestroyBuffer(GetDevice(), MapIndirectBuffer, nullptr);
    MapIndirectBuffer = VK_NULL_HANDLE;
  }
  if (MapIndirectMemory != VK_NULL_HANDLE) {
    vkFreeMemory(GetDevice(), MapIndirectMemory, nullptr);
    MapIndirectMemory = VK_NULL_HANDLE;
  }
  MapIndirectAddress = 0;
  if (MapShadowView != VK_NULL_HANDLE) {
    vkDestroyImageView(GetDevice(), MapShadowView, nullptr);
    MapShadowView = VK_NULL_HANDLE;
  }
  if (MapShadowImage != VK_NULL_HANDLE) {
    vkDestroyImage(GetDevice(), MapShadowImage, nullptr);
    MapShadowImage = VK_NULL_HANDLE;
  }
  if (MapShadowMemory != VK_NULL_HANDLE) {
    vkFreeMemory(GetDevice(), MapShadowMemory, nullptr);
    MapShadowMemory = VK_NULL_HANDLE;
  }
}

void DestroyShadowPass() {
  ShadowReady = false;
  if (ShadowPipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(GetDevice(), ShadowPipeline, nullptr);
    ShadowPipeline = VK_NULL_HANDLE;
  }
  if (ShadowPipelineTess != VK_NULL_HANDLE) {
    vkDestroyPipeline(GetDevice(), ShadowPipelineTess, nullptr);
    ShadowPipelineTess = VK_NULL_HANDLE;
  }
  if (ShadowPipelineIndirect != VK_NULL_HANDLE) {
    vkDestroyPipeline(GetDevice(), ShadowPipelineIndirect, nullptr);
    ShadowPipelineIndirect = VK_NULL_HANDLE;
  }
  if (ShadowPipelineIndirectTess != VK_NULL_HANDLE) {
    vkDestroyPipeline(GetDevice(), ShadowPipelineIndirectTess, nullptr);
    ShadowPipelineIndirectTess = VK_NULL_HANDLE;
  }
  if (SunIndirectBuffer != VK_NULL_HANDLE) {
    vkDestroyBuffer(GetDevice(), SunIndirectBuffer, nullptr);
    SunIndirectBuffer = VK_NULL_HANDLE;
  }
  if (SunIndirectMemory != VK_NULL_HANDLE) {
    vkFreeMemory(GetDevice(), SunIndirectMemory, nullptr);
    SunIndirectMemory = VK_NULL_HANDLE;
  }
  SunIndirectAddress = 0;
  if (ShadowLayout != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(GetDevice(), ShadowLayout, nullptr);
    ShadowLayout = VK_NULL_HANDLE;
  }
  if (ShadowModule != VK_NULL_HANDLE) {
    vkDestroyShaderModule(GetDevice(), ShadowModule, nullptr);
    ShadowModule = VK_NULL_HANDLE;
  }
  if (ShadowSampleView != VK_NULL_HANDLE) {
    vkDestroyImageView(GetDevice(), ShadowSampleView, nullptr);
    ShadowSampleView = VK_NULL_HANDLE;
  }
  if (ShadowAttachmentView != VK_NULL_HANDLE) {
    vkDestroyImageView(GetDevice(), ShadowAttachmentView, nullptr);
    ShadowAttachmentView = VK_NULL_HANDLE;
  }
  if (ShadowImage != VK_NULL_HANDLE) {
    vkDestroyImage(GetDevice(), ShadowImage, nullptr);
    ShadowImage = VK_NULL_HANDLE;
  }
  if (ShadowMemory != VK_NULL_HANDLE) {
    vkFreeMemory(GetDevice(), ShadowMemory, nullptr);
    ShadowMemory = VK_NULL_HANDLE;
  }
}

// --- the light grid ---------------------------------------------------------------------------
//
// One compute dispatch per LEVEL, not per frame: the map's lights are static in world space, so
// the grid they bin into is too. See the header of src/shaders/lightgrid.slang for why this is a
// world-space grid rather than the view-space cluster grid a screen-tiled renderer would use.

// 32 x 16 x 32 over the map's own bounds. The y axis gets half the resolution because a level is
// far flatter than it is wide - level01 spans 98 x 58 x 237 world units - so equal counts would
// make the vertical cells much thinner than they need to be.
constexpr uint32_t kGridX = 32;
constexpr uint32_t kGridY = 16;
constexpr uint32_t kGridZ = 32;
constexpr uint32_t kGridCells = kGridX * kGridY * kGridZ;
// [0] allocator, [1..3] dims, [4..6] grid origin, [7..9] cell size, [10..11] pad. 12 words keeps
// the cells 16-byte aligned and carries what the FRAGMENT shader needs - which is why it is not
// just the allocator: those three vectors would otherwise be 48 bytes of push constant the block
// does not have.
constexpr uint32_t kGridHeaderWords = 12;
// 512K entries. level01's 686 lights against 16,384 cells would have to average 32 lights a cell
// to exhaust it, where the measured mean is far lower - and the shader drops a whole cell rather
// than half-filling one if it ever does, so exhaustion is visible instead of plausible.
constexpr uint32_t kGridIndexCapacity = 512u * 1024u;

// Must match GridPush in src/shaders/lightgrid.slang.
struct LightGridPush {
  float grid_min[4];
  float cell_size[4];
  uint32_t dims[4]; // xyz cells, w = light count
  uint32_t index_capacity;
  uint32_t pad0;
  uint32_t pad1;
  uint32_t pad2;
};
static_assert(sizeof(LightGridPush) == 64);

VkDescriptorSetLayout GridSetLayout = VK_NULL_HANDLE;
VkDescriptorPool GridPool = VK_NULL_HANDLE;
VkDescriptorSet GridSet = VK_NULL_HANDLE;
VkPipelineLayout GridLayout = VK_NULL_HANDLE;
VkPipeline GridPipeline = VK_NULL_HANDLE;
VkShaderModule GridModule = VK_NULL_HANDLE;

bool CreateLightGridPipeline() {
  GridModule = CreateModule(kBuildGridSpv, sizeof(kBuildGridSpv));
  if (GridModule == VK_NULL_HANDLE) {
    return Fail("could not create the light grid shader module");
  }
  // Three plain storage buffers, bound rather than reached by address. The graphics side reads
  // the same two by device address, which is the shape the bindless set forces on it - but a
  // compute shader that WRITES is the one place a binding is simpler than arguing about pointer
  // semantics, and this layout is its own, so it costs the graphics path nothing.
  VkDescriptorSetLayoutBinding bindings[3] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_info = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 3;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(GetDevice(), &set_info, nullptr, &GridSetLayout) !=
      VK_SUCCESS) {
    return Fail("could not create the light grid set layout");
  }

  VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
  VkDescriptorPoolCreateInfo pool = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = 1;
  pool.poolSizeCount = 1;
  pool.pPoolSizes = &size;
  if (vkCreateDescriptorPool(GetDevice(), &pool, nullptr, &GridPool) != VK_SUCCESS) {
    return Fail("could not create the light grid descriptor pool");
  }
  VkDescriptorSetAllocateInfo allocate = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = GridPool;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &GridSetLayout;
  if (vkAllocateDescriptorSets(GetDevice(), &allocate, &GridSet) != VK_SUCCESS) {
    return Fail("could not allocate the light grid descriptor set");
  }

  VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(LightGridPush)};
  VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1;
  layout.pSetLayouts = &GridSetLayout;
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &range;
  if (vkCreatePipelineLayout(GetDevice(), &layout, nullptr, &GridLayout) != VK_SUCCESS) {
    return Fail("could not create the light grid pipeline layout");
  }

  VkComputePipelineCreateInfo info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  info.stage.module = GridModule;
  info.stage.pName = "build_grid";
  info.layout = GridLayout;
  if (vkCreateComputePipelines(GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr, &GridPipeline) !=
      VK_SUCCESS) {
    return Fail("could not create the light grid pipeline");
  }
  return CreateLightGrid((kGridHeaderWords + 2u * kGridCells) * sizeof(uint32_t),
                         kGridIndexCapacity * sizeof(uint32_t));
}

void DestroyLightGridPipeline() {
  DestroyLightGrid();
  if (GridPipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(GetDevice(), GridPipeline, nullptr);
    GridPipeline = VK_NULL_HANDLE;
  }
  if (GridLayout != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(GetDevice(), GridLayout, nullptr);
    GridLayout = VK_NULL_HANDLE;
  }
  if (GridPool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(GetDevice(), GridPool, nullptr);
    GridPool = VK_NULL_HANDLE;
    GridSet = VK_NULL_HANDLE;
  }
  if (GridSetLayout != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(GetDevice(), GridSetLayout, nullptr);
    GridSetLayout = VK_NULL_HANDLE;
  }
  if (GridModule != VK_NULL_HANDLE) {
    vkDestroyShaderModule(GetDevice(), GridModule, nullptr);
    GridModule = VK_NULL_HANDLE;
  }
}

// Must match `AoPush` in src/shaders/ao.slang. One block for both of that file's passes, exactly as
// ShadowPushConstants is one block for both of shadow.slang's entry points and for the same reason:
// a pipeline layout is per pass, and two blocks would be two layouts for no gain.
//
// **Here, and not with the rest of the AO section below**, for the reason the comment under it
// gives: the generated asserts are included at one point in this file and every push block has to
// be in scope there. The three that came before it are declared beside their own passes only
// because those passes happen to be above this line.
struct AoPushConstants {
  uint64_t vertices;
  uint64_t draws;
  uint32_t record;      // the prepass only
  uint32_t base_vertex; // ... and so is this
  uint32_t position_texture;
  uint32_t normal_texture;
  float radius;
  float screen_radius;
  float bias;
  float strength;
  uint32_t taps;
  float pad0;
};
static_assert(sizeof(AoPushConstants) == 56);

// The tonemap pass's block, here for exactly the reason the one above it is. Every field is a
// 4-byte scalar on purpose, so the scalar and std430 layouts agree and `src/gen-shader-abi.py`
// can check the pair - a `uint2` for the two extents would align to 8 under one rule and 4 under
// the other, and the generator refuses a struct whose two layouts disagree.
struct TonemapPushConstants {
  uint32_t source_texture;
  uint32_t op;
  uint32_t flags;
  uint32_t source_width;
  uint32_t source_height;
  uint32_t dest_width;
  uint32_t dest_height;
  float exposure;
  float knee;
  float white;
};
static_assert(sizeof(TonemapPushConstants) == 40);

// **Every struct this file shares with a shader, checked against the shader's own declaration.**
// Here rather than beside each struct because this is the one point where all three sources are in
// scope - `src/VkDraw.h`, `src/VertexFormat.h`, and the four push blocks above, which are
// file-local. Generated by `src/gen-shader-abi.py`; see §4.67 for the two sections it cost to not
// have it. Angle brackets for the same reason as `<Shaders.gen.inc.h>` at the top of this file.
#include <ShaderAbi.gen.inc.h>

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

// See SetPerPixelLighting in VkDraw.h. On by default, and `GKPLUS_VK_PER_PIXEL_LIGHTING=0` is the
// launch-time override - read once, lazily, because DllMain is far too early to be asking the
// environment anything and this is first needed at the first draw.
bool PerPixelLightingWanted = true;
bool PerPixelLightingRead = false;

// See SetMapLighting in VkDraw.h. **On** since §4.60, which took the frame-time reading §4.56 said
// was missing: with the grid it costs 1.83 ms on level01 - the level with the most map lights in
// the game, 686 - and nothing measurable on level02, level04 or level05. Without the grid the same
// level costs 30 ms, which is what it was off for.
bool MapLightingEnabled = true;
bool MapLightingAllEnabled = false;
// The mean of the fitted gains on the three levels where the model actually holds. Not an
// identity and not a taste: on level04 the on-screen difference from the bake minimises at
// exactly the value the offline fit put it at (§4.55).
//
// **It moved with §4.64's windowed tail**, and had to: a dimmer tail refits to a brighter gain.
// The three are now 1.1 on level01 and 1.5 on level04 and level05, a mean of 1.37, against 0.9 /
// 1.35 / 1.35 and a mean of 1.2 before. 1.35 is that rounded to the sweep's own step.
float MapLightGainValue = 1.35f;
// Refilled once a frame in RecordDraws, before any draw is recorded.
uint64_t FrameMapLightAddress = 0;
uint32_t FrameMapLightCount = 0;
float FrameMapAmbience = 0.0f;
// What the grid was last built for. The light set is identified by its address and count, which
// change together whenever MapLights() reloads - so a level change rebuilds and nothing else does.
uint64_t GridBuiltForAddress = 0;
uint64_t FrameMapLightByteOffset = 0;
// Where this frame's GpuFrameData sits. Written once at the top of RecordDraws.
uint64_t FrameDataAddress = 0;
// The same block with `colour_flags` cleared, for the 2D pass - see the end of UploadFrameData.
// Equal to `FrameDataAddress` when the second allocation failed, which costs the UI layer its
// colour-space exemption and nothing else.
uint64_t UiFrameDataAddress = 0;
uint32_t GridBuiltForCount = 0;
bool GridBuiltForCullOn = false;
float GridMin[3] = {};
float GridCell[3] = {};
bool GridValid = false;
// See SetMapLightCull in VkDraw.h. On by default: without it the fragment loops every light in
// the level, which is what phase 3b had to do and why it could not be on by default.
bool MapLightCullEnabled = true;

bool PerPixelLightingEnabled() {
  if (!PerPixelLightingRead) {
    PerPixelLightingRead = true;
    char value[16] = {};
    const DWORD len =
        ::GetEnvironmentVariableA("GKPLUS_VK_PER_PIXEL_LIGHTING", value, sizeof(value));
    if (len > 0 && len < sizeof(value)) {
      const std::string text(value, len);
      PerPixelLightingWanted = !(text == "0" || text == "off" || text == "no");
    }
  }
  return PerPixelLightingWanted;
}

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
    // A lighting map's own name contains the name of the texture it belongs to, so without this
    // `"lava"` would match `bitmaps\lava lighting.dds` as readily as `bitmaps\lava.rim` - and
    // whichever came first would win. They are not textures a mod addresses; see
    // IsLightingImage in VkLighting.h.
    if (IsLightingImage(image.index)) {
      continue;
    }
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
    if (name.empty() || IsLightingImage(image.index)) {
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

  // The lighting map, keyed on the stage-0 texture as it stands NOW - after an override may have
  // replaced it, which is the opposite of how the tint is keyed and is deliberate. A replacement
  // texture is a different surface, and the map that belongs to it is its own; keying on the
  // original would give a retextured wall the old wall's bumps.
  material.lighting_texture = LightingMapFor(material.stage0_texture);
  if (material.lighting_texture != kNoTexture) {
    ++MutableLightingCounters().materials_lit;
  }

  // Gunlok's chrome pass, recognised by what stage 1 samples. Keyed on the stage as it stands now
  // for the same reason the lighting map is: a `render.material_override` that replaced the sphere
  // map asked for that surface to stop being a reflection, and one that replaced something else
  // *with* the sphere map asked for the opposite. Either way the question is what is sampled, not
  // what the game named.
  //
  // Only meaningful with a lighting map on stage 0 - everything it gates is derived from one - so
  // it is not set without one, which keeps a reflective unit that ships no map interning exactly
  // as it did before this existed.
  if (material.stage_count == 2 && material.lighting_texture != kNoTexture &&
      IsChromeTexture(material.stage1_texture)) {
    material.chrome = 1;
    ++MutableLightingCounters().chrome_draws;
    // The reflect stage asks for D3DTEXF_NONE, which AcquireSampler reproduces as a maxLod clamp -
    // so `chrome_blur` cannot reach a single mip through the game's own sampler. See
    // MippedSamplerFor in VkResources.h; it is a no-op unless the blur is switched on.
    if (LightingParams().chrome_blur > 0.0f) {
      material.stage1_sampler = MippedSamplerFor(material.stage1_sampler);
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

// --- screen-space ambient occlusion (§4.86) -----------------------------------------------------
//
// Two passes, and the first one is the reason this is affordable at all. `RecordShadowPass` walks
// the frame's caster list from the sun's point of view; this walks the SAME list, through the same
// `CollectCasters`, from the camera's - and it needs no camera matrix to do it, because each draw's
// own `mvp` already is world x view x projection. The whole "the frustum is not available as a
// per-frame quantity on this side" problem BuildSunCascades works around simply does not arise.
//
// What the prepass writes is a **world position** per pixel rather than a depth, so the resolve
// reconstructs nothing and inverts nothing. See src/shaders/ao.slang for the technique itself and
// why its kernel is fixed rather than randomised - the short version is that a fixed kernel needs
// no blur pass, and a blur at 640x480 is a large fraction of a character.
//
// Three images at the render extent, all recreated on resize with the depth buffer:
//
//   position  R32G32B32A32_SFLOAT   4.9 MB at 640x480 - xyz world, w=1 where a pixel was covered
//   normal    R8G8B8A8_UNORM        1.2 MB - biased into [0,1], 8 bits is ample for a count
//   result    R8_UNORM              0.3 MB - what the world shader multiplies by
//
// plus a depth buffer of its own (1.2 MB) rather than a share of the world pass's. Sharing would
// work and would save the megabyte, but it would couple this pass's ordering to the world pass's
// clear, and the world pass's depth image is deliberately not SAMPLED and deliberately
// STORE_OP_DONT_CARE - two decisions this feature has no business reopening.

constexpr VkFormat kAoPositionFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkFormat kAoNormalFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kAoResultFormat = VK_FORMAT_R8_UNORM;

// GpuFrameData::ao_flags - must match world.slang.
constexpr uint32_t kAoMapOnly = 0x1u;
constexpr uint32_t kAoDebug = 0x2u;

// `AoPushConstants` is declared **above `ShaderAbi.gen.inc.h`** rather than here, beside the rest
// of this section, because the generated asserts are included at one point in this file and every
// push block has to be in scope there.

bool AoReady = false;
// **Off by default.** It is a fidelity call rather than a cost one, in the same class as
// `render.map_shadows` and `render.tessellation`: the game never had ambient occlusion, so there
// is no reference frame to be right against and nothing here can be measured as "closer to D3D8".
// Off also keeps the frame bit-identical to the build before this existed, which is the A/B every
// other feature here is judged by.
bool AoEnabled = false;
bool AoMapOnlyEnabled = true;
bool AoDebugEnabled = false;
// The defaults, and what each is: see SetAoRadius and friends in VkDraw.h.
// **A sweep, not a guess** (§4.86). At level02's settled camera the occluded fraction of the
// debug view goes 0.341 / 0.384 / 0.412 / 0.417 for a radius of 0.75 / 1.5 / 3 / 6, so 3 is the
// knee: past it the disc is the binding constraint and the hemisphere has stopped rejecting
// anything, while the picture keeps darkening until a character self-occludes over its whole
// body.
float AoRadiusValue = 3.0f;
// **A fraction of the frame's HEIGHT, not a pixel count.** The shader wants pixels and gets them,
// but the knob cannot be in pixels: Gunlok's render extent is 640x480 on the machine every number
// in the notes came from and 3072x1728 on the one this was written on, so a pixel default is
// wrong by 3.6x on one of them - and wrong in the direction that makes the feature invisible
// rather than obviously broken. 0.07 is 34 pixels at 480 lines and 121 at 1728.
//
// It costs the technique nothing. What the fixed kernel buys is that every pixel of a frame walks
// the same pattern; a value derived once per frame from the target size is still one constant.
float AoScreenRadiusValue = 0.07f;
float AoBiasValue = 0.05f;
float AoStrengthValue = 1.0f;
float AoDirectValue = 0.0f;
// **The whole disc by default.** Under-sampling a fixed kernel does not produce noise, it produces
// visible copies of every occluder's silhouette - so the tap count is not a quality dial with a
// cheap end, it is the difference between AO and ghosting. See kDisc in src/shaders/ao.slang.
uint32_t AoTapsValue = 64;

VkImage AoPositionImage = VK_NULL_HANDLE;
VkImage AoNormalImage = VK_NULL_HANDLE;
VkImage AoResultImage = VK_NULL_HANDLE;
VkImage AoDepthImage = VK_NULL_HANDLE;
VkDeviceMemory AoPositionMemory = VK_NULL_HANDLE;
VkDeviceMemory AoNormalMemory = VK_NULL_HANDLE;
VkDeviceMemory AoResultMemory = VK_NULL_HANDLE;
VkDeviceMemory AoDepthMemory = VK_NULL_HANDLE;
VkImageView AoPositionView = VK_NULL_HANDLE;
VkImageView AoNormalView = VK_NULL_HANDLE;
VkImageView AoResultView = VK_NULL_HANDLE;
VkImageView AoDepthView = VK_NULL_HANDLE;
VkPipelineLayout AoLayout = VK_NULL_HANDLE;
VkPipeline AoPrepassPipeline = VK_NULL_HANDLE;
VkPipeline AoResolvePipeline = VK_NULL_HANDLE;
VkShaderModule AoPrepassVertexModule = VK_NULL_HANDLE;
VkShaderModule AoPrepassFragmentModule = VK_NULL_HANDLE;
VkShaderModule AoFullscreenVertexModule = VK_NULL_HANDLE;
VkShaderModule AoResolveFragmentModule = VK_NULL_HANDLE;
uint32_t AoWidth = 0;
uint32_t AoHeight = 0;
// Zeroed on every path that does not render, so an `ao: off` report is never printed beside a
// caster count left over from before the knob moved - the same rule RecordShadowPass follows for
// `shadow_casters`, and for the same reason: a stale number reads as the pass still running.
uint32_t AoDrawCalls = 0;
uint32_t AoCastersDropped = 0;
// Whether this frame's prepass actually ran. The frame block is filled before the pass is
// recorded, so it cannot ask "did it draw anything" - what it can ask is whether the pass is up
// and the knob is on, and this carries the answer back for the report.
bool AoRanThisFrame = false;

void DestroyAoTargets() {
  VkImageView *views[] = {&AoPositionView, &AoNormalView, &AoResultView, &AoDepthView};
  for (VkImageView *view : views) {
    if (*view != VK_NULL_HANDLE) {
      vkDestroyImageView(GetDevice(), *view, nullptr);
      *view = VK_NULL_HANDLE;
    }
  }
  VkImage *images[] = {&AoPositionImage, &AoNormalImage, &AoResultImage, &AoDepthImage};
  for (VkImage *image : images) {
    if (*image != VK_NULL_HANDLE) {
      vkDestroyImage(GetDevice(), *image, nullptr);
      *image = VK_NULL_HANDLE;
    }
  }
  VkDeviceMemory *memories[] = {&AoPositionMemory, &AoNormalMemory, &AoResultMemory,
                                &AoDepthMemory};
  for (VkDeviceMemory *memory : memories) {
    if (*memory != VK_NULL_HANDLE) {
      vkFreeMemory(GetDevice(), *memory, nullptr);
      *memory = VK_NULL_HANDLE;
    }
  }
  AoWidth = 0;
  AoHeight = 0;
}

// One image plus its memory and view. Written out once here rather than four times, because the
// four differ only in format and usage - and a copy-pasted allocation that binds the wrong
// memory to the wrong image is invisible until something reads it.
bool CreateAoImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                   VkImageAspectFlags aspect, VkImage *image, VkDeviceMemory *memory,
                   VkImageView *view) {
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(GetDevice(), &info, nullptr, image) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requirements = {};
  vkGetImageMemoryRequirements(GetDevice(), *image, &requirements);
  if (!AllocateDeviceLocal(requirements, false, memory) ||
      vkBindImageMemory(GetDevice(), *image, *memory, 0) != VK_SUCCESS) {
    return false;
  }
  VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = *image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange = {aspect, 0, 1, 0, 1};
  return vkCreateImageView(GetDevice(), &view_info, nullptr, view) == VK_SUCCESS;
}

// The three targets and the pass's own depth buffer, at the render extent. Called from
// CreateAoPass and again from ResizeDraw, which is why it is separate from the pipelines - a
// resize must not rebuild a pipeline, and a pipeline must not be rebuilt to change an extent.
bool CreateAoTargets(uint32_t width, uint32_t height) {
  DestroyAoTargets();
  if (width == 0 || height == 0) {
    return false;
  }
  constexpr VkImageUsageFlags kColour =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (!CreateAoImage(width, height, kAoPositionFormat, kColour, VK_IMAGE_ASPECT_COLOR_BIT,
                     &AoPositionImage, &AoPositionMemory, &AoPositionView) ||
      !CreateAoImage(width, height, kAoNormalFormat, kColour, VK_IMAGE_ASPECT_COLOR_BIT,
                     &AoNormalImage, &AoNormalMemory, &AoNormalView) ||
      !CreateAoImage(width, height, kAoResultFormat, kColour, VK_IMAGE_ASPECT_COLOR_BIT,
                     &AoResultImage, &AoResultMemory, &AoResultView) ||
      !CreateAoImage(width, height, DepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     DepthAspect(), &AoDepthImage, &AoDepthMemory, &AoDepthView)) {
    DestroyAoTargets();
    return false;
  }
  // The descriptors are written once here rather than per frame: a bindless slot is a pointer to
  // a view, and these views only change when the extent does - at which point this runs again.
  WriteBindlessView(kAoPositionSlot, reinterpret_cast<uint64_t>(AoPositionView));
  WriteBindlessView(kAoNormalSlot, reinterpret_cast<uint64_t>(AoNormalView));
  WriteBindlessView(kAoResultSlot, reinterpret_cast<uint64_t>(AoResultView));
  AoWidth = width;
  AoHeight = height;
  return true;
}

void DestroyAoPass() {
  AoReady = false;
  DestroyAoTargets();
  VkPipeline *pipelines[] = {&AoPrepassPipeline, &AoResolvePipeline};
  for (VkPipeline *pipeline : pipelines) {
    if (*pipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(GetDevice(), *pipeline, nullptr);
      *pipeline = VK_NULL_HANDLE;
    }
  }
  if (AoLayout != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(GetDevice(), AoLayout, nullptr);
    AoLayout = VK_NULL_HANDLE;
  }
  VkShaderModule *modules[] = {&AoPrepassVertexModule, &AoPrepassFragmentModule,
                               &AoFullscreenVertexModule, &AoResolveFragmentModule};
  for (VkShaderModule *module : modules) {
    if (*module != VK_NULL_HANDLE) {
      vkDestroyShaderModule(GetDevice(), *module, nullptr);
      *module = VK_NULL_HANDLE;
    }
  }
}

bool CreateAoPass(uint32_t width, uint32_t height) {
  AoPrepassVertexModule = CreateModule(kAoPrepassVertexSpv, sizeof(kAoPrepassVertexSpv));
  AoPrepassFragmentModule = CreateModule(kAoPrepassFragmentSpv, sizeof(kAoPrepassFragmentSpv));
  AoFullscreenVertexModule = CreateModule(kAoFullscreenVertexSpv, sizeof(kAoFullscreenVertexSpv));
  AoResolveFragmentModule = CreateModule(kAoResolveFragmentSpv, sizeof(kAoResolveFragmentSpv));
  if (AoPrepassVertexModule == VK_NULL_HANDLE || AoPrepassFragmentModule == VK_NULL_HANDLE ||
      AoFullscreenVertexModule == VK_NULL_HANDLE || AoResolveFragmentModule == VK_NULL_HANDLE) {
    return false;
  }
  // The resolve reads its two inputs out of the bindless array, so without that set there is no
  // pass - unlike the shadow passes, which sample nothing and need no set at all.
  auto set_layout = reinterpret_cast<VkDescriptorSetLayout>(BindlessDescriptorSetLayout());
  if (set_layout == VK_NULL_HANDLE) {
    return false;
  }
  if (!CreateAoTargets(width, height)) {
    return false;
  }

  VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(AoPushConstants)};
  VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1;
  layout.pSetLayouts = &set_layout;
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &range;
  if (vkCreatePipelineLayout(GetDevice(), &layout, nullptr, &AoLayout) != VK_SUCCESS) {
    return false;
  }

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;

  // No vertex input state at all, for the same reason the world pass has none: the canonical
  // vertex is pulled by device address (§4.10).
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
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  // No culling, and the same reason the shadow pass gives: Gunlok's geometry is not consistently
  // wound across the map object and its props, so a cull mode opens holes in the set rather than
  // saving anything. A hole here is a pixel with no world position, which reads as a bright patch
  // in a crease.
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = VK_TRUE;
  depth.depthWriteEnable = VK_TRUE;
  depth.depthCompareOp = VK_COMPARE_OP_LESS;
  VkPipelineColorBlendAttachmentState attachments[2] = {};
  for (VkPipelineColorBlendAttachmentState &attachment : attachments) {
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  }
  VkPipelineColorBlendStateCreateInfo blend = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 2;
  blend.pAttachments = attachments;
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;

  const VkFormat prepass_formats[2] = {kAoPositionFormat, kAoNormalFormat};
  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 2;
  rendering.pColorAttachmentFormats = prepass_formats;
  rendering.depthAttachmentFormat = DepthFormat;
  if (DepthStencil) {
    rendering.stencilAttachmentFormat = DepthFormat;
  }

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
  info.layout = AoLayout;

  stages[0].module = AoPrepassVertexModule;
  stages[0].pName = "ao_prepass_vertex";
  stages[1].module = AoPrepassFragmentModule;
  stages[1].pName = "ao_prepass_fragment";
  if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr,
                                &AoPrepassPipeline) != VK_SUCCESS) {
    return false;
  }

  // The resolve: one triangle, no depth, one R8 target.
  VkPipelineDepthStencilStateCreateInfo no_depth = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &kAoResultFormat;
  rendering.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
  rendering.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
  info.pDepthStencilState = &no_depth;
  stages[0].module = AoFullscreenVertexModule;
  stages[0].pName = "ao_fullscreen_vertex";
  stages[1].module = AoResolveFragmentModule;
  stages[1].pName = "ao_resolve_fragment";
  if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr,
                                &AoResolvePipeline) != VK_SUCCESS) {
    return false;
  }

  AoReady = true;
  return true;
}

// --- HDR: the float target's knobs and the tonemap pass ---------------------------------------
//
// See SetHdr in VkDraw.h for what this is and the plan's "In progress: HDR" for why. Everything
// here is either a knob or the one pass that replaces the scale blit; the targets themselves
// belong to VkRenderer, which is what owns the offscreen image.

// R16G16B16A16 rather than R11G11B10: the alpha channel is not spare. The world pass's attachment
// carries the fixed function's alpha through to the framebuffer blend, and a format without one
// would make every SRCALPHA draw in the game read 1. 2.4 MB at 640x480 against the shadow atlas's
// 66 MB, so the two bytes a channel are not worth an argument.
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// GpuFrameData::colour_flags - must match world.slang.
constexpr uint32_t kLinearInput = 0x1u;
constexpr uint32_t kUnclamped = 0x2u;

// TonemapPushConstants::flags - must match tonemap.slang. A separate word from the two above
// because it describes the pass rather than the frame, and the two are set from one place each.
constexpr uint32_t kEncodeSrgb = 0x1u;

// **Off by default**, for the reason every departure in this file is: a default run has to keep
// the renderer's residual claim against `GKPLUS_RENDERER=d3d8` true, and this one breaks it
// deliberately and completely.
bool HdrEnabled = false;
bool HdrEnvRead = false;
// On by default *within* HDR - running the arithmetic on gamma-encoded values is the thing being
// fixed, so the interesting configuration is the one that fixes it. Off makes `hdr` the
// extended-range design instead, which is the bisect.
bool LinearInputEnabled = true;
// 1 = rolloff. 0 clamp, 2 reinhard, 3 aces, 4 filmic (Hable), 5 agx - see `apply_operator` in
// tonemap.slang. It is the default as the conservative choice; since §4.92 no operator reaches the
// 2D layers, so the film curves are looks rather than traps.
uint32_t TonemapOp = 1;
float ExposureValue = 1.0f;
// Where the rolloff stops being the identity. 0.75 leaves three quarters of the range untouched,
// which covers everything the game itself authors - a D3DCOLOR cannot exceed 1 - while still
// leaving the compressed half enough room not to read as a hard knee.
float TonemapKneeValue = 0.75f;
// What `reinhard` maps to exactly 1.0. 4.0 because that is the magnitude of the over-range
// actually reaching this pass: notes 4.48 measured level02's key light at `diffuse 4.0`.
float TonemapWhiteValue = 4.0f;

void ReadHdrEnvOnce() {
  if (HdrEnvRead) {
    return;
  }
  HdrEnvRead = true;
  char value[16] = {};
  const DWORD len = ::GetEnvironmentVariableA("GKPLUS_VK_HDR", value, sizeof(value));
  if (len == 0 || len >= sizeof(value)) {
    return;
  }
  const std::string text(value, len);
  HdrEnabled = !(text == "0" || text == "off" || text == "no");
}

bool TonemapPassReady = false;
VkPipelineLayout TonemapLayout = VK_NULL_HANDLE;
VkPipeline TonemapPipeline = VK_NULL_HANDLE;
VkShaderModule TonemapVertexModule = VK_NULL_HANDLE;
VkShaderModule TonemapFragmentModule = VK_NULL_HANDLE;

void DestroyTonemapPass() {
  TonemapPassReady = false;
  if (TonemapPipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(GetDevice(), TonemapPipeline, nullptr);
    TonemapPipeline = VK_NULL_HANDLE;
  }
  if (TonemapLayout != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(GetDevice(), TonemapLayout, nullptr);
    TonemapLayout = VK_NULL_HANDLE;
  }
  VkShaderModule *modules[] = {&TonemapVertexModule, &TonemapFragmentModule};
  for (VkShaderModule *module : modules) {
    if (*module != VK_NULL_HANDLE) {
      vkDestroyShaderModule(GetDevice(), *module, nullptr);
      *module = VK_NULL_HANDLE;
    }
  }
}

// Built once, at StartDraw, and never rebuilt: it writes the SWAPCHAIN, whose format is fixed for
// the life of the device - so unlike the world pipelines this one is untouched by the knob that
// changes what the world pass draws into.
bool CreateTonemapPass() {
  TonemapVertexModule =
      CreateModule(kTonemapFullscreenVertexSpv, sizeof(kTonemapFullscreenVertexSpv));
  TonemapFragmentModule = CreateModule(kTonemapFragmentSpv, sizeof(kTonemapFragmentSpv));
  if (TonemapVertexModule == VK_NULL_HANDLE || TonemapFragmentModule == VK_NULL_HANDLE) {
    return false;
  }
  // It reads the offscreen target out of the bindless array, the same way the AO resolve reads its
  // two inputs, so there is no descriptor set of its own to build.
  auto set_layout = reinterpret_cast<VkDescriptorSetLayout>(BindlessDescriptorSetLayout());
  if (set_layout == VK_NULL_HANDLE || SwapchainFormat == VK_FORMAT_UNDEFINED) {
    return false;
  }

  VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(TonemapPushConstants)};
  VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1;
  layout.pSetLayouts = &set_layout;
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &range;
  if (vkCreatePipelineLayout(GetDevice(), &layout, nullptr, &TonemapLayout) != VK_SUCCESS) {
    return false;
  }

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = TonemapVertexModule;
  stages[0].pName = "tonemap_fullscreen_vertex";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = TonemapFragmentModule;
  stages[1].pName = "tonemap_fragment";

  // One triangle pulled from SV_VertexID, so no vertex input and no buffer.
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
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  // **One sample, whatever `render.msaa` says.** The multisampled target is resolved at the end of
  // the world pass, so what this reads is already single-sampled - the same reason the scale blit
  // it replaces never knew MSAA existed.
  VkPipelineMultisampleStateCreateInfo multisample = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState attachment = {};
  attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &attachment;
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;

  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &SwapchainFormat;

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
  info.layout = TonemapLayout;
  if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr,
                                &TonemapPipeline) != VK_SUCCESS) {
    return false;
  }
  TonemapPassReady = true;
  return true;
}

} // namespace

bool StartDraw(uint32_t width, uint32_t height, uint32_t colour_format) {
  if (Ready) {
    return true;
  }
  SwapchainFormat = static_cast<VkFormat>(colour_format);
  // **Only if nothing has set it.** `ReconcileRenderTarget` runs before this at bring-up and
  // has already called `ApplyColourFormat` with whatever `HdrTargetFormat` chose, so assigning
  // the swapchain's format unconditionally here would silently undo the HDR target on the very
  // frame it was created.
  if (ColourFormat == VK_FORMAT_UNDEFINED) {
    ColourFormat = SwapchainFormat;
  }
  if (!ChooseDepthFormat() || !CreateDepth(width, height) || !CreatePipelineLayout()) {
    return false;
  }
  // Not fatal: without it the fragment loops every light in the level, which is exactly what
  // phase 3b did and is still a correct picture. A device that cannot build a compute pipeline
  // should lose the optimisation, not the renderer.
  // Not fatal either: a device that cannot build it loses shadows, not the renderer.
  if (!CreateShadowPass()) {
    DebugWrite("gkplus: shadow pass unavailable, the sun casts nothing\n");
    DestroyShadowPass();
  }
  // After the sun's, because it borrows `ShadowModule` and `ShadowLayout` from it - the two
  // passes differ only in the depth format and the matrix they are pushed. Not fatal for the
  // same reason: a device without it loses the map lights' shadows and nothing else.
  if (!MapShadowReady && !CreateMapShadowAtlas()) {
    DebugWrite("gkplus: map shadow atlas unavailable, the map lights cast nothing\n");
    DestroyMapShadowAtlas();
  }
  // After the map's, because it borrows `MapShadowModule` from it as well as the sun's layout.
  // Not fatal: a device without `multiDrawIndirect` loses the per-frame atlas and every local
  // light falls back to the static one, which is exactly what §4.65 shipped.
  if (!DynShadowReady && !CreateDynShadowAtlas()) {
    DebugWrite("gkplus: per-frame shadow atlas unavailable, local lights use the static one\n");
    DestroyDynShadowAtlas();
  }
  if (!CreateLightGridPipeline()) {
    DebugWrite("gkplus: light grid unavailable, map lighting will not be culled\n");
    DestroyLightGridPipeline();
  }
  // Last, and not fatal for the same reason as the three above: a device that cannot build it
  // loses ambient occlusion and nothing else. It needs the bindless set, which CreatePipelineLayout
  // has already proved exists by this point.
  if (!CreateAoPass(width, height)) {
    DebugWrite("gkplus: ambient occlusion unavailable\n");
    DestroyAoPass();
  }
  // Not fatal either, and this is the one whose failure has to be *reported* rather than merely
  // survived: without it `HdrTargetFormat` keeps the 8-bit target, so `render.hdr` reads back on
  // and does nothing. It needs the bindless set, which `CreatePipelineLayout` has already proved
  // exists, and the swapchain format, which is this function's own argument.
  if (!CreateTonemapPass()) {
    DebugWrite("gkplus: tonemap pass unavailable, render.hdr will not engage\n");
    DestroyTonemapPass();
  }
  Items.reserve(kMaxDrawsPerFrame);
  Ready = true;
  TheStats.ready = true;
  Error.clear();
  // `GKPLUS_VK_STOCK=1` - the whole departure set off from the first frame, for a session that is
  // there to compare against `GKPLUS_RENDERER=d3d8` (see SetStock in VkDraw.h).
  //
  // Here rather than in a lazy reader like the two per-knob env vars have, because there is no
  // single flag this gates - it writes nine, and nothing on the draw path asks it anything. After
  // `Ready`, so a setter that checks it sees the pipeline up; **once per process**, because the
  // device can be recreated and re-applying the preset on a resize would silently undo whatever
  // the REPL had set since.
  static bool stock_env_read = false;
  if (!stock_env_read) {
    stock_env_read = true;
    char value[16] = {};
    const DWORD len = ::GetEnvironmentVariableA("GKPLUS_VK_STOCK", value, sizeof(value));
    if (len > 0 && len < sizeof(value)) {
      const std::string text(value, len);
      if (!(text == "0" || text == "off" || text == "no")) {
        SetStock(true);
        DebugWrite("gkplus: GKPLUS_VK_STOCK - every departure off, drawing the stock look\n");
      }
    }
  }
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

void SetPerPixelLighting(bool enabled) {
  // Marks the env as read, so a REPL write is not undone by the first draw that asks.
  PerPixelLightingRead = true;
  PerPixelLightingWanted = enabled;
}

bool PerPixelLighting() { return PerPixelLightingEnabled(); }

namespace {
// **Off by default.** It changes the silhouette of the level rather than reproducing D3D, so the
// renderer's own claim - that it draws what the original drew - has to survive a default run.
bool TessellationOn = false;
bool TessellationShadowsOn = true;
TessSet TessellationWhich = TessSet::Map;
TessellationParams TheTessParams;
uint32_t TessDrawsThisFrame = 0;
uint32_t TessPatchesThisFrame = 0;
} // namespace

void SetTessellationEnabled(bool enabled) { TessellationOn = enabled; }
bool TessellationEnabled() { return TessellationOn && Caps().tessellation_shader; }
void SetTessellationSet(TessSet set) { TessellationWhich = set; }
TessSet TessellationSet() { return TessellationWhich; }
void SetTessellationShadows(bool enabled) { TessellationShadowsOn = enabled; }
bool TessellationShadows() { return TessellationShadowsOn; }
const TessellationParams &TessParams() { return TheTessParams; }
TessellationParams &MutableTessParams() { return TheTessParams; }
void TessellationCounts(uint32_t &draws, uint32_t &patches) {
  draws = TessDrawsThisFrame;
  patches = TessPatchesThisFrame;
}

namespace {
// Copy the level's light rig into this frame's scratch slice, premultiplied.
//
// Called once at the top of RecordDraws, **after** the addresses above are read and before any
// draw is recorded, so the address it publishes belongs to the same slice every draw this frame
// will pull from. Doing it per draw would be the same bytes many times over; doing it after
// RotateFrameScratch would publish the next scene's slice.
void UploadMapLights() {
  FrameMapLightAddress = 0;
  FrameMapLightCount = 0;
  FrameMapAmbience = 0.0f;
  if (!MapLightingEnabled) {
    return; // costs nothing at all when off, which is what makes the A/B honest
  }
  const std::vector<MapLight> &lights = MapLights();
  if (lights.empty()) {
    return;
  }
  const ScratchAlloc alloc = AllocateScratchMapLights(static_cast<uint32_t>(lights.size()));
  if (!alloc.valid || alloc.mapped == nullptr) {
    return; // the slice is full; the frame draws without map lighting rather than half of it
  }
  auto *out = static_cast<GpuMapLight *>(alloc.mapped);
  for (size_t i = 0; i < lights.size(); ++i) {
    const MapLight &light = lights[i];
    GpuMapLight &gpu = out[i];
    gpu.position[0] = light.position.x;
    gpu.position[1] = light.position.y;
    gpu.position[2] = light.position.z;
    gpu.position[3] = light.range;
    // Premultiplied here so the shader's inner loop is a multiply-add and nothing else.
    //
    // **The colour is sRGB-decoded under `render.linear_input` and the brightness is not**, which
    // is the split the whole feature turns on: the colour is a picture of a light the level's
    // author picked against a gamma-space renderer, and the brightness is a scalar they multiplied
    // it by. Decoding one and not the other is what §4.94 was about, one level up.
    const bool linear = LinearInputActive();
    for (int c = 0; c < 3; ++c) {
      const float colour = linear ? SrgbToLinear(light.colour[c]) : light.colour[c];
      gpu.colour[c] = colour * light.brightness;
    }
    gpu.colour[3] = static_cast<float>(light.flags);
    // Row 2 of the orientation - elements 6..8. §4.54 is why this row and not another.
    gpu.axis[0] = light.orientation[6];
    gpu.axis[1] = light.orientation[7];
    gpu.axis[2] = light.orientation[8];
    // The static shadow atlas slot, or -1. Published only once the bake has actually reached it,
    // so a light whose tile is still the clear value is not sampled at all rather than sampled
    // and found unshadowed - the two look the same on screen and only the first is honest.
    const int32_t slot = i < MapShadowSlotForLight.size() ? MapShadowSlotForLight[i] : -1;
    const bool baked = slot >= 0 && static_cast<uint32_t>(slot) < MapShadowCursor;
    gpu.axis[3] = baked ? static_cast<float>(slot) : -1.0f;
  }
  FrameMapLightAddress = ScratchMapLightAddress() + alloc.offset * sizeof(GpuMapLight);
  FrameMapLightByteOffset = ScratchMapLightSliceOffset() + alloc.offset * sizeof(GpuMapLight);
  FrameMapLightCount = static_cast<uint32_t>(lights.size());
  FrameMapAmbience = MapAmbience();
}
// Bounded rather than unbounded, and the report says how many it skipped: a silent cap would
// read as "the whole frame is flat" when it means "most of the frame was never looked at".
constexpr uint32_t kMaxCensusDraws = 512;

// What a census did not look at. Carried rather than discarded for the reason above: a report
// that answers for a subset and does not say so is worse than no report.
struct CensusSkips {
  uint32_t source = 0, topology = 0, over_cap = 0, read_failures = 0, examined = 0;
};

// One draw's indices and the canonical vertices they address, read out of the arena.
//
// `lowest` comes back as the vertex the span starts at, so a triangle's corner `k` of triangle
// `t` is `vertices[base_vertex + indices[t + k] + vertex_offset - lowest]`.
//
// One read for the whole draw rather than one per triangle, because `ReadArena` submits and
// waits; and the span is derived from the indices rather than assumed to start at `base_vertex`,
// since `base_vertex + index + vertex_offset` is what the shader addresses.
bool ReadDrawGeometry(const DrawItem &item, CensusSkips &skips, std::vector<uint32_t> &indices,
                      std::vector<CanonicalVertex> &vertices, int64_t &lowest) {
  // Both sources must be the arena: the scratch is a different buffer and rotates, and a
  // non-indexed draw has no shared vertices for a normal to be averaged across in the first
  // place - so neither can answer a census question even in principle.
  if (!item.indexed || item.vertex_source != DrawSource::Arena ||
      item.index_source != DrawSource::Arena) {
    ++skips.source;
    return false;
  }
  if (item.pipeline.topology != 4 /* D3DPT_TRIANGLELIST */ || item.count < 3) {
    ++skips.topology;
    return false;
  }
  if (skips.examined >= kMaxCensusDraws) {
    ++skips.over_cap;
    return false;
  }
  ++skips.examined;

  const uint32_t stride = item.index_stride == 4 ? 4u : 2u;
  std::vector<uint8_t> index_bytes(size_t(item.count) * stride, 0);
  if (!ReadArena(false, uint64_t(item.first_index) * stride, uint32_t(index_bytes.size()),
                 index_bytes.data())) {
    ++skips.read_failures;
    return false;
  }
  indices.resize(item.count);
  for (uint32_t i = 0; i < item.count; ++i) {
    indices[i] = stride == 4 ? reinterpret_cast<const uint32_t *>(index_bytes.data())[i]
                             : reinterpret_cast<const uint16_t *>(index_bytes.data())[i];
  }

  lowest = INT64_MAX;
  int64_t highest = INT64_MIN;
  for (const uint32_t index : indices) {
    const int64_t v = int64_t(item.base_vertex) + int64_t(index) + item.vertex_offset;
    lowest = v < lowest ? v : lowest;
    highest = v > highest ? v : highest;
  }
  if (lowest < 0 || highest < lowest) {
    ++skips.read_failures;
    return false;
  }
  const uint64_t span = uint64_t(highest - lowest) + 1;
  vertices.assign(size_t(span), CanonicalVertex{});
  if (!ReadArena(true, uint64_t(lowest) * sizeof(CanonicalVertex),
                 uint32_t(span * sizeof(CanonicalVertex)), vertices.data())) {
    ++skips.read_failures;
    return false;
  }
  return true;
}

// --- the PN split-corner table (§4.71) --------------------------------------------------------
//
// **The fix for the tear at a material boundary**, and it is a data fix because it cannot be
// anything else: `b210` for edge (P1,P2) is a function of P1, P2 and N1 alone, so the two
// triangles sharing that edge build the same boundary curve only while they present the same
// normal at each end. Where the mesh has split a corner into two vertices carrying different
// normals - an exporter at a material boundary or a smoothing-group break - they do not, and the
// two patches pull apart. A patch sees only its own three normals, so nothing in the shader can
// close it; the only quantities both sides could agree on are the two positions.
//
// So the question is answered here, where the whole mesh is visible, and the answer travels down
// as one bit per canonical vertex. **The bit means "the mesh says this corner is not smooth"**,
// which makes zeroing its tangent term the right answer and not merely a safe one - an
// intentional hard edge stays hard instead of being averaged into a smooth one, which is what
// reconciling the two normals would have done.
//
// Positions are keyed on their **bit patterns**, exactly as `render.seam_census()` keys an edge,
// because that is what makes a split corner one corner rather than two.
struct SplitPosition {
  float normal[3];                  // the first normal seen here
  std::vector<uint32_t> vertices;   // every canonical vertex at this position
  bool split = false;
};

struct PositionKey {
  uint32_t bits[3];
  bool operator<(const PositionKey &o) const {
    return std::memcmp(bits, o.bits, sizeof(bits)) < 0;
  }
};

// **Analysis is per draw and each draw is analysed once**, keyed on the three fields that say
// which vertices it addresses. A draw's identity cannot be its index in the list: culling
// reorders and resizes that every frame, which would re-read the whole set forever.
struct DrawKey {
  uint32_t base_vertex, first_index, count;
  bool operator<(const DrawKey &o) const {
    return base_vertex != o.base_vertex     ? base_vertex < o.base_vertex
           : first_index != o.first_index   ? first_index < o.first_index
                                            : count < o.count;
  }
};

std::map<PositionKey, SplitPosition> SplitPositions;
std::set<DrawKey> SplitAnalysedDraws;
std::set<uint32_t> SplitVertices; // the answer: which canonical vertices are split corners
uint32_t SplitGeneration = UINT32_MAX;
uint32_t SplitBase = 0, SplitCount = 0;
uint64_t SplitAddress = 0;
uint32_t SplitPending = 0;   // draws seen this frame that have not been analysed yet
bool SplitTooLarge = false;  // the bitset would not fit its scratch slice
bool SplitCornerFixOn = true;

// **Bounded per frame, not per level.** The analysis is `ReadArena`, which submits and waits, and
// a level's first tessellated frame sees ~65 map draws at once - reading all of them in one frame
// is a visible hitch where spreading them over eight is not. Until a draw has been analysed its
// corners read as unsplit, which is the behaviour that existed before this table, so the cost of
// converging slowly is a few frames of the old tear rather than anything new.
constexpr uint32_t kSplitDrawsPerFrame = 8;

void ResetSplitCorners() {
  SplitPositions.clear();
  SplitAnalysedDraws.clear();
  SplitVertices.clear();
  SplitBase = 0;
  SplitCount = 0;
  SplitTooLarge = false;
}

// Records one corner. The first normal at a position is kept as the reference; a second, different
// one marks the position split and every vertex ever seen at it - including the ones recorded
// before the disagreement showed up, which is why the vertex list is kept rather than a count.
void NoteSplitCorner(const CanonicalVertex &vertex, uint32_t index) {
  PositionKey key;
  std::memcpy(key.bits, vertex.pos, sizeof(key.bits));
  SplitPosition &entry = SplitPositions[key];
  if (entry.vertices.empty()) {
    std::memcpy(entry.normal, vertex.normal, sizeof(entry.normal));
  } else if (!entry.split &&
             std::memcmp(entry.normal, vertex.normal, sizeof(entry.normal)) != 0) {
    entry.split = true;
    for (const uint32_t seen : entry.vertices) {
      SplitVertices.insert(seen);
    }
  }
  // Guarded, because a position is revisited by every triangle that touches it and the list is
  // walked on the transition above.
  if (std::find(entry.vertices.begin(), entry.vertices.end(), index) == entry.vertices.end()) {
    entry.vertices.push_back(index);
  }
  if (entry.split) {
    SplitVertices.insert(index);
  }
}

bool WantsTessellation(const DrawItem &item);

// Analyses up to `kSplitDrawsPerFrame` of the frame's tessellated draws that have not been seen
// before. Called from UploadFrameData, so `Items` is this frame's complete list.
void UpdateSplitCorners() {
  // `MapLightsGeneration()` moves on a level change and on nothing else, which is exactly the
  // event that invalidates every vertex index here.
  const uint32_t generation = MapLightsGeneration();
  if (generation != SplitGeneration) {
    SplitGeneration = generation;
    ResetSplitCorners();
  }
  SplitPending = 0;
  if (!SplitCornerFixOn || !TessellationEnabled()) {
    return;
  }
  CensusSkips skips;
  std::vector<CanonicalVertex> vertices;
  std::vector<uint32_t> indices;
  uint32_t analysed = 0;
  for (const DrawItem &item : Items) {
    if (!WantsTessellation(item)) {
      continue;
    }
    const DrawKey key = {item.base_vertex, item.first_index, item.count};
    if (SplitAnalysedDraws.count(key) != 0) {
      continue;
    }
    if (analysed >= kSplitDrawsPerFrame) {
      ++SplitPending;
      continue;
    }
    int64_t lowest = 0;
    if (!ReadDrawGeometry(item, skips, indices, vertices, lowest)) {
      // Insert anyway: a draw this cannot read is one it will never be able to read, and
      // retrying it every frame would stall the frame forever.
      SplitAnalysedDraws.insert(key);
      continue;
    }
    ++analysed;
    SplitAnalysedDraws.insert(key);
    for (uint32_t i = 0; i < item.count; ++i) {
      const int64_t absolute = int64_t(item.base_vertex) + int64_t(indices[i]) + item.vertex_offset;
      NoteSplitCorner(vertices[size_t(absolute - lowest)], uint32_t(absolute));
    }
  }
}

// Copies the bitset into this frame's scratch and returns its address, so the fields written into
// GpuFrameData and every ShadowPushConstants come from one place and cannot disagree.
//
// The bitset spans only the split vertices themselves - `base` is the lowest and `count` the
// span - because they are what has to be addressed, and an arena offset is far from zero.
void UploadSplitCorners() {
  SplitAddress = 0;
  SplitBase = 0;
  SplitCount = 0;
  if (SplitVertices.empty()) {
    return;
  }
  const uint32_t lowest = *SplitVertices.begin();
  const uint32_t highest = *SplitVertices.rbegin();
  const uint32_t count = highest - lowest + 1;
  const uint32_t dwords = (count + 31u) / 32u;
  const ScratchAlloc alloc = AllocateScratchSplitCorners(dwords);
  if (!alloc.valid || alloc.mapped == nullptr) {
    // The slice could not hold it. Reported rather than silently truncated: half a table closes
    // half the seams and leaves the rest torn, which reads as the fix not working.
    SplitTooLarge = true;
    return;
  }
  SplitTooLarge = false;
  auto *bits = static_cast<uint32_t *>(alloc.mapped);
  std::memset(bits, 0, size_t(dwords) * sizeof(uint32_t));
  for (const uint32_t vertex : SplitVertices) {
    const uint32_t index = vertex - lowest;
    bits[index >> 5u] |= 1u << (index & 31u);
  }
  SplitAddress = ScratchSplitCornerAddress() + uint64_t(alloc.offset) * sizeof(uint32_t);
  SplitBase = lowest;
  SplitCount = count;
}

void SplitCornerTableFor(uint64_t &address, uint32_t &base, uint32_t &count) {
  address = SplitAddress;
  base = SplitBase;
  count = SplitAddress != 0 ? SplitCount : 0;
}
} // namespace

// Built before any pass reads it, which is why this is its own entry point rather than a line in
// UploadFrameData: the shadow bakes take the same table through their push block, and they are
// recorded *before* RecordDraws. The address is into the frame scratch, so a table built after
// the shadow pass would hand it the previous slice - the one being overwritten.
void PrepareTessellationTables() {
  UpdateSplitCorners();
  UploadSplitCorners();
}

void SetSplitCornerFix(bool enabled) {
  if (SplitCornerFixOn != enabled) {
    SplitCornerFixOn = enabled;
    // Cleared rather than kept, so turning it back on re-derives the table instead of trusting
    // one built before whatever the caller changed in between.
    ResetSplitCorners();
  }
}

bool SplitCornerFix() { return SplitCornerFixOn; }

void SplitCornerCounts(uint32_t &corners, uint32_t &analysed_draws, uint32_t &pending_draws,
                       bool &too_large) {
  corners = static_cast<uint32_t>(SplitVertices.size());
  analysed_draws = static_cast<uint32_t>(SplitAnalysedDraws.size());
  pending_draws = SplitPending;
  too_large = SplitTooLarge;
}

// One GpuFrameData for this frame, written after the lights are uploaded (so the addresses are
// this frame's) and before any draw is recorded (so every draw's push points at the same block).
void UploadFrameData() {
  FrameDataAddress = 0;
  UiFrameDataAddress = 0;
  const ScratchAlloc alloc = AllocateScratchFrames(1);
  if (!alloc.valid || alloc.mapped == nullptr) {
    return; // every draw then pushes address 0, and the shader's null check draws unlit
  }
  auto *frame = static_cast<GpuFrameData *>(alloc.mapped);
  *frame = GpuFrameData{};
  frame->map_lights = FrameMapLightAddress;
  frame->light_grid = LightGridAddress();
  frame->light_indices = LightIndexAddress();
  frame->force_lod = ForcedLod;
  const LightingMapParams &lighting = LightingParams();
  frame->bump_scale = lighting.bump_scale;
  frame->bump_diffuse = lighting.bump_diffuse;
  frame->specular_scale = lighting.specular_scale;
  frame->gloss_min = lighting.gloss_min;
  frame->gloss_max = lighting.gloss_max;
  frame->specular_from_diffuse = lighting.specular_from_diffuse;
  frame->chrome_scale = lighting.chrome_scale;
  frame->chrome_blur = lighting.chrome_blur;
  frame->chrome_texgen = lighting.chrome_texgen ? 1.0f : 0.0f;
  frame->per_pixel_lighting = PerPixelLightingEnabled() ? 1.0f : 0.0f;
  // The tessellation knobs (§4.71). `max_factor` is clamped to the device's own ceiling here
  // rather than in the shader: exceeding maxTessellationGenerationLevel is undefined behaviour,
  // not a clamp the hardware performs, so a REPL sweep that overshoots must not reach it.
  const TessellationParams &tess = TessParams();
  const float device_max =
      Caps().max_tessellation_level != 0 ? float(Caps().max_tessellation_level) : 64.0f;
  // Ternaries and not std::min/std::max: <windows.h> is included here and defines both as macros.
  const float wanted_max = tess.max_factor < 1.0f ? 1.0f : tess.max_factor;
  const float wanted_min = tess.min_factor < 1.0f ? 1.0f : tess.min_factor;
  frame->tess_edge_pixels = tess.edge_pixels;
  frame->tess_max = wanted_max > device_max ? device_max : wanted_max;
  frame->tess_min = wanted_min > frame->tess_max ? frame->tess_max : wanted_min;
  frame->pn_strength = tess.pn_strength;
  frame->pn_flat_threshold = tess.pn_flat_threshold;
  // The offscreen target, which is what the world pass rasterises into (§4.38) - not the
  // swapchain, whose 628x468 would make an edge read 2% short.
  frame->target_width = float(ViewportWidth);
  frame->target_height = float(ViewportHeight);
  frame->pn_max_offset = tess.pn_max_offset < 0.0f ? 0.0f : tess.pn_max_offset;
  frame->split_corners = SplitAddress;
  frame->split_base = SplitBase;
  frame->split_count = SplitAddress != 0 ? SplitCount : 0;
  frame->map_light_gain = MapLightGainValue;
  frame->map_ambience = FrameMapAmbience;
  frame->map_light_count = FrameMapLightCount;
  frame->map_flags = 0;
  if (MapLightingEnabled && FrameMapLightCount > 0) {
    frame->map_flags |= 1u;
  }
  if (MapLightingAllEnabled) {
    frame->map_flags |= 2u;
  }
  // Gated on the buffers existing as well as the build having run, so a device that could not
  // create the compute pipeline lands on the every-light fallback rather than on a null address.
  if (GridValid && frame->light_grid != 0 && frame->light_indices != 0) {
    frame->map_flags |= 4u;
  }
  // kNoTexture unless the pass actually rendered a map this frame - which is the one test the
  // fragment shader makes, so "no sun", "knob off" and "no shadow pipeline" are one state there.
  frame->shadow_texture = (ShadowReady && SunShadowsEnabled && FrameSunValid) ? kShadowMapSlot
                                                                             : kNoTexture;
  // D3D enums, which is what AcquireSampler speaks: LINEAR mag and min, no mip, CLAMP on both
  // axes. Clamp matters - a fragment outside the map's box must sample the edge rather than wrap
  // round and be shadowed by geometry on the other side of the level.
  frame->shadow_sampler = AcquireSampler(2, 2, 0, 3, 3);
  frame->shadow_strength = ShadowStrengthValue;
  // The TILE's reciprocal, not the atlas's: a PCF tap is a step in a cascade's own uv, and the
  // atlas offset is applied after the tap. Getting this wrong halves the filter width silently.
  frame->shadow_texel = 1.0f / static_cast<float>(kShadowTile);
  frame->shadow_z_near = FrameSunZNear;
  frame->shadow_z_span = FrameSunZSpan;
  frame->shadow_cascades = frame->shadow_texture == kNoTexture ? 0u : FrameCascadeCount;
  // The static shadow atlas. **`map_shadow_texture` now means "the atlas exists and somebody wants
  // it"**, and which of its two producers may sample it is `light_flags` below - because the map
  // lights (§4.61) and D3D's own point lights (§4.65) are separate features sharing one image and
  // each has its own knob. A light with no slot carries -1 in its own record, so "more lights than
  // the atlas holds" still needs no test here.
  frame->map_shadow_texture = (MapShadowReady && (MapShadowsEnabled || LocalShadowsEnabled()))
                                  ? kMapShadowMapSlot
                                  : kNoTexture;
  // The same sampler the sun's map uses, and for the same reason - LINEAR with CLAMP on both
  // axes. Clamp matters more here: the atlas is a grid of unrelated tiles, so a wrapped fetch
  // would read another light's cube face entirely.
  frame->map_shadow_sampler = AcquireSampler(2, 2, 0, 3, 3);
  frame->map_shadow_offset = MapShadowBiasValue;
  // The per-frame atlas. Gated on it having actually been baked **this frame** as well as on the
  // knob: a frame that registered no light leaves the image holding the previous frame's cubes,
  // and a light whose slot is -1 would not read it - but one whose slot survived from a frame the
  // bake skipped would, and would read another light's cube.
  // **The slice the bake just wrote**, not a fixed slot: `UploadFrameData` runs inside RecordDraws,
  // which is after BakeDynamicShadows in the frame, so `DynImageSlot` is this frame's answer.
  frame->dyn_shadow_texture = (DynShadowReady && DynamicShadowsEnabled && DynShadowSample &&
                               DynBakedFrame == DynFrame && !DynLights.empty())
                                  ? kDynShadowMapSlot - DynImageSlot
                                  : kNoTexture;
  frame->dyn_shadow_sampler = AcquireSampler(2, 2, 0, 3, 3);
  frame->dyn_shadow_offset = DynShadowBiasValue;
  // Both D3D atlases, and NOT the map lights' own reads of the static one - which is the whole
  // reason this is a frame field rather than a property of either image (§4.69).
  frame->local_shadow_taps = static_cast<uint32_t>(LocalShadowTapsValue);
  frame->light_flags = 0;
  if (LocalLightsEnabled) {
    frame->light_flags |= 1u;
  }
  if (MapShadowsEnabled) {
    frame->light_flags |= 2u;
  }
  if (LocalShadowsEnabled()) {
    frame->light_flags |= 4u;
  }
  if (LocalLightWindowEnabled) {
    frame->light_flags |= 8u;
  }
  std::memcpy(frame->cascades, FrameCascade, sizeof(frame->cascades));
  std::memcpy(frame->sun_matrix, FrameSunMatrix, sizeof(frame->sun_matrix));
  // Ambient occlusion (§4.86). Gated on the pass having actually **rendered this frame** rather
  // than on the knob, which is the same test `dyn_shadow_texture` above makes and for the same
  // reason: a frame whose caster list was empty leaves the target holding the previous frame's
  // occlusion, and multiplying by another frame's creases is worse than multiplying by nothing.
  // This can ask, where the atlases have to reconstruct it, because RecordAoPass runs earlier in
  // DrawFrame than UploadFrameData does.
  frame->ao_texture = AoRanThisFrame ? kAoResultSlot : kNoTexture;
  frame->ao_flags = (AoMapOnlyEnabled ? kAoMapOnly : 0u) | (AoDebugEnabled ? kAoDebug : 0u);
  frame->ao_direct = AoDirectValue;
  frame->pad_ao = 0.0f;
  // **Both derived from the target this frame is actually drawing into**, not from the knob.
  // `render.hdr` is a request; `ColourFormat` is the answer, and it lags the request by a frame at
  // bring-up and stays at the swapchain's format forever on a device with no tonemap pass. Lifting
  // the clamps against an 8-bit target would achieve nothing (UNORM clamps anyway) but decoding
  // the albedo against one would be visibly wrong - a dark frame nothing ever re-encodes - so the
  // gate has to be the format rather than the intent.
  const bool hdr_active = ColourFormat == kHdrFormat;
  frame->colour_flags = (hdr_active && LinearInputEnabled ? kLinearInput : 0u) |
                        (hdr_active ? kUnclamped : 0u);
  frame->pad_colour[0] = frame->pad_colour[1] = frame->pad_colour[2] = 0;
  FrameDataAddress = ScratchFrameAddress() + alloc.offset * sizeof(GpuFrameData);

  // --- and a second copy for the UI pass, differing in one word -------------------------------
  //
  // The 2D layers are drawn AFTER the tonemap, into an 8-bit target, so they must not be
  // sRGB-decoded and must not have their clamps lifted: nothing downstream would re-encode them,
  // and the fixed function's D3DCOLOR clamp is what their blends were authored against. A second
  // block is how that is said per pass without a per-draw flag - `push.frame` is already a
  // per-draw push, so a UI draw simply points at this one.
  //
  // A whole 352-byte copy rather than one field, because every *other* field has to stay
  // identical: the UI layer still samples the shadow atlases, still reads the AO result, still
  // carries the same tessellation and lighting-map parameters. Diverging on anything but
  // `colour_flags` would make the split visible as a shading change rather than as a colour-space
  // one. Allocated unconditionally so the address is valid whatever the knobs say - it costs 352
  // bytes of a 2688 KB per-frame arena.
  UiFrameDataAddress = FrameDataAddress;
  const ScratchAlloc ui_alloc = AllocateScratchFrames(1);
  if (ui_alloc.valid && ui_alloc.mapped != nullptr) {
    auto *ui = static_cast<GpuFrameData *>(ui_alloc.mapped);
    *ui = *frame;
    ui->colour_flags = 0;
    UiFrameDataAddress = ScratchFrameAddress() + ui_alloc.offset * sizeof(GpuFrameData);
  }
}

namespace {
// The world point the shadow boxes are centred on: **the camera's orbit pivot**, not its eye and
// not `GetCameraFocus()`.
//
// §4.58 used the focus and it is stale in ordinary play (§4.59). `CameraFocus` @ 0x007b3e58 is
// only latched by `SET CAMERA FOCUS`; with none in force the global still holds whatever the last
// one left, so the box sat somewhere unrelated to the view and only reached it because the extent
// was large enough to span the gap. The tell was that `shadow_extent = 20` produced **no shadow
// at all** on a frame where 70 produced a correct one, and latching a focus at the camera's own
// position brought it straight back.
//
// `CameraCoords` @ 0x007b4e0c is the pivot, which is measured rather than assumed: with the
// camera at rest on level04 it read (-65, -7, 48) while the view matrix in `GpuDrawRecord::eye`
// put the eye at (-67.16, -17.93, 58.05) - 15.007 units away, against a `camera.distance` of
// exactly 15. So the engine stores the point the camera looks at and derives the eye by pulling
// back the distance, which is precisely the centre of what is on screen.
//
// The focus still wins when one IS latched, because then the camera is pointed at it by
// definition - and that is the only reading under which both globals agree.
Vec3 ShadowPivot() {
  return gk::IsCameraFocusSet() ? gk::GetCameraFocus() : gk::GetCameraPosition();
}

// The sun's cascades: one light-space basis, and N concentric boxes in it.
//
// An orthographic box around the camera's pivot, looking along the sun. It is deliberately NOT
// fitted to the view frustum: the frustum is not available as a per-frame quantity on this side -
// the view and projection are folded into each draw's `mvp` - and a box around the pivot is what
// the camera is actually looking at in a game whose camera orbits a point. That is also what makes
// concentric cascades the right shape here rather than the usual per-frustum-slice ones: the pivot
// is the middle of the screen, so "near the pivot" is "near the middle", in every direction at once.
//
// **The cascades share a z range**, taken from the outermost. That is what lets one light-space
// transform serve all of them - a fragment's depth is computed once and only its xy is tested
// against each box - and D32_SFLOAT has depth precision to spare over a span this size.
//
// **Each box's centre is snapped to its own texel grid.** Without it the whole map re-samples
// every time the pivot moves by a fraction of a texel, and every shadow edge crawls; the snap
// costs two rounds per cascade and makes the map move in whole texels or not at all.
//
// No Y flip. Vulkan's framebuffer Y runs the other way from D3D's, and the world pass compensates
// in BuildMvp - but here the same basis both rasterises the map and looks it up, so a flip would
// cancel against itself. Leaving it out is one fewer convention to get backwards.
void BuildSunCascades() {
  FrameSunValid = false;
  FrameCascadeCount = 0;
  const Vec3 sun = gk::GetSunDirection();
  const float length = std::sqrt(sun.x * sun.x + sun.y * sun.y + sun.z * sun.z);
  if (length < 1e-4f) {
    return; // no sun set yet, which is every frame before a level is up
  }
  const float dx = sun.x / length, dy = sun.y / length, dz = sun.z / length;

  // An up vector that is not parallel to the sun. Gunlok's world is Y-down, so Y is the natural
  // choice and X is the fallback for a sun pointing straight up or down.
  float ux = 0.0f, uy = 1.0f, uz = 0.0f;
  if (std::fabs(dy) > 0.99f) {
    ux = 1.0f;
    uy = 0.0f;
  }
  // Left-handed look-at: z along the view direction, x = up cross z, y = z cross x.
  float xx = uy * dz - uz * dy, xy = uz * dx - ux * dz, xz = ux * dy - uy * dx;
  const float xl = std::sqrt(xx * xx + xy * xy + xz * xz);
  if (xl < 1e-6f) {
    return;
  }
  xx /= xl;
  xy /= xl;
  xz /= xl;
  const float yx = dy * xz - dz * xy, yy = dz * xx - dx * xz, yz = dx * xy - dy * xx;

  // World -> light space: the basis, no scale and no translation, so the result is in world
  // units. Row-vector, so the axes are the columns.
  float *m = FrameSunMatrix;
  m[0] = xx;   m[1] = yx;   m[2] = dx;   m[3] = 0.0f;
  m[4] = xy;   m[5] = yy;   m[6] = dy;   m[7] = 0.0f;
  m[8] = xz;   m[9] = yz;   m[10] = dz;  m[11] = 0.0f;
  m[12] = 0.0f; m[13] = 0.0f; m[14] = 0.0f; m[15] = 1.0f;

  const Vec3 pivot = ShadowPivot();
  const float px = pivot.x * xx + pivot.y * xy + pivot.z * xz;
  const float py = pivot.x * yx + pivot.y * yy + pivot.z * yz;
  const float pz = pivot.x * dx + pivot.y * dy + pivot.z * dz;

  const float outer = ShadowExtentValue > 1.0f ? ShadowExtentValue : 1.0f;
  // Far enough back that the box always contains the geometry casting into it. `span` is the
  // near-to-far distance, and it is generous rather than fitted because a caster clipped by the
  // near plane stops casting, which reads as a shadow that vanishes when the camera moves.
  FrameSunZNear = pz - outer * 3.0f;
  FrameSunZSpan = outer * 6.0f;

  uint32_t live = static_cast<uint32_t>(ShadowCascadeCount);
  if (live < 1) {
    live = 1;
  }
  if (live > kMaxShadowCascades) {
    live = kMaxShadowCascades;
  }
  for (uint32_t i = 0; i < live; ++i) {
    // Halving outward from the outermost, so `shadow_extent` keeps meaning "where shadows stop"
    // however many cascades are live and cascade 0 is always the sharp one.
    float extent = outer;
    for (uint32_t step = i + 1; step < live; ++step) {
      extent *= 0.5f;
    }
    const float texel = extent * 2.0f / static_cast<float>(kShadowTile);
    const float cx = std::floor(px / texel + 0.5f) * texel;
    const float cy = std::floor(py / texel + 0.5f) * texel;
    FrameCascade[i][0] = cx;
    FrameCascade[i][1] = cy;
    FrameCascade[i][2] = 1.0f / extent;
    // The knob is in texels (SetShadowBias); the shader compares in the shared 0..1 depth range,
    // so each cascade converts with its OWN texel size. That conversion is the whole reason the
    // one number works on every cascade.
    FrameCascade[i][3] = ShadowBiasValue * texel / FrameSunZSpan;

    // view * ortho, multiplied out, for the pass that rasterises this cascade's tile. The ortho
    // is the D3D LH form, whose z already lands in 0..1 - which is Vulkan's range too, so nothing
    // has to be remapped.
    const float sx = 1.0f / extent;
    const float sz = 1.0f / FrameSunZSpan;
    float *c = FrameCascadeMatrix[i];
    c[0] = xx * sx;  c[1] = yx * sx;  c[2] = dx * sz;  c[3] = 0.0f;
    c[4] = xy * sx;  c[5] = yy * sx;  c[6] = dy * sz;  c[7] = 0.0f;
    c[8] = xz * sx;  c[9] = yz * sx;  c[10] = dz * sz; c[11] = 0.0f;
    c[12] = -cx * sx; c[13] = -cy * sx; c[14] = -FrameSunZNear * sz; c[15] = 1.0f;
  }
  FrameCascadeCount = live;
  FrameSunValid = true;
}
} // namespace

void RecordShadowPass(void *command_buffer) {
  auto cmd = static_cast<VkCommandBuffer>(command_buffer);
  // Zeroed on every path that does not render, so `sun shadows: off` is never printed beside a
  // caster count left over from before the knob moved - which reads as the pass still running.
  TheStats.shadow_casters = 0;
  if (!Ready || !ShadowReady || cmd == VK_NULL_HANDLE || !SunShadowsEnabled) {
    FrameSunValid = false;
    return;
  }
  BuildSunCascades();
  if (!FrameSunValid || Items.empty()) {
    return;
  }
  const uint64_t arena_vertices = VertexArenaAddress();
  const uint64_t scratch_vertices = ScratchVertexAddress();
  const uint64_t draw_records = ScratchDrawAddress();
  if (draw_records == 0) {
    FrameSunValid = false;
    return;
  }

  // --- the caster set, culled per cascade (§4.77) ----------------------------------------------
  //
  // Nothing rejected a caster before this: every caster went into every cascade, so the pass was
  // `casters x cascades` however far apart they were. The four cascades differ **only in their
  // x/y half-extent** - 8.75 / 17.5 / 35 / 70 by default, cascade 0 being the sharp near one -
  // while `FrameSunZNear` and `FrameSunZSpan` are shared and deliberately generous, "far enough
  // back that the box always contains the geometry casting into it". That is what makes a plain
  // plane test *exact* here rather than the usual approximation: there is no occluder that is
  // outside the box along the light direction and still casts into it, because the depth range
  // already spans everything.
  //
  // Cascade 0's box is 17.5 world units across on a level spanning well over a hundred, so most
  // of the set cannot touch it. The per-cascade counts in `sun_shadow_report` are that halving,
  // measured rather than assumed.
  std::vector<CasterBucket> buckets;
  std::vector<const DrawItem *> ordered;
  CollectCasters(IsShadowCaster, kShadowMaxCommands, ordered, buckets, SunCastersDropped);
  SunCasterCount = static_cast<uint32_t>(ordered.size());
  SunBucketCount = static_cast<uint32_t>(buckets.size());
  if (ordered.empty()) {
    return;
  }

  // One run per (cascade, bucket), and the arrays behind them. Function-static for the reason the
  // per-frame bake's are: at 8192 commands this is 224 KB and it runs every frame.
  struct SunRun {
    uint32_t first = 0;
    uint32_t count = 0;
  };
  static std::vector<VkDrawIndexedIndirectCommand> commands;
  static std::vector<uint32_t> params;
  static std::vector<const DrawItem *> survivors;
  static std::vector<SunRun> runs;
  commands.clear();
  params.clear();
  survivors.clear();
  runs.clear();
  runs.resize(static_cast<size_t>(FrameCascadeCount) * buckets.size());
  SunCascadeSubmits = 0;
  SunCascadeCulled = 0;
  SunCommandsDropped = 0;
  SunUnbounded = 0;
  for (const DrawItem *item : ordered) {
    if (!item->has_bounds) {
      ++SunUnbounded;
    }
  }
  for (uint32_t i = 0; i < kMaxShadowCascades; ++i) {
    SunCascadeDrawn[i] = 0;
  }
  // `multiDrawIndirect` and a pipeline are what make the indirect path possible; the knob is what
  // makes it chosen. Resolved once here so the build below and the walk above cannot disagree.
  const bool indirect = SunIndirectEnabled && ShadowPipelineIndirect != VK_NULL_HANDLE &&
                        SunIndirectBuffer != VK_NULL_HANDLE;
  for (uint32_t cascade = 0; cascade < FrameCascadeCount; ++cascade) {
    float planes[6][4];
    BuildFrustumPlanes(FrameCascadeMatrix[cascade], planes);
    for (size_t b = 0; b < buckets.size(); ++b) {
      const CasterBucket &bucket = buckets[b];
      SunRun &run = runs[cascade * buckets.size() + b];
      run.first = static_cast<uint32_t>(survivors.size());
      run.count = 0;
      for (uint32_t i = 0; i < bucket.count; ++i) {
        const DrawItem *item = ordered[bucket.first + i];
        // A caster with no world box is drawn into every cascade, unconditionally. That is the
        // only safe reading of "unknown", and `SunUnbounded` is what says how much of the frame
        // is in that state.
        if (SunCullEnabled && item->has_bounds &&
            BoxOutsideFrustum(planes, item->bounds_min, item->bounds_max)) {
          ++SunCascadeCulled;
          continue;
        }
        if (survivors.size() >= kShadowMaxCommands) {
          ++SunCommandsDropped;
          continue;
        }
        if (indirect) {
          VkDrawIndexedIndirectCommand command = {};
          command.indexCount = item->count;
          command.instanceCount = 1;
          command.firstIndex = item->first_index;
          command.vertexOffset = item->vertex_offset;
          command.firstInstance = 0;
          commands.push_back(command);
          params.push_back(item->record);
          params.push_back(item->base_vertex);
        }
        survivors.push_back(item);
        ++run.count;
        ++SunCascadeSubmits;
        if (cascade < kMaxShadowCascades) {
          ++SunCascadeDrawn[cascade];
        }
      }
    }
  }
  if (survivors.empty()) {
    // Legal: a camera whose whole caster set is outside every cascade. The atlas still has to be
    // cleared, or it keeps the previous frame's cascades and the world pass samples them - so the
    // pass below runs and only the batch is skipped.
    FrameSunValid = true;
  }

  // This frame's slice of the ring, so the transfer cannot land on bytes an in-flight frame's
  // indirect draws are still reading - the hazard `kMapIndirectRing` documents.
  ++SunRingSerial;
  const VkDeviceSize slice =
      static_cast<VkDeviceSize>(SunRingSerial % kShadowIndirectRing) * kShadowIndirectSlice;
  if (indirect && !commands.empty()) {
    // Chunked, because the batch is allowed past the 64 KB one `vkCmdUpdateBuffer` takes.
    UpdateBufferChunked(cmd, SunIndirectBuffer, slice, commands.data(),
                        commands.size() * sizeof(VkDrawIndexedIndirectCommand));
    UpdateBufferChunked(cmd, SunIndirectBuffer, slice + kShadowParamOffset, params.data(),
                        params.size() * sizeof(uint32_t));
    // Two destinations, as §4.62: the commands are read by DRAW_INDIRECT and the parameters by
    // the vertex shader as an address, and only the first is what a transfer barrier assumes.
    VkMemoryBarrier2 written = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    written.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    written.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    written.dstStageMask =
        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    written.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo upload = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    upload.memoryBarrierCount = 1;
    upload.pMemoryBarriers = &written;
    vkCmdPipelineBarrier2(cmd, &upload);
  }

  VkImageMemoryBarrier2 to_attachment = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  to_attachment.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  to_attachment.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
  to_attachment.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  to_attachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_attachment.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  to_attachment.image = ShadowImage;
  to_attachment.subresourceRange.aspectMask =
      VK_IMAGE_ASPECT_DEPTH_BIT | (ShadowStencilAspect ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
  to_attachment.subresourceRange.levelCount = 1;
  to_attachment.subresourceRange.layerCount = 1;
  VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &to_attachment;
  vkCmdPipelineBarrier2(cmd, &dependency);

  VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = ShadowAttachmentView;
  attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; // the whole point, unlike the world pass
  attachment.clearValue.depthStencil.depth = 1.0f;

  // One pass over the whole atlas, cleared once; each cascade is then a viewport and a scissor
  // into its own tile. Four `vkCmdBeginRendering`s would clear four times and barrier three times
  // for nothing - the tiles do not overlap, so a scissor is the whole of the isolation needed.
  VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea.extent = {kShadowAtlas, kShadowAtlas};
  rendering.layerCount = 1;
  rendering.pDepthAttachment = &attachment;
  vkCmdBeginRendering(cmd, &rendering);
  // The whole pass takes one pipeline or the other: this walk draws every caster, and the
  // tessellated twin is a different pipeline rather than a per-draw state. See the note on
  // `render.tess_set` in VkDraw.h for what that costs when the colour pass tessellates only the
  // map - a prop's shadow follows a smoothed silhouette its geometry does not have.
  // Two axes, so four pipelines: which submission path, and whether the patch is amplified. The
  // tessellated twin is only taken when it actually built - a create failure there leaves the
  // untessellated one of the same path bound rather than dropping the pass or, worse, crossing
  // the two paths' entry points.
  const bool tessellating =
      ShadowTessellating() && (indirect ? ShadowPipelineIndirectTess != VK_NULL_HANDLE
                                        : ShadowPipelineTess != VK_NULL_HANDLE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    tessellating ? (indirect ? ShadowPipelineIndirectTess : ShadowPipelineTess)
                                 : (indirect ? ShadowPipelineIndirect : ShadowPipeline));

  ShadowPushConstants push = {};
  push.draws = draw_records;
  FillShadowTessPush(push);

  VkBuffer bound_index = VK_NULL_HANDLE;
  uint64_t casters = 0;
  SunDrawCalls = 0;
  for (uint32_t cascade = 0; cascade < FrameCascadeCount; ++cascade) {
    // The 2x2 tile this cascade owns. Must agree with the shader's atlas offset, which derives it
    // the same way from the same index - see `sun_visibility` in world.slang.
    const float tx = static_cast<float>((cascade & 1u) * kShadowTile);
    const float ty = static_cast<float>((cascade >> 1) * kShadowTile);
    VkViewport viewport = {tx, ty, float(kShadowTile), float(kShadowTile), 0.0f, 1.0f};
    VkRect2D scissor = {{int32_t(tx), int32_t(ty)}, {kShadowTile, kShadowTile}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    std::memcpy(push.light_matrix, FrameCascadeMatrix[cascade], sizeof(push.light_matrix));

    for (size_t b = 0; b < buckets.size(); ++b) {
      const CasterBucket &bucket = buckets[b];
      const SunRun &run = runs[cascade * buckets.size() + b];
      if (run.count == 0) {
        continue;
      }
      const VkBuffer index_buffer = bucket.index_source == DrawSource::Arena
                                        ? reinterpret_cast<VkBuffer>(IndexArenaBuffer())
                                        : reinterpret_cast<VkBuffer>(ScratchIndexBuffer());
      const VkIndexType type =
          bucket.index_stride == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
      if (index_buffer != bound_index) {
        vkCmdBindIndexBuffer(cmd, index_buffer, 0, type);
        bound_index = index_buffer;
      }
      push.vertices =
          bucket.vertex_source == DrawSource::Arena ? arena_vertices : scratch_vertices;
      casters += run.count;
      if (!indirect) {
        // The direct path, unchanged in what it draws: the record and the arena slot ride in the
        // push. It walks `survivors`, which is index-parallel to `commands`, so the two paths
        // submit the same set by construction rather than by two copies of the cull agreeing.
        for (uint32_t i = 0; i < run.count; ++i) {
          const DrawItem *item = survivors[run.first + i];
          push.record = item->record;
          push.base_vertex = item->base_vertex;
          vkCmdPushConstants(cmd, ShadowLayout, kShadowPushStages, 0, sizeof(push), &push);
          vkCmdDrawIndexed(cmd, item->count, 1, item->first_index, item->vertex_offset, 0);
          ++SunDrawCalls;
        }
        continue;
      }
      // Each run's parameters start where its commands do, so `SV_DrawIndex` - which counts from
      // 0 within one `vkCmdDrawIndexedIndirect` - indexes its own run's slice.
      push.params = SunIndirectAddress + slice + kShadowParamOffset + run.first * 8;
      vkCmdPushConstants(cmd, ShadowLayout, kShadowPushStages, 0, sizeof(push), &push);
      vkCmdDrawIndexedIndirect(cmd, SunIndirectBuffer,
                               slice + static_cast<VkDeviceSize>(run.first) * kShadowIndirectStride,
                               run.count, kShadowIndirectStride);
      ++SunDrawCalls;
    }
  }
  vkCmdEndRendering(cmd);
  // Across every cascade, so it is the pass's cost rather than the scene's caster count - divide
  // by the live cascade count for the latter, or read `sun_shadow_report`, which now splits it.
  TheStats.shadow_casters = casters;

  VkImageMemoryBarrier2 to_read = to_attachment;
  to_read.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  to_read.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  to_read.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  to_read.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  dependency.pImageMemoryBarriers = &to_read;
  vkCmdPipelineBarrier2(cmd, &dependency);
}

// Ambient occlusion, in two recorded passes (§4.86).
//
// Outside any render pass on entry and outside one on exit, like the three passes beside it in
// DrawFrame - it begins and ends its own.
void RecordAoPass(void *command_buffer) {
  auto cmd = static_cast<VkCommandBuffer>(command_buffer);
  AoDrawCalls = 0;
  AoCastersDropped = 0;
  AoRanThisFrame = false;
  if (!Ready || !AoReady || !AoEnabled || cmd == VK_NULL_HANDLE || Items.empty()) {
    return;
  }
  const uint64_t arena_vertices = VertexArenaAddress();
  const uint64_t scratch_vertices = ScratchVertexAddress();
  const uint64_t draw_records = ScratchDrawAddress();
  if (draw_records == 0) {
    return;
  }

  // **The same caster set the sun's shadow uses**, through the same collector - "indexed, opaque,
  // writes depth", which is the frame's solid geometry. That it is the same predicate is the
  // point: an occluder for the sun is an occluder here, and two definitions of "solid" that could
  // drift is exactly what CollectCasters was factored out to prevent.
  //
  // The bucketing is what the collector is really for - a bucket is a contiguous run sharing an
  // index buffer and stride - and the sort it does is safe here for the same reason it is safe
  // there: this pass resolves to the nearest surface per pixel, and a minimum does not care what
  // order it was taken in.
  std::vector<CasterBucket> buckets;
  std::vector<const DrawItem *> ordered;
  CollectCasters(IsShadowCaster, kShadowMaxCommands, ordered, buckets, AoCastersDropped);
  if (ordered.empty()) {
    return;
  }

  // Position and normal to COLOR_ATTACHMENT, depth to DEPTH_ATTACHMENT. UNDEFINED as the old
  // layout on all three: every one of them is fully overwritten by a clear load op below, so
  // preserving the previous frame's contents would be paying to decompress something about to be
  // thrown away.
  VkImageMemoryBarrier2 to_attachment[3] = {};
  for (VkImageMemoryBarrier2 &barrier : to_attachment) {
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
  }
  to_attachment[0].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  to_attachment[0].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  to_attachment[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  to_attachment[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  to_attachment[0].image = AoPositionImage;
  to_attachment[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  to_attachment[1] = to_attachment[0];
  to_attachment[1].image = AoNormalImage;
  to_attachment[2].srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  to_attachment[2].dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
  to_attachment[2].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  to_attachment[2].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  to_attachment[2].image = AoDepthImage;
  to_attachment[2].subresourceRange.aspectMask = DepthAspect();
  VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 3;
  dependency.pImageMemoryBarriers = to_attachment;
  vkCmdPipelineBarrier2(cmd, &dependency);

  VkRenderingAttachmentInfo colour[2] = {};
  for (VkRenderingAttachmentInfo &attachment : colour) {
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  }
  colour[0].imageView = AoPositionView;
  // **Cleared to zero, and the `w` of that zero is the whole uncovered test.** The resolve reads
  // `w == 0` as "nothing was drawn at this pixel" and leaves it unoccluded, which covers the sky,
  // the pixels only a pre-transformed draw touches, and every tap that leaves the frame - Load
  // returns zero out of bounds, so the border needs no clamp of its own.
  colour[1].imageView = AoNormalView;
  VkRenderingAttachmentInfo depth_attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth_attachment.imageView = AoDepthView;
  depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  // DONT_CARE: this depth buffer exists to resolve the visible surface within this pass and is
  // read by nothing afterwards. The world pass has its own and clears it again.
  depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth_attachment.clearValue.depthStencil = {1.0f, 0};

  VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea.extent = {AoWidth, AoHeight};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 2;
  rendering.pColorAttachments = colour;
  rendering.pDepthAttachment = &depth_attachment;
  vkCmdBeginRendering(cmd, &rendering);

  // **The same half-pixel viewport origin the world pass uses** (§4.28). Not cosmetic: it is what
  // makes pixel (i,j) of these targets the same surface point as pixel (i,j) of the frame, which
  // is the assumption the world shader's `Load` rests on. Off by half a pixel the occlusion would
  // still look plausible and would be consistently misregistered against every edge in the frame.
  const float origin = ViewportOrigin();
  VkViewport viewport = {origin, origin, static_cast<float>(AoWidth),
                         static_cast<float>(AoHeight), 0.0f, 1.0f};
  VkRect2D scissor = {{0, 0}, {AoWidth, AoHeight}};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, AoPrepassPipeline);

  AoPushConstants push = {};
  push.draws = draw_records;
  VkBuffer bound_index = VK_NULL_HANDLE;
  for (const CasterBucket &bucket : buckets) {
    const VkBuffer index_buffer = bucket.index_source == DrawSource::Arena
                                      ? reinterpret_cast<VkBuffer>(IndexArenaBuffer())
                                      : reinterpret_cast<VkBuffer>(ScratchIndexBuffer());
    const VkIndexType type =
        bucket.index_stride == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    if (index_buffer != bound_index) {
      vkCmdBindIndexBuffer(cmd, index_buffer, 0, type);
      bound_index = index_buffer;
    }
    push.vertices = bucket.vertex_source == DrawSource::Arena ? arena_vertices : scratch_vertices;
    for (uint32_t i = 0; i < bucket.count; ++i) {
      const DrawItem *item = ordered[bucket.first + i];
      push.record = item->record;
      push.base_vertex = item->base_vertex;
      vkCmdPushConstants(cmd, AoLayout,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(push), &push);
      vkCmdDrawIndexed(cmd, item->count, 1, item->first_index, item->vertex_offset, 0);
      ++AoDrawCalls;
    }
  }
  vkCmdEndRendering(cmd);

  // The prepass's two targets become the resolve's two inputs.
  VkImageMemoryBarrier2 to_read[3] = {};
  to_read[0] = to_attachment[0];
  to_read[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  to_read[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  to_read[0].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  to_read[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  to_read[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  to_read[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  to_read[1] = to_read[0];
  to_read[1].image = AoNormalImage;
  // ... and the result becomes an attachment, in the same barrier rather than a second one.
  to_read[2] = to_attachment[0];
  to_read[2].image = AoResultImage;
  dependency.pImageMemoryBarriers = to_read;
  vkCmdPipelineBarrier2(cmd, &dependency);

  VkRenderingAttachmentInfo result = colour[0];
  result.imageView = AoResultView;
  // LOAD_OP_DONT_CARE: the resolve writes every pixel of the target unconditionally - the
  // "nothing here" case returns 1.0 rather than skipping - so a clear would be a second write of
  // the whole image.
  result.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &result;
  rendering.pDepthAttachment = nullptr;
  vkCmdBeginRendering(cmd, &rendering);
  // **A zero origin here, not the half-pixel one.** The resolve's geometry is a full-screen
  // triangle in clip space and its fragment reads its own integer `SV_Position`; shifting the
  // viewport would move every pixel's idea of which texel it is by half a pixel, which is the one
  // way to undo the registration the prepass just took care to get right.
  VkViewport full = {0.0f, 0.0f, static_cast<float>(AoWidth), static_cast<float>(AoHeight),
                     0.0f, 1.0f};
  vkCmdSetViewport(cmd, 0, 1, &full);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, AoResolvePipeline);
  auto set = reinterpret_cast<VkDescriptorSet>(BindlessDescriptorSet());
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, AoLayout, 0, 1, &set, 0, nullptr);
  push.position_texture = kAoPositionSlot;
  push.normal_texture = kAoNormalSlot;
  push.radius = AoRadiusValue;
  // Height and not width, so the disc is the same shape and the same world size whatever the
  // aspect ratio is - a fraction of the width would make a 16:9 frame's occlusion radius differ
  // from a 4:3 one's at the same setting.
  push.screen_radius = AoScreenRadiusValue * static_cast<float>(AoHeight);
  push.bias = AoBiasValue;
  push.strength = AoStrengthValue;
  push.taps = AoTapsValue;
  vkCmdPushConstants(cmd, AoLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                     sizeof(push), &push);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);

  VkImageMemoryBarrier2 result_read = to_read[0];
  result_read.image = AoResultImage;
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &result_read;
  vkCmdPipelineBarrier2(cmd, &dependency);
  AoRanThisFrame = true;
}

namespace {
// Which lights get a slot, when the atlas holds fewer than the level has.
//
// **Ordered by `brightness * range^3`**, which is the fitted model's own answer to "how much light
// does this one put into the world": its falloff is linear in range, so the volume integral of a
// light's contribution goes as the cube of it. That is a property of §4.54's model rather than a
// heuristic, and it is why the cut is here rather than on brightness alone.
void AssignMapShadowSlots() {
  const std::vector<MapLight> &lights = MapLights();
  MapShadowSlotForLight.assign(lights.size(), -1);
  MapShadowLightForSlot.clear();
  MapShadowRefused = 0;
  if (lights.empty()) {
    return;
  }
  std::vector<uint32_t> order(lights.size());
  for (uint32_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  const auto influence = [&lights](uint32_t i) {
    const float range = lights[i].range;
    return lights[i].brightness * range * range * range;
  };
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    // The index breaks a tie, so the assignment is the same on every run of a level rather than
    // depending on how the sort happened to partition equal keys.
    const float ia = influence(a), ib = influence(b);
    return ia != ib ? ia > ib : a < b;
  });
  // **The map lights' budget, not the whole atlas.** The last `kLocalShadowSlots` tiles belong to
  // D3D's own point and spot lights (§4.65), so the cut here is sixteen lights earlier than it was
  // - which only level01 reaches, and only at the bottom of the influence order.
  const uint32_t capacity = kMapShadowLightSlots;
  for (uint32_t i = 0; i < order.size(); ++i) {
    if (i >= capacity) {
      ++MapShadowRefused;
      continue;
    }
    MapShadowSlotForLight[order[i]] = static_cast<int32_t>(MapShadowLightForSlot.size());
    MapShadowLightForSlot.push_back(order[i]);
  }
}

// World -> one cube face's clip space for one light, row-vector like every other matrix here.
//
// A standard left-handed perspective at 90 degrees with a square aspect, so `x/z` and `y/z` land
// in -1..1 across exactly one face and six of them tile the sphere. The lookup in world.slang
// re-derives the same quantities from the same face table rather than from this matrix, which is
// what makes the two halves comparable line by line.
//
// Taken as a position and a range rather than as a `MapLight`, because a D3D point light gets a
// cube out of the same function (§4.65) - and the two **must** use one, or the lookup that serves
// both would be projecting against two slightly different frusta.
void BuildCubeFaceMatrix(const Vec3 &position, float range, uint32_t face, float *m) {
  const float *R = kFaceRight[face];
  const float *U = kFaceUp[face];
  const float *F = kFaceForward[face];
  const float far_plane = range > 1e-3f ? range : 1e-3f;
  const float near_plane = far_plane * kMapShadowNear;
  const float a = far_plane / (far_plane - near_plane);
  const float b = near_plane * a;
  const float lr = position.x * R[0] + position.y * R[1] + position.z * R[2];
  const float lu = position.x * U[0] + position.y * U[1] + position.z * U[2];
  const float lf = position.x * F[0] + position.y * F[1] + position.z * F[2];
  m[0] = R[0];      m[1] = U[0];      m[2] = F[0] * a;        m[3] = F[0];
  m[4] = R[1];      m[5] = U[1];      m[6] = F[1] * a;        m[7] = F[1];
  m[8] = R[2];      m[9] = U[2];      m[10] = F[2] * a;       m[11] = F[2];
  m[12] = -lr;      m[13] = -lu;      m[14] = -lf * a - b;    m[15] = -lf;
}

// Forget every local key. A level change invalidates all of them - the geometry that occludes
// them is gone - and so does turning the feature off, which must leave no half-baked slot behind.
void ResetLocalShadows() {
  LocalShadowKeys.clear();
  LocalShadowPending.clear();
  for (const LocalShadowKey *&owner : LocalShadowOwner) {
    owner = nullptr;
  }
  LocalShadowForgotten = 0;
  LocalShadowBakes = 0;
}

// Every local cube the atlas holds dies with the clear the caller is about to schedule, so no entry
// may go on claiming to be baked. **This is the local half of `MapShadowCursor = 0`** - the map's
// re-queue has been there since §4.61, and the absence of this one is what made a forced re-bake
// lose the local cubes for the rest of the level: an entry already carrying `baked` is never pushed
// back onto `LocalShadowPending` by the acquire path, which only queues a slot at the moment it is
// claimed. Measured on level02 before this existed - the boot frame against a re-baked one differed
// by 38,335 pixels at a max delta of 53, and `local_shadow_report` read "baked and sampled: 6"
// against "cubes baked for this level: 6", i.e. not one cube after the clear. With this it is 0
// pixels and 6 -> 12 cubes.
//
// **Keys are kept, not forgotten.** A re-bake is not a level change: the geometry these cubes hold
// is the same geometry, and the lights are the same lights, so throwing the table away would make
// each of them serve out the four-frame stability gate again for no reason. Only the *contents* of
// the atlas are gone.
//
// The pending list is rebuilt rather than appended to, so a slot already queued is not baked twice.
// Each slot appears at most once because an eviction clears the previous owner's `slot` before the
// new one takes it.
void RequeueLocalShadows() {
  LocalShadowPending.clear();
  for (auto &[key, entry] : LocalShadowKeys) {
    entry.baked = false;
    if (entry.slot >= 0) {
      LocalShadowPending.push_back(static_cast<uint32_t>(entry.slot));
    }
  }
}
} // namespace

// See VkDraw.h. Called once per distinct light run, from the capture layer's StoreLight - which is
// the only place a D3D light is in hand at all.
int32_t AcquireLocalShadowSlot(const LocalShadowKey &key, uint64_t frame) {
  if (!Ready || !MapShadowReady || !LocalShadowsEnabled()) {
    return -1;
  }
  // The same identity the map half keys on, and for the same reason: the level's geometry is what
  // these cubes hold, so a level change invalidates every one of them. `MapLightsGeneration()`
  // moves on a level change and on nothing else.
  const uint32_t generation = MapLightsGeneration();
  if (LocalShadowBuiltForGeneration != generation) {
    LocalShadowBuiltForGeneration = generation;
    ResetLocalShadows();
  }
  // A directional light has no position to build a cube around, and the sun's cascades already
  // cover it. Nothing here should ever see one - the caller filters - but a `range` of 0 would
  // divide by itself in the projection, so it is refused rather than trusted.
  float range = 0.0f;
  std::memcpy(&range, &key.range, sizeof(range));
  if (key.type == 3 /* D3DLIGHT_DIRECTIONAL */ || !(range > 1e-3f)) {
    return -1;
  }

  auto found = LocalShadowKeys.find(key);
  if (found == LocalShadowKeys.end()) {
    LocalShadowEntry fresh;
    fresh.last_seen = frame;
    fresh.stable = 1;
    fresh.key = key;
    LocalShadowKeys.emplace(key, fresh);
    // Housekeeping, and it is what keeps a MOVING light from filling this table without bound:
    // a light on a track leaves one dead key a frame behind it. Anything not asked for in a while
    // goes, and a dead key holding a slot hands it back.
    if (LocalShadowKeys.size() > 4 * kLocalShadowSlots) {
      for (auto it = LocalShadowKeys.begin(); it != LocalShadowKeys.end();) {
        if (it->second.slot < 0 && frame - it->second.last_seen > 120) {
          ++LocalShadowForgotten;
          it = LocalShadowKeys.erase(it);
        } else {
          ++it;
        }
      }
    }
    return -1;
  }

  LocalShadowEntry &entry = found->second;
  if (entry.last_seen != frame) {
    ++entry.stable;
    entry.last_seen = frame;
  }
  if (entry.slot >= 0) {
    return entry.baked ? static_cast<int32_t>(kMapShadowLightSlots) + entry.slot : -1;
  }
  // **The stability gate**, and it is the whole handling of a light that moves: a new key every
  // frame never gets here twice, so it never claims a slot and never costs a bake.
  if (entry.stable < kLocalShadowStableFrames) {
    return -1;
  }

  int32_t slot = -1;
  for (uint32_t i = 0; i < kLocalShadowSlots; ++i) {
    if (LocalShadowOwner[i] == nullptr) {
      slot = static_cast<int32_t>(i);
      break;
    }
  }
  if (slot < 0) {
    // Least recently seen, and **never one seen this frame** - evicting a light that is lit right
    // now would take its shadow away and re-bake it on the next, forever.
    uint64_t oldest = frame;
    for (uint32_t i = 0; i < kLocalShadowSlots; ++i) {
      const auto owner = LocalShadowKeys.find(*LocalShadowOwner[i]);
      if (owner != LocalShadowKeys.end() && owner->second.last_seen < oldest) {
        oldest = owner->second.last_seen;
        slot = static_cast<int32_t>(i);
      }
    }
    if (slot < 0) {
      // Sixteen lights all live this frame, so nothing may be taken: this one goes unshadowed.
      // **Refusing rather than evicting a light seen this frame is what stops a thrash** - with
      // more qualified lights than slots, evicting the least-recent would re-bake six faces every
      // frame forever. Measured: 33 keys against 16 slots baked 38 cubes in total, not 38 a frame.
      return -1;
    }
    const auto owner = LocalShadowKeys.find(*LocalShadowOwner[slot]);
    if (owner != LocalShadowKeys.end()) {
      owner->second.slot = -1;
      owner->second.baked = false;
    }
  }
  entry.slot = slot;
  entry.baked = false;
  LocalShadowOwner[slot] = &found->first;
  LocalShadowPending.push_back(static_cast<uint32_t>(slot));
  return -1; // unshadowed until the bake reaches it, which is one frame at the default rate
}

// See VkDraw.h. Called from the capture layer's StoreLight, once per distinct light run.
//
// **The whole per-frame design is these fifteen lines.** There is no identity to establish, so
// there is nothing to get wrong about one: the table is this frame's, the slot is the index, and
// the next frame starts empty.
int32_t RegisterDynamicShadowLight(const LocalShadowKey &key, uint64_t frame) {
  if (!Ready || !DynShadowReady || !DynamicShadowsEnabled) {
    return -1;
  }
  float range = 0.0f;
  std::memcpy(&range, &key.range, sizeof(range));
  if (key.type == 3 /* D3DLIGHT_DIRECTIONAL */ || !(range > 1e-3f)) {
    return -1; // the sun's cascades cover a directional, and it has no position to build a cube on
  }
  if (frame != DynFrame) {
    DynFrame = frame;
    DynLights.clear();
    DynLightSlots.clear();
    DynRefused = 0;
  }
  // Deduplicated **within the frame only**, which is all "the same light" has to mean here. The
  // capture layer already dedups a light *run* by enable mask, but two runs can share a light and
  // the same light must not get two cubes.
  const auto found = DynLightSlots.find(key);
  if (found != DynLightSlots.end()) {
    return found->second;
  }
  if (DynLights.size() >= kDynShadowSlots) {
    ++DynRefused; // it falls back to its static cube if it has one, and is unshadowed if not
    return -1;
  }
  const auto slot = static_cast<int32_t>(DynLights.size());
  DynLights.push_back(key);
  DynLightSlots.emplace(key, slot);
  return slot;
}

namespace {
// **The map object, and not a prop or a unit** - declared here, defined below.
bool IsMapGeometry(const DrawItem &item);

// See the declaration above CreateShadowPass for what the sort is and why it is only safe for a
// depth-only pass.
//
// A source that is not resident is skipped rather than dropped: `vertices == 0` or a null index
// buffer means the arena or the scratch is not up, which is a state the whole pass is about to
// find out about anyway - it is not a caster that could not be carried, so it does not count
// against the cap or the drop counter.
void CollectCasters(bool (*accept)(const DrawItem &), uint32_t limit,
                    std::vector<const DrawItem *> &ordered, std::vector<CasterBucket> &buckets,
                    uint32_t &dropped) {
  const uint64_t arena_vertices = VertexArenaAddress();
  const uint64_t scratch_vertices = ScratchVertexAddress();
  buckets.clear();
  ordered.clear();
  ordered.reserve(Items.size());
  dropped = 0;
  const auto find_bucket = [&buckets](const DrawItem &item) -> CasterBucket * {
    for (CasterBucket &candidate : buckets) {
      if (candidate.vertex_source == item.vertex_source &&
          candidate.index_source == item.index_source &&
          candidate.index_stride == item.index_stride) {
        return &candidate;
      }
    }
    return nullptr;
  };
  for (const DrawItem &item : Items) {
    if (!accept(item)) {
      continue;
    }
    const uint64_t vertices =
        item.vertex_source == DrawSource::Arena ? arena_vertices : scratch_vertices;
    const VkBuffer index_buffer = item.index_source == DrawSource::Arena
                                      ? reinterpret_cast<VkBuffer>(IndexArenaBuffer())
                                      : reinterpret_cast<VkBuffer>(ScratchIndexBuffer());
    if (vertices == 0 || index_buffer == VK_NULL_HANDLE) {
      continue;
    }
    if (ordered.size() >= limit) {
      ++dropped;
      continue;
    }
    CasterBucket *bucket = find_bucket(item);
    if (bucket == nullptr) {
      buckets.push_back({item.vertex_source, item.index_source, item.index_stride, 0, 0});
      bucket = &buckets.back();
    }
    ++bucket->count;
    ordered.push_back(&item);
  }
  if (ordered.empty()) {
    return;
  }
  // Lay the buckets out contiguously and **reorder `ordered` to match**, so a bucket is a
  // contiguous run of it and `ordered[bucket.first + i]` means what it reads as.
  //
  // It did not, before, and the per-frame bake's direct path drew the wrong geometry for it: the
  // commands were placed at `bucket.first + bucket.count++` while `ordered` stayed in draw-list
  // order, and `!DynShadowIndirect` indexed `ordered` by the bucket layout anyway. Every counter
  // read correctly, because the *number* of draws was right.
  uint32_t next = 0;
  for (CasterBucket &bucket : buckets) {
    bucket.first = next;
    next += bucket.count;
    bucket.count = 0; // refilled as the casters are placed, so `first + count` stays the cursor
  }
  std::vector<const DrawItem *> by_bucket(ordered.size());
  for (const DrawItem *item : ordered) {
    CasterBucket *bucket = find_bucket(*item);
    by_bucket[bucket->first + bucket->count++] = item;
  }
  ordered.swap(by_bucket);
}

bool IsDynamicCaster(const DrawItem &item) {
  if (DynCasterArenaOnly &&
      (item.vertex_source != DrawSource::Arena || item.index_source != DrawSource::Arena)) {
    return false;
  }
  // **`render.dynamic_shadow_map_only` narrows the set to §4.65's exactly**, which is what prices
  // this feature's second half: the difference between the two is the props and the units, and
  // nothing else. `arena_only` looks like it should answer the same question and does not - a unit
  // draws from the arena as often as not (150 casters in ONE bucket at level02's fires), so it
  // separates the user-pointer draws rather than the mobile things.
  if (DynCasterMapOnly) {
    return IsMapGeometry(item);
  }
  return IsShadowCaster(item);
}

// **The map object, and not a prop or a unit.** The occluders for a level's own light rig are the
// level's own geometry: a unit walks away from the shadow it baked, and a prop carries its own
// file's rig anyway (§4.55). The marker is §4.51's - two texture stages, and stage 1 is not the
// chrome sphere map - which is exactly the set `SubmitAndFlushMapGeometry` submits.
//
// Tighter than the fragment shader's `stage_count == 2 && chrome == 0`, deliberately: that one
// only sets `chrome` on a material that also carries a lighting map, because everything it gates
// is derived from one. Here the question is what the geometry *is*, so it asks the texture.
bool IsMapGeometry(const DrawItem &item) {
  if (item.stage_count != 2 || !item.indexed) {
    return false;
  }
  if (item.pipeline.blend_enable || !item.pipeline.depth_write) {
    return false;
  }
  // **Both sources have to be the arena**, which is a real restriction and not a formality: one
  // indirect batch has one index buffer bound and one `vertices` address in the push, so a caster
  // living in the frame's scratch could not join it. The map's geometry is buffered by the game
  // and has never been anything else - `map casters dropped` in the report is what would say
  // otherwise rather than a silently smaller shadow.
  if (item.vertex_source != DrawSource::Arena || item.index_source != DrawSource::Arena) {
    return false;
  }
  return !IsChromeTexture(item.stages[1].texture_index);
}

// Whether this draw takes the PN-triangle pipeline (§4.71).
//
// **The triangle-list requirement is not a formality**: a patch list consumes three indices per
// patch, so reinterpreting a strip as one would draw a third of the triangles at arbitrary
// corners. Nothing in either set is a strip today - `render.draws` reports 0 topology skips - and
// this is what keeps that true if something ever is.
//
// The wider set spells its condition out rather than calling `IsDynamicCaster`, deliberately.
// That predicate is gated on `render.dynamic_shadow_map_only` and `_arena_only`, which belong to
// the per-frame shadow atlas; routing this through it would make a shadow knob silently change
// which draws get tessellated, and an A/B on one feature would move the other.
bool WantsTessellation(const DrawItem &item) {
  if (!TessellationEnabled() || item.pipeline.topology != 4 /* D3DPT_TRIANGLELIST */) {
    return false;
  }
  switch (TessellationSet()) {
  case TessSet::Map:
    return IsMapGeometry(item);
  case TessSet::All:
    return item.indexed && !item.pipeline.blend_enable && item.pipeline.depth_write;
  case TessSet::Off:
  default:
    return false;
  }
}
} // namespace

void BakeMapShadows(void *command_buffer) {
  auto cmd = static_cast<VkCommandBuffer>(command_buffer);
  if (!Ready || !MapShadowReady || cmd == VK_NULL_HANDLE) {
    return;
  }
  const std::vector<MapLight> &lights = MapLights();
  // **`MapLightsGeneration()` and not the light count**, which is what the grid keys on: the count
  // is the same for two levels that happen to have the same number of lights, and the scratch
  // address the lights ride in changes every frame, so neither is the identity of a light *set*.
  // The generation moves on a level change and on nothing else.
  const uint32_t generation = MapLightsGeneration();
  if (MapShadowBuiltForGeneration != generation) {
    MapShadowBuiltForGeneration = generation;
    AssignMapShadowSlots();
    MapShadowCursor = 0;
    MapShadowDraws = 0;
    MapShadowAtlasCleared = false;
    // **Both halves, or the clear below takes tiles nothing will redraw.** A level change reaches
    // the local half through `AcquireLocalShadowSlot`'s own generation test as well, but the two
    // knobs that force a re-bake - `map_shadow_indirect` and `map_shadow_rate`, both of which
    // wind `MapShadowBuiltForGeneration` back to 0 without the generation moving - reach it only
    // here. Before the bake's early-out, so a re-queued slot makes `local_work` true and the clear
    // and the redraw land in one pass rather than leaving a frame of missing shadow between them.
    RequeueLocalShadows();
  }
  // **Gated on the knob, and after the slot table above rather than before it.** Baking an atlas
  // nothing samples costs 1.9 seconds of GPU time on level01 (§4.61), so `off` has to mean off -
  // but the table has to be rebuilt on a level change either way, or turning the knob on later
  // would bake against the previous level's assignment.
  const bool map_work = MapShadowsEnabled && !lights.empty() &&
                        MapShadowCursor < MapShadowLightForSlot.size();
  // The local half (§4.65) has its own queue and its own knob, and either producer is reason
  // enough to open the pass - the two write disjoint tiles of one image.
  const bool local_work = LocalShadowsEnabled() && !LocalShadowPending.empty();
  if (!map_work && !local_work) {
    return; // switched off, nothing to bake, or the bake finished for this level
  }
  const uint64_t arena_vertices = VertexArenaAddress();
  const uint64_t draw_records = ScratchDrawAddress();
  auto index_buffer = reinterpret_cast<VkBuffer>(IndexArenaBuffer());
  if (draw_records == 0 || arena_vertices == 0 || index_buffer == VK_NULL_HANDLE ||
      Items.empty()) {
    return;
  }
  // The map's own draws, gathered once for the whole slice - the test is per draw and the slice
  // renders the same set `rate * 6` times.
  std::vector<const DrawItem *> casters;
  MapShadowCastersDropped = 0;
  for (const DrawItem &item : Items) {
    if (!IsMapGeometry(item)) {
      continue;
    }
    if (casters.size() >= kMaxMapCasters) {
      ++MapShadowCastersDropped;
      continue;
    }
    casters.push_back(&item);
  }
  if (casters.empty()) {
    return; // no map geometry in this frame - the briefing screen, or a level still loading
  }
  // **One index type for the whole batch**, since a batch has one bound index buffer. Everything
  // in the arena is 16-bit today (`0 32-bit index buffer` in `render.draws`); a 32-bit caster
  // would have to be dropped rather than drawn with the wrong stride.
  const VkIndexType index_type =
      casters[0]->index_stride == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
  for (size_t i = casters.size(); i-- > 0;) {
    if ((casters[i]->index_stride == 4) != (index_type == VK_INDEX_TYPE_UINT32)) {
      casters.erase(casters.begin() + static_cast<ptrdiff_t>(i));
      ++MapShadowCastersDropped;
    }
  }

  // The batch and its parameters, rebuilt every slice because `record` indexes the frame's own
  // scratch and that rotates. Written straight into the command buffer - 213 casters is 4.3 KB.
  //
  // Into this bake's slice of the ring, so the transfer cannot land on bytes an in-flight frame's
  // indirect draws are still reading. Read again below by the draw and by the push, which is why
  // it outlives the block that computes it.
  VkDeviceSize slice = 0;
  if (MapShadowIndirect) {
    ++MapRingSerial;
    slice = static_cast<VkDeviceSize>(MapRingSerial % kMapIndirectRing) * kMapIndirectSlice;
    std::vector<VkDrawIndexedIndirectCommand> commands(casters.size());
    std::vector<uint32_t> params(casters.size() * 2);
    for (size_t i = 0; i < casters.size(); ++i) {
      commands[i].indexCount = casters[i]->count;
      commands[i].instanceCount = 1;
      commands[i].firstIndex = casters[i]->first_index;
      commands[i].vertexOffset = casters[i]->vertex_offset;
      commands[i].firstInstance = 0;
      params[i * 2 + 0] = casters[i]->record;
      params[i * 2 + 1] = casters[i]->base_vertex;
    }
    vkCmdUpdateBuffer(cmd, MapIndirectBuffer, slice,
                      commands.size() * sizeof(VkDrawIndexedIndirectCommand), commands.data());
    vkCmdUpdateBuffer(cmd, MapIndirectBuffer, slice + kMapParamOffset,
                      params.size() * sizeof(uint32_t), params.data());
    // Both halves, and they need different destinations: the commands are consumed by the
    // DRAW_INDIRECT stage and the parameters by the vertex shader reading them as an address.
    VkMemoryBarrier2 written = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    written.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    written.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    written.dstStageMask =
        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    written.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo upload = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    upload.memoryBarrierCount = 1;
    upload.pMemoryBarriers = &written;
    vkCmdPipelineBarrier2(cmd, &upload);
  }

  // **Cleared exactly once per level, by whichever half gets here first.** It used to be
  // `MapShadowCursor == 0`, which was the same thing while the map lights were the only producer -
  // with the local half beside them (§4.65) that reading clears the whole atlas again the first
  // time a D3D light claims a tile, erasing every map cube baked before it.
  const bool first = !MapShadowAtlasCleared;
  MapShadowAtlasCleared = true;
  VkImageMemoryBarrier2 to_attachment = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  to_attachment.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  to_attachment.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
  to_attachment.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  // UNDEFINED only on the first slice, which is what discards the previous level's atlas. Every
  // later slice must PRESERVE what the earlier ones wrote, so it declares the layout it is in -
  // getting this wrong would leave the finished atlas holding only the last slice's lights.
  to_attachment.oldLayout =
      first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  to_attachment.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  to_attachment.image = MapShadowImage;
  to_attachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  to_attachment.subresourceRange.levelCount = 1;
  to_attachment.subresourceRange.layerCount = 1;
  VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &to_attachment;
  vkCmdPipelineBarrier2(cmd, &dependency);

  VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = MapShadowView;
  attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  // **CLEAR on the first slice and LOAD after it.** A clear here is the whole atlas, so clearing
  // every slice would erase the lights baked before it - and a cleared tile reads as depth 1,
  // which is "nothing occludes". That is what makes an unbaked light simply unshadowed rather
  // than fully shadowed, and it is why a level does not start black while the bake catches up.
  attachment.loadOp = first ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.clearValue.depthStencil.depth = 1.0f;

  VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea.extent = {kMapShadowAtlas, kMapShadowAtlas};
  rendering.layerCount = 1;
  rendering.pDepthAttachment = &attachment;
  vkCmdBeginRendering(cmd, &rendering);
  const bool tessellating = ShadowTessellating() && MapShadowPipelineTess != VK_NULL_HANDLE;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    tessellating ? MapShadowPipelineTess : MapShadowPipeline);

  ShadowPushConstants push = {};
  push.draws = draw_records;
  FillShadowTessPush(push);
  push.vertices = arena_vertices;
  push.params = MapIndirectAddress + slice + kMapParamOffset;
  // Once for the whole slice, not once per draw: every caster is in the arena by construction
  // (IsMapGeometry), which is what makes an indirect batch possible at all.
  vkCmdBindIndexBuffer(cmd, index_buffer, 0, index_type);

  // One cube, six faces, into whichever tiles that slot owns. Shared by both producers because the
  // only thing that differs between a `STDLIGHT`'s cube and a D3D point light's is where the
  // centre is - and having one body is what stops the two drifting apart in the projection.
  const auto bake_cube = [&](uint32_t slot, const Vec3 &position, float range) {
    for (uint32_t face = 0; face < 6; ++face) {
      // The tile this (slot, face) owns. The same arithmetic is in world.slang's lookup, and it
      // is the one thing that has to agree between them beyond the face table.
      const uint32_t tile = slot * 6 + face;
      const float tx = static_cast<float>((tile % kMapShadowTilesPerRow) * kMapShadowFace);
      const float ty = static_cast<float>((tile / kMapShadowTilesPerRow) * kMapShadowFace);
      VkViewport viewport = {tx, ty, float(kMapShadowFace), float(kMapShadowFace), 0.0f, 1.0f};
      VkRect2D scissor = {{int32_t(tx), int32_t(ty)}, {kMapShadowFace, kMapShadowFace}};
      vkCmdSetViewport(cmd, 0, 1, &viewport);
      vkCmdSetScissor(cmd, 0, 1, &scissor);
      BuildCubeFaceMatrix(position, range, face, push.light_matrix);

      vkCmdPushConstants(cmd, ShadowLayout, kShadowPushStages, 0, sizeof(push), &push);
      if (MapShadowIndirect) {
        // **One command for every caster on this face.** The whole optimisation: what was 213
        // `vkCmdDrawIndexed` calls is one, and the record each vertex needs comes out of
        // `params` at `SV_DrawIndex` instead of out of the push (§4.62).
        vkCmdDrawIndexedIndirect(cmd, MapIndirectBuffer, slice,
                                 static_cast<uint32_t>(casters.size()), kMapIndirectStride);
        ++MapShadowDraws;
      } else {
        for (const DrawItem *item : casters) {
          push.record = item->record;
          push.base_vertex = item->base_vertex;
          vkCmdPushConstants(cmd, ShadowLayout, kShadowPushStages, 0, sizeof(push),
                             &push);
          vkCmdDrawIndexed(cmd, item->count, 1, item->first_index, item->vertex_offset, 0);
          ++MapShadowDraws;
        }
      }
    }
  };

  const uint32_t rate = MapShadowRateValue > 0 ? static_cast<uint32_t>(MapShadowRateValue) : 1;
  uint32_t end = MapShadowCursor;
  if (map_work) {
    end = (std::min)(MapShadowCursor + rate, static_cast<uint32_t>(MapShadowLightForSlot.size()));
    for (uint32_t slot = MapShadowCursor; slot < end; ++slot) {
      const MapLight &light = lights[MapShadowLightForSlot[slot]];
      bake_cube(slot, light.position, light.range);
    }
  }
  MapShadowCursor = end;

  // The local queue, drained whole: it is at most `kLocalShadowSlots` cubes and in practice one or
  // two at a time, where the map's is hundreds. Rate-limiting it would only delay a shadow the
  // player is looking at.
  if (local_work) {
    for (const uint32_t local : LocalShadowPending) {
      const LocalShadowKey *owner = LocalShadowOwner[local];
      if (owner == nullptr) {
        continue; // evicted between claiming the slot and baking it
      }
      const auto entry = LocalShadowKeys.find(*owner);
      if (entry == LocalShadowKeys.end() || entry->second.slot != static_cast<int32_t>(local)) {
        continue;
      }
      Vec3 position = {};
      float range = 0.0f;
      std::memcpy(&position.x, entry->second.key.position, sizeof(float) * 3);
      std::memcpy(&range, &entry->second.key.range, sizeof(range));
      bake_cube(kMapShadowLightSlots + local, position, range);
      entry->second.baked = true;
      ++LocalShadowBakes;
    }
    LocalShadowPending.clear();
  }
  MapShadowLastCasters = static_cast<uint32_t>(casters.size());
  vkCmdEndRendering(cmd);

  VkImageMemoryBarrier2 to_read = to_attachment;
  to_read.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  to_read.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  to_read.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  to_read.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  dependency.pImageMemoryBarriers = &to_read;
  vkCmdPipelineBarrier2(cmd, &dependency);
}

// The per-frame bake (§4.66). Every light this frame registered, six faces each, over every
// opaque caster in the frame - map, props and units alike.
void BakeDynamicShadows(void *command_buffer) {
  auto cmd = static_cast<VkCommandBuffer>(command_buffer);
  DynCasters = 0;
  DynCastersDropped = 0;
  DynBuckets = 0;
  DynFaceSubmits = 0;
  DynFaceCulledRange = 0;
  DynFaceCulledFrustum = 0;
  DynCommandsDropped = 0;
  DynUnbounded = 0;
  if (!Ready || !DynShadowReady || cmd == VK_NULL_HANDLE || !DynamicShadowsEnabled) {
    return;
  }
  // Nothing new to bake. Not merely an optimisation: without it a frame that registers no light -
  // a menu, a load - would re-bake the previous frame's set against the previous frame's records,
  // which the scratch has since rotated out from under.
  ++DynCalls;
  if (DynLights.empty() || DynFrame == DynBakedFrame) {
    ++DynSkipped;
    return;
  }
  const uint64_t arena_vertices = VertexArenaAddress();
  const uint64_t scratch_vertices = ScratchVertexAddress();
  const uint64_t draw_records = ScratchDrawAddress();
  if (draw_records == 0 || Items.empty()) {
    return;
  }

  std::vector<CasterBucket> buckets;
  std::vector<const DrawItem *> ordered;
  CollectCasters(IsDynamicCaster, kMaxMapCasters, ordered, buckets, DynCastersDropped);
  if (ordered.empty()) {
    return;
  }

  // **Range-check the casters before anything is submitted**, and keep the first few for the
  // report.
  //
  // A RenderDoc capture cannot see this bake: the device is lost before `EndFrameCapture` can
  // write the file, so the capture never exists. What a capture would have shown - a command with
  // an index range past its buffer, which is the classic way to hang a GPU on an indirect draw -
  // is computable here, at no risk, from the values about to be sent.
  //
  // Per caster rather than per emitted command, which it used to be. A command is a copy of its
  // caster's index range, so the set of distinct ranges is the same either way - and the cull
  // below emits a caster once per face it survives, which would otherwise count one bad range
  // several times and fill the sample with one draw.
  DynBadRanges = 0;
  DynWorstIndexEnd = 0;
  DynSample.clear();
  {
    const uint64_t index_capacity = Resources().index_arena_bytes;
    for (const CasterBucket &bucket : buckets) {
      for (uint32_t i = 0; i < bucket.count; ++i) {
        const DrawItem *item = ordered[bucket.first + i];
        const uint64_t end = static_cast<uint64_t>(item->first_index) + item->count;
        const uint64_t bytes = end * bucket.index_stride;
        if (bucket.index_source == DrawSource::Arena && bytes > index_capacity) {
          ++DynBadRanges;
        }
        if (bytes > DynWorstIndexEnd) {
          DynWorstIndexEnd = bytes;
        }
        if (DynSample.size() < 12) {
          DynSample.push_back({item->count, item->first_index,
                               static_cast<uint32_t>(item->vertex_offset), item->record,
                               item->base_vertex, bucket.index_stride,
                               bucket.index_source == DrawSource::Arena});
        }
      }
    }
  }

  // The bisect caps (§4.66), read here rather than at the draw loop because the batch below is
  // built per (light, face) and has to know how many of each there will be. Everything above
  // still describes the whole set, so `dynamic_shadow_report` keeps saying what the frame
  // *offered* while these say what was actually submitted. 0 is no cap.
  const uint32_t light_limit =
      DynMaxLights > 0 ? (std::min)(static_cast<uint32_t>(DynMaxLights),
                                    static_cast<uint32_t>(DynLights.size()))
                       : static_cast<uint32_t>(DynLights.size());
  const uint32_t face_limit = DynMaxFaces > 0 ? (std::min)(static_cast<uint32_t>(DynMaxFaces), 6u)
                                              : 6u;

  // --- the cull ---------------------------------------------------------------------------
  //
  // Nothing rejected a caster before this: the whole list went to all six faces of every light,
  // so the bake was `casters x lights x 6` however far apart any of them were. On the frame this
  // was written against that is 304 x 9 x 6 = 16,416 pieces of geometry against the world pass's
  // 367 - the level's own mesh redrawn into every 256-texel tile and thrown away by the scissor.
  //
  // Two tests, cheapest first, and they are not the same test at different strengths:
  //
  // - the light's **sphere**, which answers for all six faces at once and is where the bulk of a
  //   level goes. A caster it rejects cannot be lit by this light at all.
  // - the face's **frustum**, which is the 90-degree pyramid the tile actually rasterises. Its
  //   far plane is the light's range and its near plane is `range / 64`, so it subsumes the
  //   sphere - the sphere is kept because it runs once per light instead of six times.
  //
  // A caster with no world box (`has_bounds` false) is drawn on every face, unconditionally.
  // That is the only safe reading of "unknown", and `DynUnbounded` is what says how much of the
  // frame is in that state.
  //
  // The four vectors are function-static rather than local: at 8192 commands the batch is 224 KB
  // and this runs every frame, so keeping the capacity is worth more than the tidiness. Only the
  // render thread reaches here.
  static std::vector<VkDrawIndexedIndirectCommand> commands;
  static std::vector<uint32_t> params;
  static std::vector<const DrawItem *> survivors;
  static std::vector<uint8_t> in_range;
  // One run per (light, face, bucket), in that nesting order - a contiguous range of the arrays
  // above, which is what `vkCmdDrawIndexedIndirect` needs and what the direct path walks.
  struct FaceRun {
    uint32_t first = 0;
    uint32_t count = 0;
  };
  static std::vector<FaceRun> runs;
  commands.clear();
  params.clear();
  survivors.clear();
  runs.clear();
  runs.resize(static_cast<size_t>(light_limit) * face_limit * buckets.size());
  DynBucketReports.clear();
  for (const CasterBucket &bucket : buckets) {
    DynBucketReport report;
    report.vertex_arena = bucket.vertex_source == DrawSource::Arena;
    report.index_arena = bucket.index_source == DrawSource::Arena;
    report.stride = bucket.index_stride;
    report.count = bucket.count;
    for (uint32_t i = 0; i < bucket.count; ++i) {
      if (!ordered[bucket.first + i]->has_bounds) {
        ++report.unbounded;
        ++DynUnbounded;
      }
    }
    DynBucketReports.push_back(report);
  }
  in_range.assign(ordered.size(), 1);
  for (uint32_t slot = 0; slot < light_limit; ++slot) {
    Vec3 position = {};
    float range = 0.0f;
    std::memcpy(&position.x, DynLights[slot].position, sizeof(float) * 3);
    std::memcpy(&range, &DynLights[slot].range, sizeof(range));
    for (size_t i = 0; i < ordered.size(); ++i) {
      const DrawItem *item = ordered[i];
      in_range[i] = !(DynCullEnabled && item->has_bounds &&
                      BoxOutsideSphere(item->bounds_min, item->bounds_max, position, range));
    }
    for (uint32_t face = 0; face < face_limit; ++face) {
      float matrix[16];
      BuildCubeFaceMatrix(position, range, face, matrix);
      float planes[6][4];
      BuildFrustumPlanes(matrix, planes);
      for (size_t b = 0; b < buckets.size(); ++b) {
        const CasterBucket &bucket = buckets[b];
        FaceRun &run = runs[(static_cast<size_t>(slot) * face_limit + face) * buckets.size() + b];
        run.first = static_cast<uint32_t>(survivors.size());
        run.count = 0;
        // The caster cap, per bucket: with one bucket it is exactly "the first N casters", and
        // with more it takes the first N of each, which is what keeps every bucket represented
        // while the set shrinks. Applied to the bucket's casters, not to its survivors, so the
        // cap selects the same geometry whether the cull is on or off.
        const uint32_t draw_count =
            DynMaxCasters > 0 ? (std::min)(static_cast<uint32_t>(DynMaxCasters), bucket.count)
                              : bucket.count;
        for (uint32_t i = 0; i < draw_count; ++i) {
          const uint32_t at = bucket.first + i;
          const DrawItem *item = ordered[at];
          if (DynCullEnabled && item->has_bounds) {
            if (!in_range[at]) {
              ++DynFaceCulledRange;
              continue;
            }
            if (BoxOutsideFrustum(planes, item->bounds_min, item->bounds_max)) {
              ++DynFaceCulledFrustum;
              continue;
            }
          }
          if (survivors.size() >= kDynMaxCommands) {
            ++DynCommandsDropped;
            continue;
          }
          VkDrawIndexedIndirectCommand command = {};
          command.indexCount = item->count;
          command.instanceCount = 1;
          command.firstIndex = item->first_index;
          command.vertexOffset = item->vertex_offset;
          command.firstInstance = 0;
          commands.push_back(command);
          params.push_back(item->record);
          params.push_back(item->base_vertex);
          survivors.push_back(item);
          ++run.count;
          ++DynFaceSubmits;
        }
      }
    }
  }
  // Every light's every face lost its whole caster list, which is a legal frame - a light with
  // nothing near it - and there is then no batch to upload and nothing to draw. The atlas still
  // has to be cleared, or its slots keep the previous frame's cubes, so the pass below runs
  // anyway; only the transfer is skipped.
  const bool have_batch = !commands.empty();

  // This frame's slice of the ring, so the transfer cannot land on bytes an in-flight frame's
  // indirect draws are still reading.
  ++DynRingSerial;
  const VkDeviceSize slice =
      static_cast<VkDeviceSize>(DynRingSerial % kDynIndirectRing) * kDynIndirectSlice;
  // ... and the atlas image's own slice, which is the hazard the batch's ring does not cover:
  // frame N's world pass is still SAMPLING the atlas when frame N+1's bake declares
  // `oldLayout = UNDEFINED` on it.
  DynImageSlot = static_cast<uint32_t>(DynRingSerial % kDynShadowRing);

  if (have_batch) {
    // Chunked, because the batch is allowed past the 64 KB one `vkCmdUpdateBuffer` takes.
    UpdateBufferChunked(cmd, DynIndirectBuffer, slice,
                        commands.data(),
                        commands.size() * sizeof(VkDrawIndexedIndirectCommand));
    UpdateBufferChunked(cmd, DynIndirectBuffer, slice + kDynParamOffset, params.data(),
                        params.size() * sizeof(uint32_t));
    // Two destinations, as §4.62: the commands are read by DRAW_INDIRECT and the parameters by
    // the vertex shader as an address, and only the first is what a transfer barrier assumes.
    VkMemoryBarrier2 written = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    written.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    written.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    written.dstStageMask =
        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    written.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo upload = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    upload.memoryBarrierCount = 1;
    upload.pMemoryBarriers = &written;
    vkCmdPipelineBarrier2(cmd, &upload);
  }

  VkImageMemoryBarrier2 to_attachment = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  to_attachment.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  to_attachment.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
  to_attachment.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  // **UNDEFINED every frame**, where the static atlas may only say it on its first slice: this one
  // is rebuilt whole, so the previous frame's contents are not ours to preserve and saying so lets
  // the driver skip a decompress. It is the same reasoning as the offscreen target's (§4.38).
  to_attachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_attachment.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  to_attachment.image = DynShadowImage[DynImageSlot];
  to_attachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  to_attachment.subresourceRange.levelCount = 1;
  to_attachment.subresourceRange.layerCount = 1;
  VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &to_attachment;
  vkCmdPipelineBarrier2(cmd, &dependency);

  VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = DynShadowView[DynImageSlot];
  attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  // CLEAR unconditionally, for the same reason the barrier says UNDEFINED - and it is what makes a
  // slot no light claimed this frame read as depth 1, "nothing occludes", rather than as whatever
  // light held it last frame.
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.clearValue.depthStencil.depth = 1.0f;

  VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea.extent = {kDynShadowAtlas, kDynShadowAtlas};
  rendering.layerCount = 1;
  rendering.pDepthAttachment = &attachment;
  vkCmdBeginRendering(cmd, &rendering);
  // Two axes, so four pipelines: which submission path, and whether the patch is amplified. The
  // tessellated twin is only taken when it actually built - a create failure there leaves the
  // untessellated one bound rather than dropping the bake.
  const bool tessellating =
      ShadowTessellating() && (DynShadowIndirect ? DynShadowPipelineTess != VK_NULL_HANDLE
                                                 : DynShadowDirectPipelineTess != VK_NULL_HANDLE);
  VkPipeline dyn_pipeline =
      tessellating ? (DynShadowIndirect ? DynShadowPipelineTess : DynShadowDirectPipelineTess)
                   : (DynShadowIndirect ? DynShadowPipeline : DynShadowDirectPipeline);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dyn_pipeline);

  ShadowPushConstants push = {};
  push.draws = draw_records;
  FillShadowTessPush(push);
  VkBuffer bound_index = VK_NULL_HANDLE;
  VkIndexType bound_type = VK_INDEX_TYPE_UINT16;

  // One run per (light, face, bucket), already culled, in the nesting order this loop walks. A
  // face whose run is empty costs a viewport and a matrix and nothing else - and a light with no
  // caster near it costs six of those, against the whole list it used to redraw.
  for (uint32_t slot = 0; slot < light_limit; ++slot) {
    Vec3 position = {};
    float range = 0.0f;
    std::memcpy(&position.x, DynLights[slot].position, sizeof(float) * 3);
    std::memcpy(&range, &DynLights[slot].range, sizeof(range));
    for (uint32_t face = 0; face < face_limit; ++face) {
      const uint32_t tile = slot * 6 + face;
      const float tx = static_cast<float>((tile % kDynShadowTilesPerRow) * kDynShadowFace);
      const float ty = static_cast<float>((tile / kDynShadowTilesPerRow) * kDynShadowFace);
      VkViewport viewport = {tx, ty, float(kDynShadowFace), float(kDynShadowFace), 0.0f, 1.0f};
      VkRect2D scissor = {{int32_t(tx), int32_t(ty)}, {kDynShadowFace, kDynShadowFace}};
      vkCmdSetViewport(cmd, 0, 1, &viewport);
      vkCmdSetScissor(cmd, 0, 1, &scissor);
      BuildCubeFaceMatrix(position, range, face, push.light_matrix);

      for (size_t b = 0; b < buckets.size(); ++b) {
        const CasterBucket &bucket = buckets[b];
        const FaceRun &run =
            runs[(static_cast<size_t>(slot) * face_limit + face) * buckets.size() + b];
        if (run.count == 0) {
          continue;
        }
        const VkBuffer index_buffer = bucket.index_source == DrawSource::Arena
                                          ? reinterpret_cast<VkBuffer>(IndexArenaBuffer())
                                          : reinterpret_cast<VkBuffer>(ScratchIndexBuffer());
        const VkIndexType type =
            bucket.index_stride == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        if (index_buffer != bound_index || type != bound_type) {
          vkCmdBindIndexBuffer(cmd, index_buffer, 0, type);
          bound_index = index_buffer;
          bound_type = type;
        }
        push.vertices = bucket.vertex_source == DrawSource::Arena ? arena_vertices
                                                                 : scratch_vertices;
        if (!DynShadowIndirect) {
          // The direct path: the record and the arena slot ride in the push, exactly as the sun's
          // pass does it, and nothing reads the indirect buffer at all. It walks `survivors`,
          // which is index-parallel to `commands` - so the two paths draw the same set by
          // construction rather than by two copies of the cull agreeing.
          for (uint32_t i = 0; i < run.count; ++i) {
            const DrawItem *item = survivors[run.first + i];
            push.record = item->record;
            push.base_vertex = item->base_vertex;
            vkCmdPushConstants(cmd, ShadowLayout, kShadowPushStages, 0, sizeof(push),
                               &push);
            vkCmdDrawIndexed(cmd, item->count, 1, item->first_index, item->vertex_offset, 0);
            ++DynIndirectCommands;
          }
          continue;
        }
        // Each run's parameters start where its commands do, so `SV_DrawIndex` - which counts
        // from 0 within one `vkCmdDrawIndexedIndirect` - indexes its own run's slice.
        push.params = DynIndirectAddress + slice + kDynParamOffset + run.first * 8;
        vkCmdPushConstants(cmd, ShadowLayout, kShadowPushStages, 0, sizeof(push), &push);
        vkCmdDrawIndexedIndirect(cmd, DynIndirectBuffer,
                                 slice + static_cast<VkDeviceSize>(run.first) *
                                             kMapIndirectStride,
                                 run.count, kMapIndirectStride);
        ++DynIndirectCommands;
      }
    }
  }
  vkCmdEndRendering(cmd);

  VkImageMemoryBarrier2 to_read = to_attachment;
  to_read.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  to_read.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  to_read.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  to_read.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  dependency.pImageMemoryBarriers = &to_read;
  vkCmdPipelineBarrier2(cmd, &dependency);

  DynBakedFrame = DynFrame;
  DynCasters = static_cast<uint32_t>(ordered.size());
  DynBuckets = static_cast<uint32_t>(buckets.size());
}

void BuildLightGrid(void *command_buffer) {
  auto cmd = static_cast<VkCommandBuffer>(command_buffer);
  if (!Ready || GridPipeline == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE) {
    return;
  }
  // UploadMapLights runs at the top of RecordDraws, which is AFTER this - so the addresses it
  // publishes belong to the previous frame at this point. That is exactly what is wanted: the
  // scratch holds the same lights every frame, and rebuilding against last frame's copy of an
  // unchanged set is the same work as rebuilding against this frame's.
  const bool want = MapLightingEnabled && MapLightCullEnabled && FrameMapLightCount > 0 &&
                    FrameMapLightAddress != 0;
  if (!want) {
    GridValid = false;
    GridBuiltForAddress = 0;
    return;
  }
  if (GridValid && GridBuiltForCount == FrameMapLightCount &&
      GridBuiltForCullOn == MapLightCullEnabled) {
    return; // already built for this level
  }

  const gk::Map *map = gk::GetCurrentMap();
  if (map == nullptr) {
    return;
  }
  // The grid covers the map's own bounds. A light outside them still reaches cells inside, which
  // the sphere test handles - what the bounds decide is only where cells exist, and a fragment
  // outside them clamps into the edge cell rather than going unlit.
  const float lo[3] = {map->bounds_min.x, map->bounds_min.y, map->bounds_min.z};
  const float hi[3] = {map->bounds_max.x, map->bounds_max.y, map->bounds_max.z};
  const uint32_t dims[3] = {kGridX, kGridY, kGridZ};
  for (int i = 0; i < 3; ++i) {
    GridMin[i] = lo[i];
    // A degenerate axis would divide by zero and put every light in one cell; 1e-3 keeps the
    // arithmetic finite and the result merely useless rather than NaN.
    // Written out rather than with std::max: windows.h is included here without NOMINMAX, so
    // `max` is a macro and the error lands on this line with nothing pointing at the cause.
    const float span = (hi[i] - lo[i]) / static_cast<float>(dims[i]);
    GridCell[i] = span > 1e-3f ? span : 1e-3f;
  }

  auto grid_buffer = reinterpret_cast<VkBuffer>(LightGridBuffer());
  auto index_buffer = reinterpret_cast<VkBuffer>(LightIndexBuffer());
  if (grid_buffer == VK_NULL_HANDLE || index_buffer == VK_NULL_HANDLE) {
    return;
  }

  // The three bindings, rewritten each build. The light buffer is the scratch slice, so its
  // offset moves with the frame - which is why this is a write rather than a one-time setup.
  VkDescriptorBufferInfo buffers[3] = {};
  buffers[0].buffer = reinterpret_cast<VkBuffer>(ScratchMapLightVkBuffer());
  buffers[0].offset = FrameMapLightByteOffset;
  buffers[0].range = static_cast<VkDeviceSize>(FrameMapLightCount) * sizeof(GpuMapLight);
  buffers[1].buffer = grid_buffer;
  buffers[1].range = VK_WHOLE_SIZE;
  buffers[2].buffer = index_buffer;
  buffers[2].range = VK_WHOLE_SIZE;
  VkWriteDescriptorSet writes[3] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = GridSet;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &buffers[i];
  }
  vkUpdateDescriptorSets(GetDevice(), 3, writes, 0, nullptr);

  // The header: the allocator zeroed, and the three vectors the fragment shader reads back out.
  // vkCmdUpdateBuffer rather than a staged copy - it takes up to 64 KB inline and this is 48
  // bytes, so the values ride in the command buffer itself.
  uint32_t header[kGridHeaderWords] = {};
  header[0] = 0;
  for (int i = 0; i < 3; ++i) {
    header[1 + i] = dims[i];
    std::memcpy(&header[4 + i], &GridMin[i], sizeof(float));
    std::memcpy(&header[7 + i], &GridCell[i], sizeof(float));
  }
  vkCmdUpdateBuffer(cmd, grid_buffer, 0, sizeof(header), header);
  VkMemoryBarrier2 fill_done = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  fill_done.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  fill_done.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  fill_done.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  fill_done.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &fill_done;
  vkCmdPipelineBarrier2(cmd, &dependency);

  LightGridPush push = {};
  for (int i = 0; i < 3; ++i) {
    push.grid_min[i] = GridMin[i];
    push.cell_size[i] = GridCell[i];
    push.dims[i] = dims[i];
  }
  push.dims[3] = FrameMapLightCount;
  push.index_capacity = kGridIndexCapacity;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, GridPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, GridLayout, 0, 1, &GridSet, 0,
                          nullptr);
  vkCmdPushConstants(cmd, GridLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
  vkCmdDispatch(cmd, (kGridCells + 63) / 64, 1, 1);

  // The fragment shader reads both buffers by device address, so the barrier is a memory one
  // rather than a buffer one - there is no handle on that side to name.
  VkMemoryBarrier2 build_done = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  build_done.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  build_done.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  build_done.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  build_done.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
  dependency.pMemoryBarriers = &build_done;
  vkCmdPipelineBarrier2(cmd, &dependency);

  GridValid = true;
  GridBuiltForAddress = FrameMapLightAddress;
  GridBuiltForCount = FrameMapLightCount;
  GridBuiltForCullOn = MapLightCullEnabled;
  ++TheStats.light_grid_builds;
}

void SetMapLighting(bool enabled) { MapLightingEnabled = enabled; }

bool MapLighting() { return MapLightingEnabled; }

void SetStencilShadow(bool enabled) { StencilShadowEnabled = enabled; }
bool StencilShadow() { return StencilShadowEnabled; }

void SetSunShadows(bool enabled) { SunShadowsEnabled = enabled; }
bool SunShadows() { return SunShadowsEnabled; }
void SetShadowBias(float bias) { ShadowBiasValue = bias; }
float ShadowBias() { return ShadowBiasValue; }
void SetShadowStrength(float strength) { ShadowStrengthValue = strength; }
float ShadowStrength() { return ShadowStrengthValue; }
void SetShadowExtent(float extent) { ShadowExtentValue = extent; }
float ShadowExtent() { return ShadowExtentValue; }
void SetAmbientOcclusion(bool enabled) { AoEnabled = enabled; }
bool AmbientOcclusion() { return AoEnabled && AoReady; }
void SetAoRadius(float radius) { AoRadiusValue = radius < 0.0f ? 0.0f : radius; }
float AoRadius() { return AoRadiusValue; }
void SetAoScreenRadius(float fraction) {
  AoScreenRadiusValue = fraction < 0.0f ? 0.0f : fraction;
}
float AoScreenRadius() { return AoScreenRadiusValue; }
void SetAoBias(float bias) { AoBiasValue = bias; }
float AoBias() { return AoBiasValue; }
void SetAoStrength(float strength) { AoStrengthValue = strength; }
float AoStrength() { return AoStrengthValue; }
void SetAoDirect(float direct) { AoDirectValue = direct; }
float AoDirect() { return AoDirectValue; }
// Clamped here rather than in the shader: the disc is a fixed-length array and a `taps` past its
// end would read off it. The shader clamps too, because a push constant is not a promise.
void SetAoTaps(int taps) {
  const int clamped = taps < 1 ? 1 : (taps > 64 ? 64 : taps);
  AoTapsValue = static_cast<uint32_t>(clamped);
}
int AoTaps() { return static_cast<int>(AoTapsValue); }
void SetAoMapOnly(bool enabled) { AoMapOnlyEnabled = enabled; }
bool AoMapOnly() { return AoMapOnlyEnabled; }
void SetAoDebug(bool enabled) { AoDebugEnabled = enabled; }
bool AoDebug() { return AoDebugEnabled; }

void SetMapShadows(bool enabled) { MapShadowsEnabled = enabled; }
bool MapShadows() { return MapShadowsEnabled; }
void SetMapShadowBias(float texels) { MapShadowBiasValue = texels; }
float MapShadowBias() { return MapShadowBiasValue; }
void SetMapShadowIndirect(bool enabled) {
  const bool want = enabled && Caps().multi_draw_indirect;
  if (!MapShadowReady || want == MapShadowIndirect) {
    return; // nothing to rebuild, or the device has no multiDrawIndirect to turn on
  }
  MapShadowIndirect = want;
  // The pipeline is in flight for as long as a frame is; waiting is the honest way to swap it,
  // and this is a diagnostic that runs once, not a per-frame path.
  {
    QueueGuard queue_guard(QueueMutex());
    vkDeviceWaitIdle(GetDevice());
  }
  vkDestroyPipeline(GetDevice(), MapShadowPipeline, nullptr);
  MapShadowPipeline = VK_NULL_HANDLE;
  MapShadowReady = CreateMapShadowPipeline();
  MapShadowBuiltForGeneration = 0; // and re-bake, or the A/B compares one path against itself
}
bool MapShadowIndirectEnabled() { return MapShadowIndirect; }

void SetMapShadowRate(int lights) {
  MapShadowRateValue = lights < 1 ? 1 : lights;
  // A rate change re-bakes from the start, because the interesting reason to change it is to see
  // whether a longer submit is affordable - and that question is about the first slice, which has
  // already gone by the time anyone asks.
  MapShadowBuiltForGeneration = 0;
}
int MapShadowRate() { return MapShadowRateValue; }

std::string MapShadowReport() {
  std::string out;
  char line[256];
  const auto add = [&out, &line](auto... args) {
    std::snprintf(line, sizeof(line), args...);
    out += line;
  };
  if (!MapShadowReady) {
    return "map shadow atlas: NOT CREATED - this device has no D16_UNORM sampled depth, or the "
           "pipeline failed\n";
  }
  const size_t lights = MapLights().size();
  add("map shadow atlas: %ux%u (%u KB), %u faces of %u, %u light slots\n", kMapShadowAtlas,
      kMapShadowAtlas, (unsigned)(MapShadowBytes / 1024),
      kMapShadowTilesPerRow * kMapShadowTilesPerRow, kMapShadowFace, kMapShadowSlots);
  add("  %u of this level's %u lights have a slot (%u refused - the atlas is full)\n",
      (unsigned)MapShadowLightForSlot.size(), (unsigned)lights, MapShadowRefused);
  add("  baked %u of %u, %d lights a frame, %llu %s so far%s\n", MapShadowCursor,
      (unsigned)MapShadowLightForSlot.size(), MapShadowRateValue,
      (unsigned long long)MapShadowDraws,
      MapShadowIndirect ? "indirect commands" : "draw calls",
      MapShadowCursor >= MapShadowLightForSlot.size() ? " (finished)" : " (in progress)");
  // Printed even when it reads zero, unlike most counters here: "no caster was dropped" is what
  // says the batch is the whole map rather than most of it, and the two are indistinguishable
  // from the picture.
  add("  submission: %s (%u casters, %u dropped - must be 0)\n",
      MapShadowIndirect ? "vkCmdDrawIndexedIndirect, one command a face"
                        : "a draw call per caster per face - no multiDrawIndirect",
      MapShadowLastCasters, MapShadowCastersDropped);
  add("  normal offset %.2f texels, sampled: %s\n", MapShadowBiasValue,
      MapShadowsEnabled ? "yes" : "NO - render.map_shadows is off");
  return out;
}

void SetShadowCascades(int count) {
  ShadowCascadeCount = count < 1 ? 1
                                 : (count > static_cast<int>(kMaxShadowCascades)
                                        ? static_cast<int>(kMaxShadowCascades)
                                        : count);
}
int ShadowCascades() { return ShadowCascadeCount; }

void SetLocalLights(bool enabled) { LocalLightsEnabled = enabled; }
bool LocalLights() { return LocalLightsEnabled; }

void SetLocalLightWindow(bool enabled) { LocalLightWindowEnabled = enabled; }
bool LocalLightWindow() { return LocalLightWindowEnabled; }

void SetDynamicShadows(bool enabled) { DynamicShadowsEnabled = enabled; }
void SetDynamicShadowArenaOnly(bool enabled) { DynCasterArenaOnly = enabled; }
void SetDynamicShadowMapOnly(bool enabled) { DynCasterMapOnly = enabled; }
bool DynamicShadowMapOnly() { return DynCasterMapOnly; }
void SetDynamicShadowSample(bool enabled) { DynShadowSample = enabled; }
bool DynamicShadowSample() { return DynShadowSample; }
bool DynamicShadowArenaOnly() { return DynCasterArenaOnly; }
bool DynamicShadows() { return DynamicShadowsEnabled; }
void SetDynamicShadowBias(float texels) { DynShadowBiasValue = texels; }
float DynamicShadowBias() { return DynShadowBiasValue; }

// Clamped at 3 (a 7x7) because the kernel is quadratic and the loop is per light per fragment -
// 4 would be 81 taps against 9, and the artefact this exists for is gone by 1.
void SetLocalShadowTaps(int radius) {
  LocalShadowTapsValue = radius < 0 ? 0 : (radius > 3 ? 3 : radius);
}
int LocalShadowTaps() { return LocalShadowTapsValue; }

// The bisect caps. Clamped at 0 rather than rejected, because 0 is the meaningful "no cap" value
// and a negative would otherwise read as an enormous unsigned one at the comparison.
void SetDynamicShadowMaxLights(int lights) { DynMaxLights = lights < 0 ? 0 : lights; }
int DynamicShadowMaxLights() { return DynMaxLights; }
void SetDynamicShadowMaxFaces(int faces) { DynMaxFaces = faces < 0 ? 0 : faces; }
int DynamicShadowMaxFaces() { return DynMaxFaces; }
void SetDynamicShadowMaxCasters(int casters) { DynMaxCasters = casters < 0 ? 0 : casters; }
int DynamicShadowMaxCasters() { return DynMaxCasters; }
void SetDynamicShadowIndirect(bool enabled) { DynShadowIndirect = enabled; }
bool DynamicShadowIndirect() { return DynShadowIndirect; }

void SetDynamicShadowCull(bool enabled) { DynCullEnabled = enabled; }
bool DynamicShadowCull() { return DynCullEnabled; }

void SetSunShadowCull(bool enabled) { SunCullEnabled = enabled; }
bool SunShadowCull() { return SunCullEnabled; }
// Setting it on a device that cannot take it does nothing and reads back false, exactly as
// `map_shadow_indirect` does - the pipeline is what decides, not the knob.
void SetSunShadowIndirect(bool enabled) {
  SunIndirectEnabled = enabled && ShadowPipelineIndirect != VK_NULL_HANDLE;
}
bool SunShadowIndirect() { return SunIndirectEnabled; }

std::string SunShadowReport() {
  std::string out;
  char line[256];
  const auto add = [&](const char *format, auto... args) {
    std::snprintf(line, sizeof(line), format, args...);
    out += line;
  };
  add("sun shadow pass: %s\n",
      !SunShadowsEnabled ? "OFF - render.sun_shadows"
                         : (!ShadowReady ? "NO PIPELINE" : (FrameSunValid ? "on" : "no sun")));
  add("atlas %ux%u, %u tiles of %u: %u cascades live, extent %.0f\n", kShadowAtlas, kShadowAtlas,
      kMaxShadowCascades, kShadowTile, FrameCascadeCount, ShadowExtentValue);
  add("last pass: %u casters in %u buckets (%u dropped)\n", SunCasterCount, SunBucketCount,
      SunCastersDropped);
  // In (caster, cascade) pairs, because that is the unit of work: the same caster is separate
  // geometry in every cascade it reaches. `offered` is what the pass would have drawn with the
  // cull off, which is exactly what `render.sun_shadow_cull = false` produces.
  {
    const uint32_t offered = SunCascadeSubmits + SunCascadeCulled + SunCommandsDropped;
    add("cull: %s - %u of %u caster-cascades drawn (%.1f%%), %u outside the cascade box\n",
        SunCullEnabled ? "on" : "OFF - render.sun_shadow_cull", SunCascadeSubmits, offered,
        offered == 0 ? 0.0 : 100.0 * SunCascadeSubmits / offered, SunCascadeCulled);
    add("  no bounds (drawn in every cascade): %u of %u casters   batch cap %u, %u dropped\n",
        SunUnbounded, SunCasterCount, kShadowMaxCommands, SunCommandsDropped);
  }
  // The halving, measured. Cascade 0 is the sharp near box and should keep almost nothing on a
  // level of any size; cascade `live - 1` is `shadow_extent` and should keep nearly everything.
  float extent = ShadowExtentValue;
  for (uint32_t i = 1; i < FrameCascadeCount; ++i) {
    extent *= 0.5f;
  }
  for (uint32_t i = 0; i < FrameCascadeCount && i < kMaxShadowCascades; ++i) {
    add("  cascade %u: half-extent %7.2f   %u casters drawn\n", i, extent, SunCascadeDrawn[i]);
    extent *= 2.0f;
  }
  add("submission: %s - %u calls this pass, against %u casters x %u cascades unculled\n",
      SunIndirectEnabled
          ? "vkCmdDrawIndexedIndirect, one run a bucket a cascade"
          : (ShadowPipelineIndirect == VK_NULL_HANDLE
                 ? "a draw call per caster per cascade (no indirect pipeline on this device)"
                 : "a draw call per caster per cascade (sun_shadow_indirect off)"),
      SunDrawCalls, SunCasterCount, FrameCascadeCount);
  return out;
}

std::string DynamicShadowReport() {
  std::string out;
  char line[256];
  const auto add = [&](const char *format, auto... args) {
    std::snprintf(line, sizeof(line), format, args...);
    out += line;
  };
  // **Three states and not two.** The first draft printed "UNAVAILABLE - no multiDrawIndirect"
  // for every way of not being ready, and the atlas creation was then accidentally never called
  // at all - which read as a device limitation on a card that plainly has the feature, and cost a
  // diagnosis. `multiDrawIndirect` is now asked separately from whether the atlas exists.
  add("per-frame shadow atlas: %s\n",
      !DynamicShadowsEnabled
          ? "OFF - render.dynamic_shadows"
          : (DynShadowReady ? "on"
                            : (Caps().multi_draw_indirect
                                   ? "NOT CREATED - see the log; the device supports it"
                                   : "UNAVAILABLE - this device has no multiDrawIndirect")));
  add("atlas %ux%u (%u KB, D16) x%u ring, %u faces of %u: %u light slots\n", kDynShadowAtlas,
      kDynShadowAtlas, (unsigned)(DynShadowBytes / 1024), kDynShadowRing,
      kDynShadowTilesPerRow * kDynShadowTilesPerRow, kDynShadowFace, kDynShadowSlots);
  add("last frame: %u lights (%u refused - no slot), %u casters in %u buckets (%u dropped)\n",
      (unsigned)DynLights.size(), DynRefused, DynCasters, DynBuckets, DynCastersDropped);
  // The count that says what this costs to submit: at most 6 x lights x buckets a frame, where
  // the static atlas was 6 x lights once per level - and fewer than that once the cull starts
  // emptying faces, since an empty run is not submitted at all.
  add("indirect commands issued: %llu cumulative, up to %u this frame\n",
      (unsigned long long)DynIndirectCommands,
      (unsigned)(DynLights.size() * 6 * (DynBuckets == 0 ? 0 : DynBuckets)));
  // **The reading that prices the pass**, and it is in (caster, face) pairs rather than casters,
  // because that is the unit of work: the same caster is separate geometry on every face it
  // reaches. `offered` is what the bake would have drawn with the cull off, which is exactly what
  // `render.dynamic_shadow_cull = false` produces - so the two numbers are an A/B that needs no
  // second run.
  {
    const uint32_t offered = DynFaceSubmits + DynFaceCulledRange + DynFaceCulledFrustum +
                             DynCommandsDropped;
    add("cull: %s - %u of %u caster-faces drawn (%.1f%%), %u out of range, %u outside the face\n",
        DynCullEnabled ? "on" : "OFF - render.dynamic_shadow_cull", DynFaceSubmits, offered,
        offered == 0 ? 0.0 : 100.0 * DynFaceSubmits / offered, DynFaceCulledRange,
        DynFaceCulledFrustum);
    // The part of the frame the cull cannot see. A caster with no world box is drawn into every
    // face of every light, so this number is a ceiling on what is left to win - and it moving is
    // how a regression in DrawItem::has_bounds surfaces as something other than a frame time.
    add("  no bounds (drawn on every face): %u of %u casters   batch cap %u, %u dropped\n",
        DynUnbounded, DynCasters, kDynMaxCommands, DynCommandsDropped);
    for (size_t b = 0; b < DynBucketReports.size(); ++b) {
      const DynBucketReport &report = DynBucketReports[b];
      add("    bucket %u: %s vertices, %s indices x%u - %u casters, %u with no bounds\n",
          (unsigned)b, report.vertex_arena ? "arena  " : "scratch",
          report.index_arena ? "arena  " : "scratch", report.stride, report.count,
          report.unbounded);
    }
  }
  add("normal offset %.2f texels\n", DynShadowBiasValue);
  // The bisect state, printed unconditionally: a capped bake that survives looks exactly like a
  // healthy one, so "which configuration was that?" has to be readable off the report itself.
  add("submission: %s, caps lights %d faces %d casters %d (0 = no cap)\n",
      DynShadowIndirect ? "vkCmdDrawIndexedIndirect, one batch a bucket a face"
                        : "a draw call per caster per face (dynamic_shadow_indirect off)",
      DynMaxLights, DynMaxFaces, DynMaxCasters);
  // **The check a RenderDoc capture of this bake cannot supply**, because the device is lost
  // before the file is written and so the capture never exists. An indirect command whose index
  // range runs past its buffer is the classic way to hang a GPU on an indirect draw, and it is
  // computable from the bytes about to be submitted - at no risk at all.
  add("bake calls: %llu   skipped as nothing new: %llu   frame %llu, baked %llu\n",
      (unsigned long long)DynCalls, (unsigned long long)DynSkipped,
      (unsigned long long)DynFrame, (unsigned long long)DynBakedFrame);
  add("index ranges past the arena: %u   furthest byte any command reads: %llu of %llu\n",
      DynBadRanges, (unsigned long long)DynWorstIndexEnd,
      (unsigned long long)Resources().index_arena_bytes);
  if (!DynSample.empty()) {
    out += "  the batch, first entries:  idxCount  firstIdx  vtxOffset  record  baseVtx  stride"
           "  source\n";
    for (const DynSampleEntry &e : DynSample) {
      add("    %8u  %8u  %9u  %6u  %7u  %6u  %s\n", e.index_count, e.first_index, e.vertex_offset,
          e.record, e.base_vertex, e.stride, e.arena ? "arena" : "scratch");
    }
  }
  for (uint32_t slot = 0; slot < DynLights.size(); ++slot) {
    float position[3] = {};
    float range = 0.0f;
    std::memcpy(position, DynLights[slot].position, sizeof(position));
    std::memcpy(&range, &DynLights[slot].range, sizeof(range));
    add("  slot %2u  %s  %.2f %.2f %.2f  range %.2f\n", slot,
        DynLights[slot].type == 2 ? "spot " : "point", position[0], position[1], position[2],
        range);
  }
  return out;
}

void SetLocalShadows(bool enabled) {
  if (LocalShadowsEnabled() == enabled) {
    return;
  }
  LocalShadowsWanted = enabled;
  LocalShadowsRead = true;
  // Forget every key on the way off, so turning it back on re-earns and re-bakes each slot rather
  // than sampling tiles whose lights may have moved while nobody was watching them.
  ResetLocalShadows();
}

bool LocalShadows() { return LocalShadowsEnabled(); }

std::string LocalShadowReport() {
  std::string out;
  char line[256];
  const auto add = [&](const char *format, auto... args) {
    std::snprintf(line, sizeof(line), format, args...);
    out += line;
  };
  // **Every count here is of LIGHTS, derived from the table now**, not accumulated per call:
  // this function is asked once and the acquire path runs once per light per frame, so a
  // cumulative refusal counts frames rather than lights. Only `forgotten` is cumulative, and it
  // is the one thing that genuinely is an event rather than a state.
  uint32_t held = 0;
  uint32_t baked = 0;
  uint32_t waiting = 0;
  uint32_t unslotted = 0;
  for (const auto &[key, entry] : LocalShadowKeys) {
    held += entry.slot >= 0 ? 1u : 0u;
    baked += entry.slot >= 0 && entry.baked ? 1u : 0u;
    waiting += entry.slot < 0 && entry.stable < kLocalShadowStableFrames ? 1u : 0u;
    unslotted += entry.slot < 0 && entry.stable >= kLocalShadowStableFrames ? 1u : 0u;
  }
  add("local light shadows: %s\n",
      LocalShadowsEnabled() ? "on" : "OFF - render.local_shadows / GKPLUS_VK_LOCAL_SHADOWS");
  add("atlas: %u slots reserved of %u, at tiles %u..%u\n", kLocalShadowSlots, kMapShadowSlots,
      kMapShadowLightSlots, kMapShadowSlots - 1);
  add("keys live: %u   holding a slot: %u   baked and sampled: %u\n",
      (unsigned)LocalShadowKeys.size(), held, baked);
  // These two are the pair to read, and they mean opposite things. **`waiting` is the feature
  // working**: a light that moves is here every frame under a new key and never leaves, which is
  // exactly why it costs nothing. `unslotted` is the only real limit - lights that held still and
  // found the sixteen slots taken.
  add("waiting out the %u-frame stability gate: %u   (a light that MOVES lives here permanently)\n",
      kLocalShadowStableFrames, waiting);
  add("held still but found no free slot: %u\n", unslotted);
  add("keys forgotten for going stale: %u   cubes baked for this level: %llu\n",
      LocalShadowForgotten, (unsigned long long)LocalShadowBakes);
  for (const auto &[key, entry] : LocalShadowKeys) {
    if (entry.slot < 0) {
      continue;
    }
    float position[3] = {};
    float range = 0.0f;
    std::memcpy(position, key.position, sizeof(position));
    std::memcpy(&range, &key.range, sizeof(range));
    add("  slot %2u  %s  %.2f %.2f %.2f  range %.2f  seen %u frames running\n",
        kMapShadowLightSlots + entry.slot, entry.baked ? "baked  " : "PENDING", position[0],
        position[1], position[2], range, entry.stable);
  }
  return out;
}

void SetMapLightCull(bool enabled) {
  MapLightCullEnabled = enabled;
  GridValid = false; // force a rebuild rather than leaving a stale grid behind the toggle
}

bool MapLightCull() { return MapLightCullEnabled; }

void SetMapLightingAll(bool enabled) { MapLightingAllEnabled = enabled; }

bool MapLightingAll() { return MapLightingAllEnabled; }

void SetMapLightGain(float gain) { MapLightGainValue = gain; }

float MapLightGain() { return MapLightGainValue; }

void SetShadeMode(bool enabled) { ShadeModeEnabled = enabled; }

bool ShadeMode() { return ShadeModeEnabled; }

// --- multisample antialiasing ------------------------------------------------------------------
//
// See SetMsaa in VkDraw.h. Everything here is bookkeeping: the work happens in
// `ReconcileRenderTarget`, which compares `MsaaTargetSamples()` against the target it built last
// and rebuilds under the wait-idle it already takes.

void SetMsaa(uint32_t samples) {
  // The env var is consumed here as well as at bring-up, so that the first *write* cannot be
  // overwritten by a lazy read that had not happened yet. Reading it after assigning would put
  // the environment's value back over the caller's.
  ReadMsaaEnvOnce();
  // Stored as asked, not as clamped. `MsaaTargetSamples` does the clamping on the way out, which
  // keeps a request of 8 on a 4x device asking for 8 - so it takes effect by itself if the device
  // is ever replaced by one that can, rather than being quietly rewritten to 4 forever.
  MsaaRequested = samples == 0 ? 1 : samples;
}

uint32_t Msaa() { return static_cast<uint32_t>(SampleCount); }

uint32_t MsaaWanted() {
  ReadMsaaEnvOnce();
  return MsaaRequested;
}

uint32_t MsaaTargetSamples() {
  ReadMsaaEnvOnce();
  return static_cast<uint32_t>(ClampSamples(MsaaRequested));
}

void ApplySampleCount(uint32_t samples) {
  const auto wanted = static_cast<VkSampleCountFlagBits>(samples == 0 ? 1 : samples);
  if (wanted == SampleCount) {
    return;
  }
  SampleCount = wanted;
  // **Not optional and not deferrable.** `rasterizationSamples` is baked into every one of these
  // and a pipeline whose count disagrees with the attachments is an invalid draw, not a wrong
  // picture - so they go now, while the caller's wait-idle still holds, rather than being marked
  // stale for a frame that would use them first.
  DestroyPipelineCache();
}

// --- HDR ---------------------------------------------------------------------------------------
//
// See SetHdr in VkDraw.h. The knobs are stores; the rebuild they imply happens in
// `ReconcileRenderTarget`, exactly as it does for `render.msaa`.

void SetHdr(bool on) {
  // The env var is consumed here as well as at bring-up, for the reason `SetMsaa` gives: reading
  // it after assigning would put the environment's value back over the caller's.
  ReadHdrEnvOnce();
  HdrEnabled = on;
}

bool Hdr() {
  ReadHdrEnvOnce();
  return HdrEnabled;
}

void SetLinearInput(bool on) { LinearInputEnabled = on; }
bool LinearInput() { return LinearInputEnabled; }

bool LinearInputActive() { return ColourFormat == kHdrFormat && LinearInputEnabled; }

void SetTonemap(uint32_t op) { TonemapOp = op; }
uint32_t Tonemap() { return TonemapOp; }

void SetExposure(float value) { ExposureValue = value; }
float Exposure() { return ExposureValue; }

void SetTonemapKnee(float value) { TonemapKneeValue = value; }
float TonemapKnee() { return TonemapKneeValue; }

void SetTonemapWhite(float value) { TonemapWhiteValue = value; }
float TonemapWhite() { return TonemapWhiteValue; }

uint32_t HdrTargetFormat(uint32_t swapchain_format) {
  ReadHdrEnvOnce();
  // **Gated on the pass, not just on the knob.** A float target nothing can present is a black
  // window, so a device that could not build the tonemap pipeline keeps the 8-bit target and the
  // blit - the same "lose the feature, not the frame" rule the shadow atlases and the AO pass
  // follow. It also means the first frame comes up 8-bit even under `GKPLUS_VK_HDR=1`, because
  // `ReconcileRenderTarget` runs before `StartDraw` builds the pass; the next frame's reconcile
  // notices the mismatch and rebuilds, which costs one frame at startup and nothing after it.
  return (HdrEnabled && TonemapPassReady) ? static_cast<uint32_t>(kHdrFormat) : swapchain_format;
}

void ApplyColourFormat(uint32_t format) {
  const auto wanted = static_cast<VkFormat>(format);
  if (wanted == ColourFormat) {
    return;
  }
  ColourFormat = wanted;
  // **Not optional and not deferrable**, and word for word the reason `ApplySampleCount` gives:
  // the colour attachment's format is baked into every cached pipeline, and a pipeline whose
  // format disagrees with the attachment is an invalid draw rather than a wrong picture. They go
  // now, while the caller's wait-idle still holds.
  DestroyPipelineCache();
}

bool TonemapReady() { return TonemapPassReady; }

void RecordTonemap(void *command_buffer, uint64_t dest_view, uint32_t source_width,
                   uint32_t source_height, uint32_t dest_width, uint32_t dest_height) {
  if (!TonemapPassReady || dest_view == 0 || source_width == 0 || source_height == 0 ||
      dest_width == 0 || dest_height == 0) {
    return;
  }
  auto cmd = static_cast<VkCommandBuffer>(command_buffer);

  VkRenderingAttachmentInfo colour = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  colour.imageView = reinterpret_cast<VkImageView>(dest_view);
  colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  // DONT_CARE: the triangle covers every pixel of the target and the fragment shader writes
  // unconditionally, so a clear would be a second whole-image write. Same reasoning as the AO
  // resolve's.
  colour.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea.extent = {dest_width, dest_height};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &colour;
  vkCmdBeginRendering(cmd, &rendering);

  // **A zero origin, not `ViewportOrigin()`.** The half-pixel shift is a fixed-function
  // convention difference that belongs to the world pass; this triangle is in clip space and its
  // fragment reads its own integer `SV_Position` to pick a source texel, so shifting the viewport
  // would move every pixel's idea of which column it is reading by half a column - which is
  // exactly the registration the nearest scale exists to get right. The AO resolve says the same
  // thing about the same trap.
  VkViewport viewport = {0.0f, 0.0f, static_cast<float>(dest_width),
                         static_cast<float>(dest_height), 0.0f, 1.0f};
  VkRect2D scissor = {{0, 0}, {dest_width, dest_height}};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, TonemapPipeline);
  auto set = reinterpret_cast<VkDescriptorSet>(BindlessDescriptorSet());
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, TonemapLayout, 0, 1, &set, 0,
                          nullptr);

  TonemapPushConstants push = {};
  push.source_texture = kTonemapSourceSlot;
  push.op = TonemapOp;
  // The encode is `linear_input`'s other half and must never be set without it: the buffer only
  // holds linear light when something decoded it on the way in, and encoding gamma-encoded values
  // a second time washes the whole frame out. One flag word derived in one place is what keeps the
  // two from being set independently by accident.
  push.flags = (ColourFormat == kHdrFormat && LinearInputEnabled) ? kEncodeSrgb : 0u;
  push.source_width = source_width;
  push.source_height = source_height;
  push.dest_width = dest_width;
  push.dest_height = dest_height;
  push.exposure = ExposureValue;
  push.knee = TonemapKneeValue;
  push.white = TonemapWhiteValue;
  vkCmdPushConstants(cmd, TonemapLayout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                     &push);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
}

// --- the stock-look preset (§4.87) ------------------------------------------------------------
//
// See SetStock in VkDraw.h for what is in the set and what is deliberately not.

namespace {
// The ten departures, as one value that can be snapshotted and compared. The defaults here are
// the build's, which is what a `stock = false` with nothing saved applies.
struct DepartureSet {
  bool per_pixel_lighting = true;
  bool map_lighting = true;
  bool lighting_maps = true;
  bool sun_shadows = true;
  bool map_shadows = true;
  bool dynamic_shadows = true;
  bool local_shadows = true;
  bool ambient_occlusion = false;
  bool tessellation = false;
  // The only member that is not a bool, and the reason kStock spells its value out rather than
  // relying on a zero: the reproduction here is ONE sample per pixel, not none.
  uint32_t msaa = 1;
  // **`hdr` alone, and deliberately not its four sub-knobs.** `linear_input`, `tonemap`,
  // `exposure`, `knee` and `white` describe how the float target is presented; with `hdr` off
  // there is no float target and none of them does anything, so putting them in the set would mean
  // `stock = false` restoring five values that were already inert - and, worse, would make an
  // operator the user had chosen part of what "the stock look" means.
  bool hdr = false;

  friend bool operator==(const DepartureSet &, const DepartureSet &) = default;
};

// Every departure's reproduction value. Spelled out per field rather than as ten bare `false`s
// because the *list* is the claim - a departure added later whose stock value is something other
// than false would need a member here, and would be invisible as an eleventh positional zero.
// `msaa` is exactly that departure, and a positional zero would have been a sample count of none.
constexpr DepartureSet kStock = {
    .per_pixel_lighting = false,
    .map_lighting = false,
    .lighting_maps = false,
    .sun_shadows = false,
    .map_shadows = false,
    .dynamic_shadows = false,
    .local_shadows = false,
    .ambient_occlusion = false,
    .tessellation = false,
    .msaa = 1,
    .hdr = false,
};

DepartureSet SavedDepartures;
bool HaveSavedDepartures = false;

DepartureSet CurrentDepartures() {
  DepartureSet set;
  // **The wanted value of each, never the effective one.** `AmbientOcclusion()` is false until
  // `CreateAoPass` has run and `TessellationEnabled()` is false on a device with no tessellation
  // shader, so snapshotting through the public getters would read "off" for a knob the user had
  // switched on and restore a frame they never configured. The two env-backed pairs go through
  // their lazy readers instead of at their flags, which is what makes a `GKPLUS_VK_LOCAL_SHADOWS=0`
  // session survive the round trip rather than coming back on out of the header's default.
  set.per_pixel_lighting = PerPixelLightingEnabled();
  set.map_lighting = MapLightingEnabled;
  set.lighting_maps = LightingMaps();
  set.sun_shadows = SunShadowsEnabled;
  set.map_shadows = MapShadowsEnabled;
  set.dynamic_shadows = DynamicShadowsEnabled;
  set.local_shadows = LocalShadowsEnabled();
  set.ambient_occlusion = AoEnabled;
  set.tessellation = TessellationOn;
  // `MsaaWanted` and not `Msaa` for the same reason, and it bites harder here than anywhere else
  // in this function: `Msaa()` reads 1 for a whole frame after a change is asked for, because the
  // reconcile has not run yet - so a snapshot through it would record "off" for a knob the caller
  // had just turned on, and `Stock()` would answer true for a frame that is about to be
  // multisampled.
  set.msaa = MsaaWanted();
  // `Hdr()` and not `ColourFormat == kHdrFormat`, for the reason the whole comment above gives:
  // the format is the effective value and lags the request, so snapshotting through it would
  // record "off" for a knob switched on a frame ago.
  set.hdr = Hdr();
  return set;
}

// Through the setters and not at the flags, because three of them do work rather than assign:
// `SetPerPixelLighting` marks the environment as read so the first draw cannot undo this,
// `SetLocalShadows` forgets every atlas key so a slot is re-earned and re-baked rather than
// sampled from a tile whose light may have moved, and `SetLightingMaps` drops its cache and images
// so a map edited while the game ran is picked up. A preset that assigned the flags would be a
// second path through state those three are careful about, and it would drift from them silently.
void ApplyDepartures(const DepartureSet &set) {
  SetPerPixelLighting(set.per_pixel_lighting);
  SetMapLighting(set.map_lighting);
  SetLightingMaps(set.lighting_maps);
  SetSunShadows(set.sun_shadows);
  SetMapShadows(set.map_shadows);
  SetDynamicShadows(set.dynamic_shadows);
  SetLocalShadows(set.local_shadows);
  SetAmbientOcclusion(set.ambient_occlusion);
  SetTessellationEnabled(set.tessellation);
  SetMsaa(set.msaa);
  SetHdr(set.hdr);
}
} // namespace

void SetStock(bool enabled) {
  if (enabled) {
    // Guarded on the transition, so `stock = true` twice keeps the first snapshot instead of
    // saving the stock values over it and making the way back a no-op.
    if (!Stock()) {
      SavedDepartures = CurrentDepartures();
      HaveSavedDepartures = true;
    }
    ApplyDepartures(kStock);
    return;
  }
  ApplyDepartures(HaveSavedDepartures ? SavedDepartures : DepartureSet{});
}

bool Stock() { return CurrentDepartures() == kStock; }

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
  if (!Ready) {
    return false;
  }
  // The AO targets are the render extent's, so they move with the depth buffer. A failure here
  // takes the pass down rather than the renderer, exactly as a failure at creation does - and
  // `AoReady` false is a state the frame block and the record pass both already handle.
  if (AoReady && !CreateAoTargets(width, height)) {
    DebugWrite("gkplus: ambient occlusion targets could not be resized\n");
    DestroyAoPass();
  }
  return CreateDepth(width, height);
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
  // The same shape and the same trigger: a comparison against TextureRegistryGeneration, which
  // moves only when an image is created, destroyed or named. A texture with no companion file
  // costs one hash lookup once, not a file probe per frame (VkLighting.h).
  EnsureLightingMapsResolved();
  // The game's own stencil shadow, dropped once the sun is casting a real one - otherwise a unit
  // carries both. All three of its passes have stencil on and nothing else in level01 or level02
  // does (§4.31), so this is exact rather than a guess; `render.stencil_shadow` puts it back.
  //
  // Gated on the sun actually drawing, not merely on the knob: with no shadow pipeline or no sun
  // set, dropping the game's shadow would remove the only one there is.
  if (!StencilShadowEnabled && SunShadowsEnabled && ShadowReady && item.pipeline.stencil_enable) {
    ++TheStats.stencil_shadow_draws_hidden;
    return;
  }
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

bool HasUiDraws() {
  for (const DrawItem &item : Items) {
    if (item.ui) {
      return true;
    }
  }
  return false;
}

void RecordDraws(void *command_buffer, Layer layer) {
  // Per frame, not cumulative: the question these answer is "what is the set predicate selecting
  // right now", which a running total cannot say. Zeroed here rather than at the draw loop so an
  // early return below reports 0 rather than the previous frame's figures.
  // **Only on the pass that comes first.** Under `render.hdr` this function runs twice a frame -
  // once per layer - and zeroing here unconditionally would make the UI pass wipe the world pass's
  // figures and report a frame with 261 draws as a frame with 7.
  if (layer != Layer::Ui) {
    TessDrawsThisFrame = 0;
    TessPatchesThisFrame = 0;
  }
  TheStats.items = Items.size();
  if (Items.size() > TheStats.max_items) {
    TheStats.max_items = Items.size();
  }
  TheStats.materials = InternedMaterials.size();
  if (InternedMaterials.size() > TheStats.max_materials) {
    TheStats.max_materials = InternedMaterials.size();
  }
  if (!Ready || Items.empty()) {
    if (layer != Layer::World) {
      ClearDraws();
    }
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
    if (layer != Layer::World) {
      ClearDraws();
    }
    return;
  }

  // **Once a frame, on the first pass only.** Both allocate out of the frame scratch and both
  // publish addresses the draws below push; running them again for the UI pass would allocate a
  // second set and, worse, `UploadFrameData` would hand `Layer::Ui` a block whose `colour_flags`
  // it had just recomputed. The UI pass reads what the world pass published.
  if (layer != Layer::Ui) {
    UploadMapLights();
    UploadFrameData();
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
  // The batching census (DrawStats::batch_runs). Nothing here changes what is submitted - it
  // counts what an indirect world pass could merge, which is the question that decides whether
  // one is worth writing at all.
  // Zeroed on the first pass only, for the reason the tessellation counters above are: under
  // `render.hdr` this function runs once per layer, and the 2D pass would otherwise report its
  // seven draws as the whole frame's batching. The runs it then accumulates are the two layers'
  // added together, which is what the census is asking anyway - it counts what an indirect world
  // pass could merge.
  if (layer != Layer::Ui) {
    TheStats.batch_runs = 0;
    TheStats.batch_longest = 0;
    TheStats.batch_draws = 0;
  }
  uint64_t run_length = 0;
  VkPipeline run_pipeline = VK_NULL_HANDLE;
  uint32_t run_ref = UINT32_MAX, run_mask = 0, run_write_mask = 0;
  float run_min_depth = -1.0f, run_max_depth = -1.0f;
  int32_t run_x = INT32_MIN, run_y = INT32_MIN;
  uint32_t run_width = 0, run_height = 0;
  VkBuffer run_indices = VK_NULL_HANDLE;
  VkIndexType run_index_type = VK_INDEX_TYPE_MAX_ENUM;
  DrawSource run_vertex_source = DrawSource::Arena;
  bool run_open = false;
  const auto close_run = [&]() {
    if (run_open && run_length > TheStats.batch_longest) {
      TheStats.batch_longest = run_length;
    }
  };

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
    // The layer split. `All` records everything, which is the single-pass frame and what runs
    // whenever HDR is off.
    if ((layer == Layer::World && item.ui) || (layer == Layer::Ui && !item.ui)) {
      continue;
    }
    // The tessellation bit is set here, on a copy, rather than by the capture layer: which draws
    // are the level mesh is this renderer's policy and not something D3D was asked for, and the
    // predicates and the material table are both in this file. A draw that does not want it
    // forms exactly the key it formed before the feature existed, which is what makes
    // `render.tessellation = false` bit-identical rather than merely equivalent.
    PipelineState key = item.pipeline;
    // Which target this pass writes, so the pipeline declares the right attachment format. Only
    // ever set on the UI pass, which only ever runs under HDR - so with the feature off every key
    // formed here is the one that was formed before it existed.
    if (layer == Layer::Ui) {
      key.ldr_target = 1;
    }
    if (WantsTessellation(item)) {
      key.tessellate = 1;
      ++TessDrawsThisFrame;
      TessPatchesThisFrame += item.count / 3;
    }
    const VkPipeline pipeline = PipelineFor(key);
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
    // The 2D layers get the copy with `colour_flags` cleared: they are drawn after the tonemap
    // into an 8-bit target, so nothing downstream would re-encode a decoded albedo and the
    // D3DCOLOR clamp their blends were authored against has to stay. See UploadFrameData.
    push.frame = layer == Layer::Ui ? UiFrameDataAddress : FrameDataAddress;
    if (push.vertices == 0) {
      continue;
    }
    // Every stage the range names, whether or not this draw's pipeline has them: the spec
    // requires a push to cover the whole overlapping range, not just the stages that will read
    // it, so naming only VERTEX|FRAGMENT here is invalid the moment the range gained the two
    // tessellation bits. Measured - validation says so on every draw.
    vkCmdPushConstants(cmd, Layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                           VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                           VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                       0, sizeof(push), &push);

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

    // The census, taken after the draw so it sees exactly the state the draw went out with. A
    // non-indexed draw ends the run whatever else matches: `vkCmdDrawIndexedIndirect` cannot
    // carry one.
    const bool same_run =
        run_open && item.indexed && pipeline == run_pipeline && item.stencil_ref == run_ref &&
        item.stencil_mask == run_mask && item.stencil_write_mask == run_write_mask &&
        item.min_depth == run_min_depth && item.max_depth == run_max_depth &&
        item_x == run_x && item_y == run_y && item_width == run_width &&
        item_height == run_height && bound_indices == run_indices &&
        bound_type == run_index_type && item.vertex_source == run_vertex_source;
    ++TheStats.batch_draws;
    if (same_run) {
      ++run_length;
    } else {
      close_run();
      ++TheStats.batch_runs;
      run_length = 1;
      run_open = true;
      run_pipeline = pipeline;
      run_ref = item.stencil_ref;
      run_mask = item.stencil_mask;
      run_write_mask = item.stencil_write_mask;
      run_min_depth = item.min_depth;
      run_max_depth = item.max_depth;
      run_x = item_x;
      run_y = item_y;
      run_width = item_width;
      run_height = item_height;
      run_indices = bound_indices;
      run_index_type = bound_type;
      run_vertex_source = item.vertex_source;
    }
  }
  close_run();
  // **Only on the pass that comes last**, which is the whole hazard of running this twice: the
  // world pass handing the list to `LastItems` would leave the UI pass with nothing to record and
  // the frame with no HUD. `Layer::World` is by construction never the last pass - VkRenderer
  // follows it with `Layer::Ui` unconditionally when it splits at all.
  if (layer != Layer::World) {
    // Kept rather than dropped, so `render.draw_info(i)` can describe the frame that was just
    // recorded. A swap rather than a copy: the buffers trade places and neither allocates.
    LastItems.swap(Items);
    ClearDraws();
  }
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
      // `depth_clamp` is set for a pre-transformed draw and ONLY for one (see PipelineState),
      // so it is this frame's answer to "is this draw 2D" - which is what decides whether the
      // tonemap should touch it. Printed rather than derived from the depth slice, which is only
      // a proxy: the slice says where a draw sits, not how its vertices got there.
      "  alpha test func %u ref %u   shade %u   material %u   depth slice %.4f..%.4f  %s\n"
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
      d.max_depth, p.depth_clamp ? "PRE-TRANSFORMED (2D)" : "3D",
      d.viewport_x, d.viewport_y, d.viewport_width, d.viewport_height,
      d.stage_count,
      d.stages[0].texture_index == kNoTexture ? -1 : (int)d.stages[0].texture_index,
      d.stages[0].sampler_index, d.stages[0].color, d.stages[0].alpha,
      d.stages[1].texture_index == kNoTexture ? -1 : (int)d.stages[1].texture_index,
      d.stages[1].sampler_index, d.stages[1].color, d.stages[1].alpha);
  return line;
}

namespace {
// One bucket of the normal census. `corners` is triangle corners, so three per triangle, and
// every count below is over those unless it says otherwise.
struct NormalCensus {
  uint32_t draws = 0;
  uint64_t triangles = 0;
  uint64_t corners = 0;
  // The four classes of the PN tangent term, `|dot(normalize(edge), normal)|`, taken as the worst
  // of a corner's two edges. `flat` is the class PN triangles cannot move.
  uint64_t flat = 0;    // < 1e-4  - exactly the linear control point, to float precision
  uint64_t near_ = 0;   // < 0.01  - sub-texel bulge, invisible
  uint64_t soft = 0;    // < 0.10
  uint64_t curved = 0;  // >= 0.10 - a real curve, and what the feature exists for
  uint64_t flat_triangles = 0; // all three corners flat: PN is a bit-exact identity on these
  uint64_t degenerate = 0;     // zero-area, so there is no edge direction to test against
  uint64_t no_normal = 0;      // a zero-length vertex normal - the unlit FVFs
  double worst = 0.0;
  double total = 0.0;

  // **The same term again, un-normalised** - `|dot(Pj - Pi, Ni)| / 3`, which is how far b210
  // actually moves off the chord, in the level's own units. The classes above deliberately divide
  // by the edge length so that they mean the same thing on a large triangle and a small one, and
  // that is exactly what hides the failure mode: a modest term on a long edge is a large bulge.
  // A 0.7 term is 3% of a 0.15-unit edge and 23% of a 30-unit pipe.
  uint64_t half_edges = 0;
  double edge_total = 0.0;
  double offset_total = 0.0;
  double offset_worst = 0.0;
  // Split by how far the edge's two endpoint normals disagree, because that is the quantity that
  // separates "a coarsely-facetted smooth surface" from "a crease whose normals were averaged for
  // lighting". Both produce a large term; only the second is wrong to curve. A 6-sided pipe's
  // cross-section edge lands at 60 degrees, an axial edge whose ends were averaged into the end
  // caps lands at 90.
  static constexpr double kCreaseBounds[4] = {30.0, 60.0, 90.0, 1e9};
  uint64_t crease_edges[4] = {};
  double crease_offset_total[4] = {};
  double crease_offset_worst[4] = {};

  // What §4.74's ceiling takes away at the knob's current setting - so the census says what the
  // cap does to this frame rather than only what the defect costs it.
  uint64_t capped = 0;
  double capped_removed = 0.0;

  void HalfEdge(double length, double offset, double disagreement_degrees, double cap) {
    if (offset > cap) {
      ++capped;
      capped_removed += offset - cap;
    }
    ++half_edges;
    edge_total += length;
    offset_total += offset;
    offset_worst = offset > offset_worst ? offset : offset_worst;
    uint32_t klass = 3;
    for (uint32_t i = 0; i < 4; ++i) {
      if (disagreement_degrees < kCreaseBounds[i]) {
        klass = i;
        break;
      }
    }
    ++crease_edges[klass];
    crease_offset_total[klass] += offset;
    crease_offset_worst[klass] =
        offset > crease_offset_worst[klass] ? offset : crease_offset_worst[klass];
  }

  void Corner(double deviation) {
    ++corners;
    total += deviation;
    worst = deviation > worst ? deviation : worst;
    if (deviation < 1e-4) {
      ++flat;
    } else if (deviation < 0.01) {
      ++near_;
    } else if (deviation < 0.10) {
      ++soft;
    } else {
      ++curved;
    }
  }
};

// `|dot(normalize(b - a), n)|` - the tangent term the PN construction divides by the edge
// length. Zero when the normal is perpendicular to the edge, which is the flat case.
//
// `degenerate` distinguishes the two ways this returns zero, which are opposites and must not be
// conflated: a zero-length edge has no direction to be perpendicular to, where a genuinely
// perpendicular normal is the flat case the whole feature is built around.
double TangentTerm(const float *a, const float *b, const float *n, double normal_length,
                   bool &degenerate) {
  const double e[3] = {double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2]};
  const double length = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
  if (length < 1e-9 || normal_length < 1e-9) {
    degenerate = true;
    return 0.0;
  }
  const double dot = e[0] * n[0] + e[1] * n[1] + e[2] * n[2];
  return std::fabs(dot / (length * normal_length));
}

// The world-unit half of the same construction: `|dot(Pj - Pi, Ni)| / (3 * |Ni|)` is exactly how
// far `b210` sits off the chord, in the units the level is authored in. Also reports how far the
// two endpoint normals disagree, in degrees, which is the crease test.
//
// False when either end carries no normal or the edge is degenerate - there is nothing to displace
// in either case, and averaging a zero in would read as "this mesh barely bulges".
// `term_a` and `term_b` come back SIGNED and in the shader's own convention - `dot(Pb - Pa, Na)`
// and `dot(Pa - Pb, Nb)`. The census only ever reports their magnitudes, but the signs are what
// tell a genuine arc (they agree) from normals demanding an S-bend inside one edge (they do not),
// which is the reading §4.74 rests on - so they are not thrown away here.
bool EdgeBulge(const CanonicalVertex &a, const CanonicalVertex &b, double &length,
               double &term_a, double &term_b, double &disagreement_degrees) {
  const double e[3] = {double(b.pos[0]) - a.pos[0], double(b.pos[1]) - a.pos[1],
                       double(b.pos[2]) - a.pos[2]};
  length = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
  const double la = std::sqrt(double(a.normal[0]) * a.normal[0] + double(a.normal[1]) * a.normal[1] +
                              double(a.normal[2]) * a.normal[2]);
  const double lb = std::sqrt(double(b.normal[0]) * b.normal[0] + double(b.normal[1]) * b.normal[1] +
                              double(b.normal[2]) * b.normal[2]);
  if (length < 1e-9 || la < 1e-9 || lb < 1e-9) {
    return false;
  }
  term_a = (e[0] * a.normal[0] + e[1] * a.normal[1] + e[2] * a.normal[2]) / la;
  term_b = -(e[0] * b.normal[0] + e[1] * b.normal[1] + e[2] * b.normal[2]) / lb;
  const double cosine = (double(a.normal[0]) * b.normal[0] + double(a.normal[1]) * b.normal[1] +
                         double(a.normal[2]) * b.normal[2]) /
                        (la * lb);
  disagreement_degrees =
      std::acos(cosine < -1.0 ? -1.0 : (cosine > 1.0 ? 1.0 : cosine)) * (180.0 / 3.14159265358979323846);
  return true;
}

// A percentage to one decimal, as TEXT - never handed to `%f`.
//
// **The CRT this DLL links truncates `%.1f` and signs its zero.** 104 of 1611 printed as `6.4%`
// where the value is 6.456, and a zero percentage printed as `-0.0%`. Reproducing the identical
// `snprintf` call argument-for-argument in a standalone 32-bit clang build prints `6.5` and `0.0`,
// so it is the runtime rather than the arithmetic, the format string or an argument mismatch.
//
// **Rounding on this side first does not fix it and makes it worse**, which is worth stating
// because it was tried: hand a truncating conversion the rounded 1.9 and it prints 1.8, since the
// nearest double to 1.9 is 1.8999999999999999. The two errors compose instead of cancelling, and
// seven of the eight percentages in a level02 census moved 0.1 the wrong way. Keeping the value
// out of the float conversion entirely is the only robust form.
//
// No count is affected either way, and every conclusion in §4.71 rests on counts - but a
// diagnostic whose printed ratio disagrees with its own numerator and denominator cannot be
// checked by hand, which is most of what it is for.
std::string Percent(uint64_t n, uint64_t d) {
  if (d == 0) {
    return "0.0";
  }
  const auto tenths = static_cast<uint64_t>(1000.0 * double(n) / double(d) + 0.5);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%llu.%llu", (unsigned long long)(tenths / 10),
                (unsigned long long)(tenths % 10));
  return buf;
}

void CensusText(std::string &out, const char *what, const NormalCensus &c) {
  char line[512];
  auto pct = [&](uint64_t n) { return Percent(n, c.corners); };
  std::snprintf(line, sizeof(line),
                "  %s: %u draws, %llu triangles, %llu corners\n"
                "    flat   (< 1e-4): %10llu  %5s%%   <- PN is an exact identity here\n"
                "    near   (< 0.01): %10llu  %5s%%\n"
                "    soft   (< 0.10): %10llu  %5s%%\n"
                "    curved (>=0.10): %10llu  %5s%%   <- what tessellation can reach\n"
                "    triangles with all three corners flat: %llu of %llu (%s%%)\n"
                // `worst` and `mean` stay on %f: they are not ratios of two printed integers, so
                // a reader cannot check them by hand anyway and a tenth at the end does not
                // change what either is for.
                "    worst %.4f   mean %.5f   %llu degenerate, %llu with no normal\n",
                what, c.draws, (unsigned long long)c.triangles,
                (unsigned long long)c.corners, (unsigned long long)c.flat, pct(c.flat).c_str(),
                (unsigned long long)c.near_, pct(c.near_).c_str(), (unsigned long long)c.soft,
                pct(c.soft).c_str(), (unsigned long long)c.curved, pct(c.curved).c_str(),
                (unsigned long long)c.flat_triangles, (unsigned long long)c.triangles,
                Percent(c.flat_triangles, c.triangles).c_str(),
                c.worst, c.corners == 0 ? 0.0 : c.total / double(c.corners),
                (unsigned long long)c.degenerate, (unsigned long long)c.no_normal);
  out += line;
  if (c.half_edges == 0) {
    return;
  }
  const auto mean = [](double total, uint64_t n) { return n == 0 ? 0.0 : total / double(n); };
  std::snprintf(line, sizeof(line),
                "    control-point offset, WORLD UNITS (how far b210 leaves the chord):\n"
                "      %llu half-edges, mean edge %.3f, mean offset %.4f, worst %.4f\n"
                "      by how far the edge's two normals disagree:\n"
                "        <30 deg : %8llu  mean %.4f  worst %.4f\n"
                "        30-60   : %8llu  mean %.4f  worst %.4f\n"
                "        60-90   : %8llu  mean %.4f  worst %.4f\n"
                "        >=90    : %8llu  mean %.4f  worst %.4f   <- a crease, not a curve\n",
                (unsigned long long)c.half_edges, mean(c.edge_total, c.half_edges),
                mean(c.offset_total, c.half_edges), c.offset_worst,
                (unsigned long long)c.crease_edges[0],
                mean(c.crease_offset_total[0], c.crease_edges[0]), c.crease_offset_worst[0],
                (unsigned long long)c.crease_edges[1],
                mean(c.crease_offset_total[1], c.crease_edges[1]), c.crease_offset_worst[1],
                (unsigned long long)c.crease_edges[2],
                mean(c.crease_offset_total[2], c.crease_edges[2]), c.crease_offset_worst[2],
                (unsigned long long)c.crease_edges[3],
                mean(c.crease_offset_total[3], c.crease_edges[3]), c.crease_offset_worst[3]);
  out += line;
  std::snprintf(line, sizeof(line),
                "      the ceiling at the current pn_max_offset: %llu half-edges over it (%s%%), "
                "%.2f of %.2f total offset removed (%s%%)\n",
                (unsigned long long)c.capped, Percent(c.capped, c.half_edges).c_str(),
                c.capped_removed, c.offset_total,
                Percent(uint64_t(c.capped_removed * 1000.0), uint64_t(c.offset_total * 1000.0))
                    .c_str());
  out += line;
}

} // namespace

std::string DescribeNormalCensus() {
  if (LastItems.empty()) {
    return "no frame has been recorded yet\n";
  }
  NormalCensus map, other;
  // Read from the live knob rather than hard-coded, so the cap row below answers "what does the
  // current setting do to this frame" - which is what makes a REPL sweep of it readable.
  const double cap = double(TessParams().pn_max_offset);
  CensusSkips skips;

  std::vector<CanonicalVertex> vertices;
  std::vector<uint32_t> indices;

  for (const DrawItem &item : LastItems) {
    int64_t lowest = 0;
    if (!ReadDrawGeometry(item, skips, indices, vertices, lowest)) {
      continue;
    }

    NormalCensus &bucket = IsMapGeometry(item) ? map : other;
    ++bucket.draws;
    for (uint32_t t = 0; t + 2 < item.count; t += 3) {
      const CanonicalVertex *corner[3];
      for (uint32_t k = 0; k < 3; ++k) {
        const int64_t v =
            int64_t(item.base_vertex) + int64_t(indices[t + k]) + item.vertex_offset - lowest;
        corner[k] = &vertices[size_t(v)];
      }
      ++bucket.triangles;
      bool all_flat = true;
      for (uint32_t k = 0; k < 3; ++k) {
        const float *n = corner[k]->normal;
        const double length = std::sqrt(double(n[0]) * n[0] + double(n[1]) * n[1] +
                                        double(n[2]) * n[2]);
        if (length < 1e-9) {
          ++bucket.no_normal;
          ++bucket.corners;
          ++bucket.flat; // no normal is no curvature, and it must not read as one
          continue;
        }
        // The worse of the corner's two outgoing edges: the control point on either one bulges,
        // so a corner is only flat if both terms vanish.
        bool degenerate_a = false, degenerate_b = false;
        const double a =
            TangentTerm(corner[k]->pos, corner[(k + 1) % 3]->pos, n, length, degenerate_a);
        const double b =
            TangentTerm(corner[k]->pos, corner[(k + 2) % 3]->pos, n, length, degenerate_b);
        const double deviation = a > b ? a : b;
        if (degenerate_a || degenerate_b) {
          // A zero-length edge: the triangle has no extent at this corner, so there is nothing
          // to be flat or curved about and the zero it contributes is not evidence of flatness.
          // Counted apart for that reason - folding it into `flat` would make a sliver-heavy
          // mesh read as a mesh that tessellation cannot change.
          ++bucket.degenerate;
        }
        bucket.Corner(deviation);
        all_flat = all_flat && deviation < 1e-4;
      }
      bucket.flat_triangles += all_flat ? 1 : 0;
      // A second pass over the same triangle, per EDGE rather than per corner, because the two
      // questions the un-normalised metric answers are both edge-shaped: how far the control point
      // moves in world units, and how far the edge's two normals disagree.
      for (uint32_t k = 0; k < 3; ++k) {
        double length = 0.0, term_a = 0.0, term_b = 0.0, disagreement = 0.0;
        if (EdgeBulge(*corner[k], *corner[(k + 1) % 3], length, term_a, term_b, disagreement)) {
          bucket.HalfEdge(length, std::fabs(term_a) / 3.0, disagreement, cap);
          bucket.HalfEdge(length, std::fabs(term_b) / 3.0, disagreement, cap);
        }
      }
    }
  }

  std::string out;
  char line[512];
  std::snprintf(line, sizeof(line),
                "normal census over %u draws of the last frame (%u examined)\n"
                "  the metric is |dot(normalize(edge), normal)| - the PN tangent term over edge "
                "length,\n"
                "  so a corner reading d bulges its edge by about d * length / 3\n",
                static_cast<unsigned>(LastItems.size()), skips.examined);
  out += line;
  CensusText(out, "map geometry (IsMapGeometry)", map);
  CensusText(out, "everything else (props, units, effects)", other);
  std::snprintf(line, sizeof(line),
                "  not examined: %u not arena-indexed, %u not a triangle list, %u over the %u "
                "draw cap, %u arena read failures\n",
                skips.source, skips.topology, skips.over_cap, kMaxCensusDraws,
                skips.read_failures);
  out += line;
  return out;
}

namespace {
// --- the seam census (§4.71) ------------------------------------------------------------------
//
// The normal census answers "is there curvature for tessellation to find". This one answers the
// question that comes *after* it: **where two triangles meet, do their two PN patches agree?**
//
// The construction's watertight property is conditional, and the condition is about the DATA
// rather than about the arithmetic: `b210` for edge (P1,P2) is a function of P1, P2 and N1
// alone, so the two triangles sharing that edge build the same boundary curve **provided they
// present the same P and the same N at each end**. Where the mesh splits a corner into two
// vertices with different normals - which is what an exporter does at a material boundary or a
// smoothing-group break - the two curves are different curves through the same two points, and
// the patches pull apart into a visible tear whose width is bounded by the mid-edge deviation.
//
// So this walks the frame's triangles, keys every edge on the bit patterns of its two endpoint
// positions, and for each edge used by exactly two triangles measures the widest separation of
// the two boundary curves in world units. Keying on the position BITS rather than on the index
// is the whole point: a duplicated corner has a different index in each draw and the identical
// position, which is exactly the case that has to be caught.

// An edge, canonically ordered so the two triangles sharing it produce the same key.
struct SeamKey {
  uint32_t bits[6];
  bool operator<(const SeamKey &o) const {
    return std::memcmp(bits, o.bits, sizeof(bits)) < 0;
  }
};

// One triangle's view of an edge: the two endpoint normals, in the key's own order.
struct SeamSide {
  float normal_a[3], normal_b[3];
  bool tessellated;
  bool map_geometry;
};

struct SeamEntry {
  SeamKey key;
  SeamSide side[2];
  uint32_t uses; // counted past 2, so a non-manifold edge is reported rather than silently kept
};

SeamKey MakeSeamKey(const float *a, const float *b, bool &swapped) {
  uint32_t pa[3], pb[3];
  std::memcpy(pa, a, sizeof(pa));
  std::memcpy(pb, b, sizeof(pb));
  // Ordered on the raw bit patterns and not on the float values: this only has to be a total
  // order that both sides agree on, and NaN or a signed zero would make a value comparison
  // disagree with itself. A -0.0 and a +0.0 corner would key apart, which is the one case this
  // gets wrong - and it is the right way round, since it reports a seam that is not one rather
  // than hiding one that is.
  swapped = std::memcmp(pa, pb, sizeof(pa)) > 0;
  SeamKey key;
  std::memcpy(key.bits, swapped ? pb : pa, sizeof(pa));
  std::memcpy(key.bits + 3, swapped ? pa : pb, sizeof(pb));
  return key;
}

// `pn_weight` from world.slang, on the CPU and at the live settings, so this reports the tear the
// current knobs produce rather than the one the defaults would. Kept deliberately expression-for-
// expression with the shader - if the two ever disagree, this instrument is the thing that lies.
double PnWeight(const float *pi, const float *pj, const double *ni_unit,
                const TessellationParams &p) {
  const double edge[3] = {double(pj[0]) - pi[0], double(pj[1]) - pi[1], double(pj[2]) - pi[2]};
  const double length_sq = edge[0] * edge[0] + edge[1] * edge[1] + edge[2] * edge[2];
  if (length_sq < 1e-18) {
    return 0.0;
  }
  const double w = edge[0] * ni_unit[0] + edge[1] * ni_unit[1] + edge[2] * ni_unit[2];
  const double threshold = double(p.pn_flat_threshold);
  if (w * w <= threshold * threshold * length_sq) {
    return 0.0;
  }
  const double limit = 3.0 * double(p.pn_max_offset);
  const double clamped = w < -limit ? -limit : (w > limit ? limit : w);
  return clamped * double(p.pn_strength);
}

// False when the normal is unusable, which is not the same as flat: `normalize` on a zero vector
// is a NaN in the shader too, so a corner with no normal is a case this cannot answer rather than
// one it should score as agreeing.
bool UnitNormal(const float *n, double *out) {
  const double length =
      std::sqrt(double(n[0]) * n[0] + double(n[1]) * n[1] + double(n[2]) * n[2]);
  if (length < 1e-9) {
    return false;
  }
  out[0] = n[0] / length;
  out[1] = n[1] / length;
  out[2] = n[2] / length;
  return true;
}

// The two interior control points of the cubic boundary curve, in the key's order.
//
// On edge (p1,p2) the domain shader's third barycentric is zero, so the position collapses to
// `p1*u^3 + 3*b210*u^2*v + 3*b120*u*v^2 + p2*v^3` with `u = 1-t, v = t` - an ordinary cubic
// Bezier with control points p1, b210, b120, p2. Only the middle two can differ between the two
// triangles, so they are the whole tear.
// `split_a`/`split_b` are the shader's own rule: a corner the mesh has split contributes no
// tangent term, so that end of the curve is linear on both sides and cannot disagree.
bool BoundaryControls(const float *pa, const float *pb, const float *na, const float *nb,
                      const TessellationParams &p, bool split_a, bool split_b, double *b1,
                      double *b2) {
  double ua[3], ub[3];
  if (!UnitNormal(na, ua) || !UnitNormal(nb, ub)) {
    return false;
  }
  const double wa = split_a ? 0.0 : PnWeight(pa, pb, ua, p);
  const double wb = split_b ? 0.0 : PnWeight(pb, pa, ub, p);
  for (uint32_t i = 0; i < 3; ++i) {
    b1[i] = (2.0 * pa[i] + pb[i] - wa * ua[i]) / 3.0;
    b2[i] = (2.0 * pb[i] + pa[i] - wb * ub[i]) / 3.0;
  }
  return true;
}

// The widest separation of two cubics that share their endpoints.
//
// Their difference is `3*(1-t)^2*t*db1 + 3*(1-t)*t^2*db2`, a smooth curve vanishing at both
// ends, so a fixed sample of the interior finds its maximum to well within what this is for.
// Sampled rather than solved because the answer wanted is "how wide is the tear", not the
// parameter it is widest at.
double WidestGap(const double *b1a, const double *b2a, const double *b1b, const double *b2b) {
  double worst = 0.0;
  for (uint32_t step = 1; step < 8; ++step) {
    const double t = double(step) / 8.0;
    const double u = 1.0 - t;
    const double ca = 3.0 * u * u * t, cb = 3.0 * u * t * t;
    double d = 0.0;
    for (uint32_t i = 0; i < 3; ++i) {
      const double delta = ca * (b1a[i] - b1b[i]) + cb * (b2a[i] - b2b[i]);
      d += delta * delta;
    }
    worst = d > worst ? d : worst;
  }
  return std::sqrt(worst);
}

struct SeamCensus {
  uint64_t edges = 0;       // distinct edges seen
  uint64_t shared = 0;      // used by exactly two triangles
  uint64_t border = 0;      // used once - a genuine mesh boundary, or a T-junction's long side
  uint64_t nonmanifold = 0; // used more than twice
  uint64_t agree = 0;       // shared, and both endpoint normals identical: watertight
  uint64_t differ = 0;      // shared, normals differ: a tear
  uint64_t no_normal = 0;   // shared, but a corner carries no normal so the question is undefined
  uint64_t tess_split = 0;  // shared, and only ONE side takes the tessellated pipeline
  uint64_t open = 0;        // of `differ`, those whose gap survives the current knobs
  double gap_total = 0.0;
  double gap_worst = 0.0;
  float worst_at[3] = {0.0f, 0.0f, 0.0f};
  // The same three with the split-corner rule applied - **recomputed here rather than read off
  // the live table**, so agreeing with the shader is evidence and not a tautology. This is the
  // check on `render.pn_seam_fix`: it must be 0 open, on every level and at every knob setting.
  uint64_t open_fixed = 0;
  double gap_fixed_total = 0.0;
  double gap_fixed_worst = 0.0;
};

void SeamText(std::string &out, const char *what, const SeamCensus &c) {
  char line[768];
  std::snprintf(
      line, sizeof(line),
      "  %s: %llu distinct edges\n"
      "    shared by two triangles : %10llu  %5s%%\n"
      "      normals agree         : %10llu  %5s%%   <- watertight by construction\n"
      "      normals differ        : %10llu  %5s%%   <- a tear unless the knobs close it\n"
      "      a corner has no normal: %10llu  %5s%%\n"
      "      only one side tessellated: %7llu  %5s%%   <- a tear the predicates opened\n"
      "    used once (mesh border) : %10llu  %5s%%\n"
      "    used more than twice    : %10llu  %5s%%\n",
      what, (unsigned long long)c.edges, (unsigned long long)c.shared,
      Percent(c.shared, c.edges).c_str(), (unsigned long long)c.agree,
      Percent(c.agree, c.shared).c_str(), (unsigned long long)c.differ,
      Percent(c.differ, c.shared).c_str(), (unsigned long long)c.no_normal,
      Percent(c.no_normal, c.shared).c_str(), (unsigned long long)c.tess_split,
      Percent(c.tess_split, c.shared).c_str(), (unsigned long long)c.border,
      Percent(c.border, c.edges).c_str(), (unsigned long long)c.nonmanifold,
      Percent(c.nonmanifold, c.edges).c_str());
  out += line;
  if (c.differ == 0) {
    return;
  }
  std::snprintf(line, sizeof(line),
                "    the gap at the current pn_strength / pn_flat_threshold / pn_max_offset:\n"
                "      %llu of the %llu still open (%s%%), mean %.4f, worst %.4f world units\n"
                "      worst at (%.2f, %.2f, %.2f)\n"
                "    ... and with the split-corner rule applied (render.pn_seam_fix):\n"
                "      %llu still open, mean %.4f, worst %.4f   <- has to be 0\n",
                (unsigned long long)c.open, (unsigned long long)c.differ,
                Percent(c.open, c.differ).c_str(),
                c.open == 0 ? 0.0 : c.gap_total / double(c.open), c.gap_worst, c.worst_at[0],
                c.worst_at[1], c.worst_at[2], (unsigned long long)c.open_fixed,
                c.open_fixed == 0 ? 0.0 : c.gap_fixed_total / double(c.open_fixed),
                c.gap_fixed_worst);
  out += line;
}
} // namespace

std::string DescribeSeamCensus() {
  if (LastItems.empty()) {
    return "no frame has been recorded yet\n";
  }
  const TessellationParams &params = TessParams();
  CensusSkips skips;
  std::map<SeamKey, SeamEntry> edges;
  // The split-corner rule, re-derived here from the frame's own geometry rather than read off the
  // live table. That is what makes "0 still open with the rule applied" evidence about the rule
  // instead of a restatement of whatever the table happens to hold: two independent walks of the
  // same mesh have to reach the same set. Keyed on the position, like everything else here.
  // Comparing every corner against the FIRST normal seen at its position is enough to answer
  // "are they all equal", which is the question - a third normal that matches the first still
  // leaves the position split, and the flag is never cleared.
  struct CensusSplit {
    float normal[3];
    bool split;
  };
  std::map<PositionKey, CensusSplit> split;
  std::vector<CanonicalVertex> vertices;
  std::vector<uint32_t> indices;
  uint64_t triangles = 0;

  const auto note_split = [&split](const CanonicalVertex &vertex) {
    PositionKey key;
    std::memcpy(key.bits, vertex.pos, sizeof(key.bits));
    const auto found = split.find(key);
    if (found == split.end()) {
      CensusSplit entry = {};
      std::memcpy(entry.normal, vertex.normal, sizeof(entry.normal));
      split.emplace(key, entry);
    } else if (std::memcmp(found->second.normal, vertex.normal,
                           sizeof(found->second.normal)) != 0) {
      found->second.split = true;
    }
  };
  const auto is_split = [&split](const float *pos) {
    PositionKey key;
    std::memcpy(key.bits, pos, sizeof(key.bits));
    const auto found = split.find(key);
    return found != split.end() && found->second.split;
  };

  for (const DrawItem &item : LastItems) {
    int64_t lowest = 0;
    if (!ReadDrawGeometry(item, skips, indices, vertices, lowest)) {
      continue;
    }
    const bool is_map = IsMapGeometry(item);
    const bool tessellated = WantsTessellation(item);
    for (uint32_t t = 0; t + 2 < item.count; t += 3) {
      const CanonicalVertex *corner[3];
      for (uint32_t k = 0; k < 3; ++k) {
        const int64_t v =
            int64_t(item.base_vertex) + int64_t(indices[t + k]) + item.vertex_offset - lowest;
        corner[k] = &vertices[size_t(v)];
      }
      ++triangles;
      for (uint32_t k = 0; k < 3; ++k) {
        note_split(*corner[k]);
      }
      for (uint32_t k = 0; k < 3; ++k) {
        const CanonicalVertex &a = *corner[k];
        const CanonicalVertex &b = *corner[(k + 1) % 3];
        bool swapped = false;
        const SeamKey key = MakeSeamKey(a.pos, b.pos, swapped);
        SeamEntry &entry = edges[key];
        entry.key = key;
        if (entry.uses < 2) {
          SeamSide &side = entry.side[entry.uses];
          const CanonicalVertex &first = swapped ? b : a;
          const CanonicalVertex &second = swapped ? a : b;
          std::memcpy(side.normal_a, first.normal, sizeof(side.normal_a));
          std::memcpy(side.normal_b, second.normal, sizeof(side.normal_b));
          side.tessellated = tessellated;
          side.map_geometry = is_map;
        }
        ++entry.uses;
      }
    }
  }

  size_t split_positions = 0;
  for (const auto &pair : split) {
    split_positions += pair.second.split ? 1 : 0;
  }

  SeamCensus map, other;
  for (const auto &pair : edges) {
    const SeamEntry &entry = pair.second;
    // An edge is the map's when everything meeting along it is - a seam between the map object
    // and a prop belongs in neither bucket's "watertight" column, and putting it in `other` is
    // what keeps the map row answering only for §4.65's set.
    const bool is_map =
        entry.side[0].map_geometry && (entry.uses < 2 || entry.side[1].map_geometry);
    SeamCensus &bucket = is_map ? map : other;
    ++bucket.edges;
    if (entry.uses == 1) {
      ++bucket.border;
      continue;
    }
    if (entry.uses > 2) {
      ++bucket.nonmanifold;
      continue;
    }
    ++bucket.shared;
    if (entry.side[0].tessellated != entry.side[1].tessellated) {
      ++bucket.tess_split;
    }
    const bool same = std::memcmp(entry.side[0].normal_a, entry.side[1].normal_a,
                                  sizeof(entry.side[0].normal_a)) == 0 &&
                      std::memcmp(entry.side[0].normal_b, entry.side[1].normal_b,
                                  sizeof(entry.side[0].normal_b)) == 0;
    if (same) {
      ++bucket.agree;
      continue;
    }
    const float *pa = reinterpret_cast<const float *>(entry.key.bits);
    const float *pb = reinterpret_cast<const float *>(entry.key.bits + 3);
    double b1a[3], b2a[3], b1b[3], b2b[3];
    if (!BoundaryControls(pa, pb, entry.side[0].normal_a, entry.side[0].normal_b, params, false,
                          false, b1a, b2a) ||
        !BoundaryControls(pa, pb, entry.side[1].normal_a, entry.side[1].normal_b, params, false,
                          false, b1b, b2b)) {
      ++bucket.no_normal;
      continue;
    }
    ++bucket.differ;
    const double gap = WidestGap(b1a, b2a, b1b, b2b);
    if (gap > 0.0) {
      ++bucket.open;
      bucket.gap_total += gap;
      if (gap > bucket.gap_worst) {
        bucket.gap_worst = gap;
        bucket.worst_at[0] = pa[0];
        bucket.worst_at[1] = pa[1];
        bucket.worst_at[2] = pa[2];
      }
    }
    // The same edge again under the split-corner rule. An edge only reaches here because the two
    // sides' normals differ, so at least one of its ends IS split by definition - which is why
    // this must come out at zero, and why a non-zero would mean the rule as implemented is not
    // the rule as derived rather than that some seam is merely unlucky.
    const bool split_a = is_split(pa), split_b = is_split(pb);
    if (BoundaryControls(pa, pb, entry.side[0].normal_a, entry.side[0].normal_b, params, split_a,
                         split_b, b1a, b2a) &&
        BoundaryControls(pa, pb, entry.side[1].normal_a, entry.side[1].normal_b, params, split_a,
                         split_b, b1b, b2b)) {
      const double fixed = WidestGap(b1a, b2a, b1b, b2b);
      if (fixed > 0.0) {
        ++bucket.open_fixed;
        bucket.gap_fixed_total += fixed;
        bucket.gap_fixed_worst =
            fixed > bucket.gap_fixed_worst ? fixed : bucket.gap_fixed_worst;
      }
    }
  }

  std::string out;
  char line[768];
  std::snprintf(line, sizeof(line),
                "seam census over %u draws of the last frame (%u examined), %llu triangles\n"
                "  edges are keyed on the BITS of their two endpoint positions, so a corner the\n"
                "  mesh has split into two vertices still reads as one edge - which is the case\n"
                "  the whole question is about\n"
                "  the gap is in WORLD UNITS: the widest separation of the two boundary curves\n",
                static_cast<unsigned>(LastItems.size()), skips.examined,
                (unsigned long long)triangles);
  out += line;
  SeamText(out, "map geometry (IsMapGeometry)", map);
  SeamText(out, "everything else, and map-to-prop seams", other);
  // The table the shader is actually reading, beside the rule this function just re-derived.
  // `pending` is the one to watch after a level load: the analysis is an arena readback and is
  // deliberately spread over frames, so a non-zero here means the table is still converging and
  // some seams are legitimately open this frame.
  uint32_t corners = 0, analysed = 0, pending = 0;
  bool too_large = false;
  SplitCornerCounts(corners, analysed, pending, too_large);
  std::snprintf(line, sizeof(line),
                "  the live table (render.pn_seam_fix %s): %u corners marked over %u draws "
                "analysed, %u still queued%s\n"
                "  this census found %u split positions of its own\n",
                SplitCornerFix() ? "on" : "off", corners, analysed, pending,
                too_large ? ", AND THE BITSET DID NOT FIT ITS SCRATCH SLICE" : "",
                static_cast<unsigned>(split_positions));
  out += line;
  std::snprintf(line, sizeof(line),
                "  not examined: %u not arena-indexed, %u not a triangle list, %u over the %u "
                "draw cap, %u arena read failures\n",
                skips.source, skips.topology, skips.over_cap, kMaxCensusDraws,
                skips.read_failures);
  out += line;
  return out;
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
      TheStats.dropped_over_capacity + TheStats.dropped_materials + TheStats.hidden_draws +
      TheStats.stencil_shadow_draws_hidden;
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
  // Printed unconditionally rather than only when off, because this one is a departure from the
  // original: a reader comparing a shot against real D3D8 has to know which equation ran, and
  // "no line" would read as "the fixed-function one" to anyone who had not been told otherwise.
  add("light sum: per %s\n", PerPixelLightingEnabled() ? "PIXEL" : "vertex (the original)");
  // Printed unconditionally for the same reason, and it has to say all three parts:
  // `render.hdr` reads back as REQUESTED, so a device with no tonemap pass and the one frame
  // between a write and the reconcile both read `hdr = true` while the frame is still 8-bit.
  // This is where that difference is visible, and "requested but not engaged" is the only
  // wording that tells "I asked for it" from "it is happening".
  if (Hdr() || ColourFormat == kHdrFormat) {
    const bool engaged = ColourFormat == kHdrFormat;
    add("hdr: %s, %s input, tonemap %s, exposure %.2f (knee %.2f, white %.2f)\n",
        engaged ? "R16G16B16A16_SFLOAT"
                : (TonemapPassReady ? "requested, not engaged yet"
                                    : "requested, NO TONEMAP PASS ON THIS DEVICE"),
        (engaged && LinearInputEnabled) ? "linear" : "gamma (linear_input off)",
        TonemapOp == 1   ? "rolloff"
        : TonemapOp == 2 ? "reinhard"
        : TonemapOp == 3 ? "aces"
        : TonemapOp == 4 ? "filmic"
        : TonemapOp == 5 ? "agx"
                         : "clamp",
        ExposureValue, TonemapKneeValue, TonemapWhiteValue);
  }
  // Only when it is on, for the reason the material overrides are: a `0 draws, 0 patches` on every
  // report would read as an invariant of the renderer rather than as "nobody asked for it"
  // (§4.44). A device with no `tessellationShader` therefore prints nothing whatever the knob
  // says, which is the same statement.
  if (TessellationEnabled()) {
    add("tessellation: PN triangles over %s, %u draws / %u patches this frame\n"
        "  edge %.0f px, factor %.1f..%.1f, pn strength %.2f, flat threshold %.3f%s\n",
        TessellationSet() == TessSet::All ? "every opaque indexed draw" : "the map only",
        TessDrawsThisFrame, TessPatchesThisFrame, TheTessParams.edge_pixels,
        TheTessParams.min_factor, TheTessParams.max_factor, TheTessParams.pn_strength,
        TheTessParams.pn_flat_threshold,
        TessellationShadowsOn ? "" : "   (shadow passes NOT tessellated)");
  }
  if (MapLightingEnabled) {
    // `grid builds` is the invariant here: the map's lights are static, so this is **once per
    // level**. A number that climbs with the frame count means the rebuild test has broken and
    // the dispatch is running every frame - which would still draw correctly, and so would never
    // show up as anything but a frame rate.
    add("map lighting: %u lights, gain %.2f, %s (%llu grid builds - one per level)\n",
        FrameMapLightCount, MapLightGainValue,
        MapLightCullEnabled ? (GridValid ? "culled by the world grid" : "grid NOT built")
                            : "every light per pixel",
        (unsigned long long)TheStats.light_grid_builds);
    // One line, with `render.map_shadow_report` for the rest. `baked N/M` is the one number worth
    // having here: a bake still in progress and a bake that never started look the same on screen.
    if (MapShadowReady) {
      add("  static shadows: %s, baked %u/%u lights (%u refused, offset %.2f texels)\n",
          MapShadowsEnabled ? "on" : "off (render.map_shadows)", MapShadowCursor,
          (unsigned)MapShadowLightForSlot.size(), MapShadowRefused, MapShadowBiasValue);
    }
  }
  // Four states, not two: a level with no sun set produces no matrix and therefore no shadow,
  // and a device that could not build the pipeline produces none either. Both look exactly like
  // the knob being off from the screen, so the report is what tells them apart.
  add("sun shadows: %s (%llu casters over %u cascades, bias %.2f texels, strength %.2f, "
      "extent %.0f)\n",
      !SunShadowsEnabled ? "off"
                         : (!ShadowReady ? "NO PIPELINE" : (FrameSunValid ? "on" : "no sun")),
      (unsigned long long)TheStats.shadow_casters, FrameCascadeCount, ShadowBiasValue,
      ShadowStrengthValue, ShadowExtentValue);
  if (ShadowReady) {
    // The near cascade's extent and its texel are what "sharp" means here, and they are the two
    // numbers a level's scale actually moves - printed rather than derived, because the halving
    // makes the near extent depend on the cascade COUNT as well as on `shadow_extent`.
    float near_extent = ShadowExtentValue;
    for (int i = 1; i < ShadowCascadeCount; ++i) {
      near_extent *= 0.5f;
    }
    add("  atlas %ux%u (%u KB, format %u), tile %u: near cascade +-%.2f world units, "
        "%.4f per texel\n",
        kShadowAtlas, kShadowAtlas, (unsigned)(ShadowBytes / 1024), (unsigned)ShadowFormat,
        kShadowTile, near_extent, near_extent * 2.0f / static_cast<float>(kShadowTile));
  }
  if (TheStats.stencil_shadow_draws_hidden > 0) {
    add("  the game's own stencil shadow: %llu draws dropped (render.stencil_shadow puts it "
        "back)\n",
        (unsigned long long)TheStats.stencil_shadow_draws_hidden);
  }
  // The local half of the static atlas (§4.65), in one line - `render.local_shadow_report` is the
  // detail. `holding` against `keys` is the reading: the gap is lights that move, which is the
  // design working rather than failing.
  {
    uint32_t held = 0;
    uint32_t moving = 0;
    for (const auto &[key, entry] : LocalShadowKeys) {
      held += entry.slot >= 0 && entry.baked ? 1u : 0u;
      moving += entry.slot < 0 && entry.stable < kLocalShadowStableFrames ? 1u : 0u;
    }
    add("local light shadows: %s (%u of %u keys hold a baked cube, %u reserved slots, "
        "%u still moving)\n",
        LocalShadowsEnabled() ? "on" : "off", held, (unsigned)LocalShadowKeys.size(),
        kLocalShadowSlots, moving);
  }
  // The per-frame atlas (§4.66). `casters` against the sun's is the reading: this one takes the
  // same set the sun's pass does, so a large gap means something is being bucketed away.
  add("per-frame light shadows: %s (%u lights x 6 faces, %u casters in %u buckets, %u refused, "
      "%u dropped)\n",
      !DynamicShadowsEnabled ? "off" : (DynShadowReady ? "on" : "UNAVAILABLE"),
      (unsigned)DynLights.size(), DynCasters, DynBuckets, DynRefused, DynCastersDropped);
  // Ambient occlusion (§4.86). `draws` against the sun's caster count is the reading: both walk
  // the same collector, so the two should differ only by the sun's cascade multiplier.
  if (!AoEnabled) {
    add("ambient occlusion: off%s\n", AoReady ? "" : " (UNAVAILABLE - the pass did not build)");
  } else {
    add("ambient occlusion: %s (%u draws, %ux%u, radius %.2f units / %.4f of height = %.1f px, "
        "%u taps, bias %.3f, strength %.2f, direct %.2f%s%s)\n",
        AoReady ? (AoRanThisFrame ? "on" : "on but idle - no casters this frame") : "UNAVAILABLE",
        AoDrawCalls, AoWidth, AoHeight, AoRadiusValue, AoScreenRadiusValue,
        AoScreenRadiusValue * static_cast<float>(AoHeight), AoTapsValue, AoBiasValue,
        AoStrengthValue, AoDirectValue, AoMapOnlyEnabled ? ", map only" : "",
        AoDebugEnabled ? ", DEBUG VIEW" : "");
    if (AoCastersDropped != 0) {
      add("  casters refused by the %u-command cap: %u\n", kShadowMaxCommands, AoCastersDropped);
    }
  }
  add("viewport depth-slice changes: %llu\n", (unsigned long long)TheStats.viewport_sets);
  add("index binds: %llu   pipelines: %llu (%llu binds, %llu failures - must be 0)\n",
      (unsigned long long)TheStats.index_binds, (unsigned long long)TheStats.pipelines,
      (unsigned long long)TheStats.pipeline_binds,
      (unsigned long long)TheStats.pipeline_failures);
  // **What an indirect world pass could merge, and nothing more.** See DrawStats::batch_runs:
  // runs are consecutive only, because the game's order is what makes blending come out right.
  // `drawn / runs` is the ceiling on the draw calls one would remove.
  add("batchable runs: %llu over %llu draws (mean %.2f, longest %llu)\n",
      (unsigned long long)TheStats.batch_runs, (unsigned long long)TheStats.batch_draws,
      TheStats.batch_runs == 0
          ? 0.0
          : static_cast<double>(TheStats.batch_draws) / TheStats.batch_runs,
      (unsigned long long)TheStats.batch_longest);
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
  // Printed on the same terms and for the same reason: only once a companion file has actually
  // been found, so "no mod ships one" stays silent rather than reading as a broken feature.
  const LightingMapStats &lighting = LightingMapCounters();
  if (lighting.maps_found != 0 || lighting.load_failures != 0) {
    add("lighting maps: %llu images from %llu files (%llu refused - see "
        "render.lighting_map_report), %llu draws lit%s\n",
        (unsigned long long)lighting.images_created, (unsigned long long)lighting.maps_found,
        (unsigned long long)lighting.load_failures,
        (unsigned long long)lighting.materials_lit, LightingMaps() ? "" : "  (OFF)");
  }
  return out;
}

void ShutdownDraw() {
  // Reverse creation order: the per-frame atlas borrows the map's module, and the map's borrows
  // the sun's module and layout. Destroying a pipeline after its layout is legal, but this is the
  // order that reads correctly.
  DestroyTonemapPass();
  // Forgotten with the pass, not kept: `StartDraw` only adopts the swapchain's format when this is
  // UNDEFINED, so leaving a stale one here would make a recreated device draw into whatever the
  // last one happened to be using.
  ColourFormat = VK_FORMAT_UNDEFINED;
  SwapchainFormat = VK_FORMAT_UNDEFINED;
  DestroyAoPass();
  DestroyDynShadowAtlas();
  DestroyMapShadowAtlas();
  DestroyShadowPass();
  DestroyLightGridPipeline();
  if (!Ready) {
    return;
  }
  {
    QueueGuard queue_guard(QueueMutex());
    vkDeviceWaitIdle(GetDevice());
  }
  // Before the resources go, and from here rather than from the renderer: these are images this
  // side created, and nothing outside the draw path knows they exist.
  ShutdownLightingMaps();
  DestroyDepth();
  DestroyPipelineCache();
  for (VkShaderModule *module :
       {&VertexModule, &FragmentModule, &TessVertexModule, &HullModule, &DomainModule}) {
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
