// The capture layer's evidence: the histograms, the verifiers, the frame draw log and every
// `render.*` reading built on them. `D3D8Capture.cpp` is the recorder; this is what asks it
// questions. `D3D8CaptureInternal.h` is the seam, and its header comment is the argument for
// the split.
//
// Nothing here is on the path a frame takes. The `Note*`/`Log*` collectors are called from the
// recorder's draw path and only ever append; everything else runs from the REPL, at Present.
// That direction is the invariant worth keeping - a diagnostic that mutates state the renderer
// then reads is not a diagnostic, and this file is where that mistake would be made.
//
// Two things learned the hard way live here rather than in the recorder, because they are
// properties of the INSTRUMENTS:
//
//   * a deferred readback proves consistency, not correctness. `VerifyBufferSlots` and
//     `DescribeWatchedDrawGeometry`'s late columns read the game's buffer as it stands now,
//     which is a frame or more after the draw they describe - long enough for a dynamic buffer
//     to have been refilled. §4.42's defect survived exactly that for three sections. The
//     at-draw columns in `MaybeVerifyStateForDraw` are the answer;
//   * a verifier that cannot fail proves nothing. `CompareShadowToDevice` is self-tested with
//     `GKPLUS_NO_CULL=1`, which makes the forwarded state differ from the mirror on purpose:
//     it must report a CULLMODE mismatch and nothing else.

#include "D3D8CaptureInternal.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "Core.h"

namespace gk {
namespace d3d8 {

const std::string *TextureAssetName(IDirect3DBaseTexture8 *texture) {
  if (texture == nullptr || LiveTextureWrappers.count(texture) == 0) {
    return nullptr;
  }
  return &static_cast<CaptureTexture *>(texture)->rim_path_;
}

// Walks the cache records and names any texture that has acquired one since the last pass.
// Driven from Present, because a record is populated at an unpredictable point after the
// texture is created and there is no event for it.
void ResolveTextureNames() {
  TheStats.rim_records = RimRecords.size();
  TheStats.rim_records_bound = 0;
  for (AwTexture *const record : RimRecords) {
    if (record == nullptr || record->d3d_texture == nullptr) {
      continue;
    }
    ++TheStats.rim_records_bound;
    if (record->path == nullptr ||
        LiveTextureWrappers.count(record->d3d_texture) == 0) {
      continue;
    }
    auto &texture = *static_cast<CaptureTexture *>(record->d3d_texture);
    if (!texture.rim_path_.empty()) {
      continue;
    }
    texture.rim_path_ = record->path;
    ++TheStats.textures_named;
    // The image may not exist yet; NameTextureImage no-ops then, and EnsureTextureImage
    // picks the name up when it does.
    vulkan::NameTextureImage(texture.image_, texture.rim_path_);
  }
}

// --- the synthetic quad probe (§4.35) ---------------------------------------------------------
//
// One textured quad, pre-transformed to exact screen pixels, drawn last, through this device's
// own methods so that the reference and this renderer get the same thing. Everything that could
// explain a difference in a *scene* is removed rather than controlled for: no lighting, no second
// stage, no alpha test, no blending, no depth, and `SELECTARG1(TEXTURE)` so the vertex colour
// cannot contribute either. What is left is the texture, the coordinates and the filter.
//
// `scale` is how many screen pixels a texel gets: 1.0 draws the texture at its own size, which is
// the case with no minification at all and therefore no LOD to choose. That is the point of the
// sweep - §4.34 ruled out mip *selection*, so the question is whether a 1:1 quad still differs.

void CaptureDevice::DrawProbeQuad() {
  if (ProbeTexture == nullptr || LiveTextureWrappers.count(ProbeTexture) == 0) {
    return;
  }
  const float width = static_cast<float>(ProbeTexture->width_) * ProbeScale;
  const float height = static_cast<float>(ProbeTexture->height_) * ProbeScale;
  // Top-left at a whole pixel plus `ProbeOffset`, which is what moves the sample points between
  // texel corners and texel centres - see the note on ProbeOffset.
  const float x0 = 16.0f + ProbeOffset, y0 = 16.0f + ProbeOffset;
  const float x1 = x0 + width, y1 = y0 + height;
  ProbeRect = {static_cast<LONG>(x0), static_cast<LONG>(y0), static_cast<LONG>(x1),
               static_cast<LONG>(y1)};

  BeginScene();
  SetRenderState(D3DRS_LIGHTING, FALSE);
  SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  SetRenderState(D3DRS_ZENABLE, FALSE);
  SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  SetRenderState(D3DRS_STENCILENABLE, FALSE);
  SetRenderState(D3DRS_FOGENABLE, FALSE);
  SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
  SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
  SetTexture(0, ProbeTexture);
  SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  SetTextureStageState(0, D3DTSS_COLORARG1,
                       ProbeAlpha ? (D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE) : D3DTA_TEXTURE);
  SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
  // The five sampler states are plain #defines in d3d8to9's header rather than enumerators,
  // because D3D9 moved them off the texture stage - hence the cast. They are the same states
  // §4.28 found the game sets only from inside state blocks.
  const auto stage_state = [](int value) {
    return static_cast<D3DTEXTURESTAGESTATETYPE>(value);
  };
  SetTextureStageState(0, stage_state(D3DTSS_MAGFILTER), D3DTEXF_LINEAR);
  SetTextureStageState(0, stage_state(D3DTSS_MINFILTER), D3DTEXF_LINEAR);
  SetTextureStageState(0, stage_state(D3DTSS_MIPFILTER), ProbeMipFilter);
  SetTextureStageState(0, stage_state(D3DTSS_ADDRESSU), D3DTADDRESS_CLAMP);
  SetTextureStageState(0, stage_state(D3DTSS_ADDRESSV), D3DTADDRESS_CLAMP);
  SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

  // D3DFVF_XYZRHW | DIFFUSE | SPECULAR | TEX1 - the layout the game itself uses for its 2D
  // draws, so the converter is on a path it already takes rather than on one only the probe
  // exercises.
  struct ProbeVertex {
    float x, y, z, rhw;
    DWORD diffuse, specular;
    float u, v;
  };
  static_assert(sizeof(ProbeVertex) == 32);
  SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1);
  const ProbeVertex quad[4] = {
      {x0, y0, 0.5f, 1.0f, 0xffffffff, 0, 0.0f, 0.0f},
      {x1, y0, 0.5f, 1.0f, 0xffffffff, 0, 1.0f, 0.0f},
      {x0, y1, 0.5f, 1.0f, 0xffffffff, 0, 0.0f, 1.0f},
      {x1, y1, 0.5f, 1.0f, 0xffffffff, 0, 1.0f, 1.0f},
  };
  DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ProbeVertex));
  EndScene();
}

// See the note on DepthProbeArmed in D3D8CaptureInternal.h. A quad that appears and a quad that
// does not, which is the whole reading.
void CaptureDevice::DrawDepthProbe() {
  if (!DepthProbeArmed) {
    return;
  }
  // The viewport rectangle has to be the one the scene uses, or the quad lands somewhere else -
  // only the depth slice is under test here.
  D3DVIEWPORT8 saved = {};
  if (FAILED(GetViewport(&saved))) {
    return;
  }
  D3DVIEWPORT8 probe = saved;
  probe.MinZ = DepthProbeMinZ;
  probe.MaxZ = DepthProbeMaxZ;

  BeginScene();
  SetViewport(&probe);
  // Only the depth buffer, and **straight at the forwarded runtime**: the shadow's `Clear`
  // records the frame's clear values for the Vulkan path's load ops, and a mid-frame clear
  // recorded there would become the whole frame's depth clear. That also makes this a
  // d3d8/d3d9 instrument by construction - under `vulkan` the quad still draws, but against
  // whatever depth the scene left rather than against a known value, so read it in a
  // reference mode. Which is the point: the question is what D3D does.
  inner_->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, DepthProbeClearZ, 0);
  SetRenderState(D3DRS_LIGHTING, FALSE);
  SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  SetRenderState(D3DRS_STENCILENABLE, FALSE);
  SetRenderState(D3DRS_FOGENABLE, FALSE);
  SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
  SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
  // No write, so the probe cannot change what the test it is measuring compares against.
  SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
  SetTexture(0, nullptr);
  SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

  struct ProbeVertex {
    float x, y, z, rhw;
    DWORD diffuse, specular;
    float u, v;
  };
  static_assert(sizeof(ProbeVertex) == 32);
  SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1);
  // Bottom-left, clear of the texture probe at (16, 16) and of Gunlok's own HUD.
  const float x0 = 16.0f, y0 = 340.0f, x1 = 144.0f, y1 = 436.0f;
  const float z = DepthProbeQuadZ;
  const ProbeVertex quad[4] = {
      {x0, y0, z, 1.0f, 0xffff00ff, 0, 0.0f, 0.0f},
      {x1, y0, z, 1.0f, 0xffff00ff, 0, 1.0f, 0.0f},
      {x0, y1, z, 1.0f, 0xffff00ff, 0, 0.0f, 1.0f},
      {x1, y1, z, 1.0f, 0xffff00ff, 0, 1.0f, 1.0f},
  };
  DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ProbeVertex));
  SetViewport(&saved);
  EndScene();
}

// See the note on ViewportProbeArmed in D3D8CaptureInternal.h. Where the quad appears is the
// whole reading, and both answers put it inside the rectangle so clipping cannot fake either.
void CaptureDevice::DrawViewportProbe() {
  if (!ViewportProbeArmed) {
    return;
  }
  D3DVIEWPORT8 saved = {};
  if (FAILED(GetViewport(&saved))) {
    return;
  }
  D3DVIEWPORT8 probe = saved;
  probe.X = static_cast<DWORD>(ViewportProbeX);
  probe.Y = static_cast<DWORD>(ViewportProbeY);
  probe.Width = ViewportProbeWidth;
  probe.Height = ViewportProbeHeight;
  probe.MinZ = 0.0f;
  probe.MaxZ = 1.0f;

  BeginScene();
  SetViewport(&probe);
  SetRenderState(D3DRS_LIGHTING, FALSE);
  SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  SetRenderState(D3DRS_ZENABLE, FALSE);
  SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  SetRenderState(D3DRS_STENCILENABLE, FALSE);
  SetRenderState(D3DRS_FOGENABLE, FALSE);
  SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
  SetTexture(0, nullptr);
  SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

  struct ProbeVertex {
    float x, y, z, rhw;
    DWORD diffuse, specular;
    float u, v;
  };
  static_assert(sizeof(ProbeVertex) == 32);
  SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1);
  // 20 px in from the rectangle's own origin, so the quad sits well inside it under either
  // answer and a whole rectangle's width apart between them.
  const float x0 = static_cast<float>(ViewportProbeX) + 20.0f;
  const float y0 = static_cast<float>(ViewportProbeY) + 20.0f;
  const float x1 = x0 + 64.0f, y1 = y0 + 32.0f;
  const ProbeVertex quad[4] = {
      {x0, y0, 0.5f, 1.0f, 0xffff00ff, 0, 0.0f, 0.0f},
      {x1, y0, 0.5f, 1.0f, 0xffff00ff, 0, 1.0f, 0.0f},
      {x0, y1, 0.5f, 1.0f, 0xffff00ff, 0, 0.0f, 1.0f},
      {x1, y1, 0.5f, 1.0f, 0xffff00ff, 0, 1.0f, 1.0f},
  };
  DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ProbeVertex));
  SetViewport(&saved);
  EndScene();
}

std::string ArmViewportProbe(bool armed, int32_t x, int32_t y, uint32_t width,
                             uint32_t height) {
  ViewportProbeArmed = armed;
  if (width > 0 && height > 0) {
    ViewportProbeX = x;
    ViewportProbeY = y;
    ViewportProbeWidth = width;
    ViewportProbeHeight = height;
  }
  if (!armed) {
    return "viewport probe off";
  }
  const float qx = static_cast<float>(ViewportProbeX) + 20.0f;
  const float qy = static_cast<float>(ViewportProbeY) + 20.0f;
  char line[320];
  std::snprintf(line, sizeof(line),
                "viewport probe: rect %d,%d %ux%u, magenta XYZRHW quad at (%.0f,%.0f)-(%.0f,%.0f)\n"
                "  X/Y added   -> the quad is at (%.0f,%.0f)-(%.0f,%.0f)\n"
                "  X/Y ignored -> the quad is at (%.0f,%.0f)-(%.0f,%.0f)\n"
                "read it in d3d8, not vulkan: the question is what D3D does",
                ViewportProbeX, ViewportProbeY, ViewportProbeWidth, ViewportProbeHeight, qx, qy,
                qx + 64.0f, qy + 32.0f, qx + ViewportProbeX, qy + ViewportProbeY,
                qx + ViewportProbeX + 64.0f, qy + ViewportProbeY + 32.0f, qx, qy, qx + 64.0f,
                qy + 32.0f);
  return line;
}

std::string ArmDepthProbe(bool armed, double quad_z, double clear_z, double min_z,
                          double max_z) {
  DepthProbeArmed = armed;
  DepthProbeQuadZ = static_cast<float>(quad_z);
  DepthProbeClearZ = static_cast<float>(clear_z);
  DepthProbeMinZ = static_cast<float>(min_z);
  DepthProbeMaxZ = static_cast<float>(max_z);
  if (!armed) {
    return "depth probe off";
  }
  const float scaled = DepthProbeMinZ + (DepthProbeMaxZ - DepthProbeMinZ) * DepthProbeQuadZ;
  char text[512];
  std::snprintf(text, sizeof(text),
                "depth probe armed: magenta quad at 16,340..144,436  z %.4f  viewport "
                "%.4f..%.4f  depth cleared to %.4f, ZFUNC LESS\n"
                "  if the viewport transform APPLIES to a pre-transformed vertex: depth %.4f "
                "-> quad %s\n"
                "  if the vertex z IS the depth value:                             depth %.4f "
                "-> quad %s\n",
                DepthProbeQuadZ, DepthProbeMinZ, DepthProbeMaxZ, DepthProbeClearZ, scaled,
                scaled < DepthProbeClearZ ? "DRAWN" : "absent", DepthProbeQuadZ,
                DepthProbeQuadZ < DepthProbeClearZ ? "DRAWN" : "absent");
  return text;
}

// --- verifying the shadow against the device --------------------------------------------------
//
// The mirror is the whole basis of this renderer - every draw is described by what the shadow
// says was set - and nothing checked it against D3D until §4.39 found a state-block bug that let
// the two diverge silently for a whole scene. This is `verify_textures` and `verify_buffers`
// pointed at state instead of at bytes: read it back off the device and diff.
//
// **Only what the shadow claims to track, and only what the game has actually set.** Walking the
// whole 0..255 render-state space would ask D3D about indices that are not states and report its
// refusals as mismatches, and the states the game never touches are exactly the ones the mirror
// has nothing to be wrong about.
//
// Two honest limits. The device it compares against is `inner_`, which is d3d8to9 under
// `GKPLUS_RENDERER=vulkan` and Windows' own D3D8 under `d3d8` - so what this answers is "does the
// mirror match the device we forward to", which is the mirror's own contract. And the armed
// per-draw form needs a Vulkan draw list to index into, so it is a `vulkan`-mode instrument; the
// immediate form works in any mode.
//
// The comparison and its state live here, in this file's anonymous namespace; the four entry
// points are down with the rest of the public API, past where it closes.

// Deliberately separate from FormatShadowState's own table, which also carries a print order and
// a float flag. Drift between them costs readability here and nothing else: an unknown state
// prints as its number rather than as the wrong name.
const char *RenderStateName(uint32_t state) {
  switch (state) {
  case D3DRS_ZENABLE: return "ZENABLE";
  case D3DRS_ZWRITEENABLE: return "ZWRITEENABLE";
  case D3DRS_ZFUNC: return "ZFUNC";
  case D3DRS_CULLMODE: return "CULLMODE";
  case D3DRS_SRCBLEND: return "SRCBLEND";
  case D3DRS_DESTBLEND: return "DESTBLEND";
  case D3DRS_ALPHABLENDENABLE: return "ALPHABLENDENABLE";
  case D3DRS_ALPHATESTENABLE: return "ALPHATESTENABLE";
  case D3DRS_ALPHAFUNC: return "ALPHAFUNC";
  case D3DRS_ALPHAREF: return "ALPHAREF";
  case D3DRS_COLORWRITEENABLE: return "COLORWRITEENABLE";
  case D3DRS_LIGHTING: return "LIGHTING";
  case D3DRS_AMBIENT: return "AMBIENT";
  case D3DRS_SPECULARENABLE: return "SPECULARENABLE";
  case D3DRS_COLORVERTEX: return "COLORVERTEX";
  case D3DRS_NORMALIZENORMALS: return "NORMALIZENORMALS";
  case D3DRS_LOCALVIEWER: return "LOCALVIEWER";
  case D3DRS_DIFFUSEMATERIALSOURCE: return "DIFFUSEMATERIALSOURCE";
  case D3DRS_SPECULARMATERIALSOURCE: return "SPECULARMATERIALSOURCE";
  case D3DRS_AMBIENTMATERIALSOURCE: return "AMBIENTMATERIALSOURCE";
  case D3DRS_EMISSIVEMATERIALSOURCE: return "EMISSIVEMATERIALSOURCE";
  case D3DRS_TEXTUREFACTOR: return "TEXTUREFACTOR";
  case D3DRS_SHADEMODE: return "SHADEMODE";
  case D3DRS_FILLMODE: return "FILLMODE";
  case D3DRS_STENCILENABLE: return "STENCILENABLE";
  case D3DRS_STENCILFUNC: return "STENCILFUNC";
  case D3DRS_STENCILREF: return "STENCILREF";
  case D3DRS_STENCILMASK: return "STENCILMASK";
  case D3DRS_STENCILWRITEMASK: return "STENCILWRITEMASK";
  case D3DRS_STENCILFAIL: return "STENCILFAIL";
  case D3DRS_STENCILZFAIL: return "STENCILZFAIL";
  case D3DRS_STENCILPASS: return "STENCILPASS";
  case D3DRS_FOGENABLE: return "FOGENABLE";
  case D3DRS_CLIPPING: return "CLIPPING";
  case D3DRS_SOFTWAREVERTEXPROCESSING: return "SOFTWAREVERTEXPROCESSING";
  default: return nullptr;
  }
}

const char *StageStateName(uint32_t type) {
  switch (type) {
  case D3DTSS_COLOROP: return "COLOROP";
  case D3DTSS_COLORARG1: return "COLORARG1";
  case D3DTSS_COLORARG2: return "COLORARG2";
  case D3DTSS_ALPHAOP: return "ALPHAOP";
  case D3DTSS_ALPHAARG1: return "ALPHAARG1";
  case D3DTSS_ALPHAARG2: return "ALPHAARG2";
  case D3DTSS_TEXCOORDINDEX: return "TEXCOORDINDEX";
  case D3DTSS_ADDRESSU: return "ADDRESSU";
  case D3DTSS_ADDRESSV: return "ADDRESSV";
  case D3DTSS_MAGFILTER: return "MAGFILTER";
  case D3DTSS_MINFILTER: return "MINFILTER";
  case D3DTSS_MIPFILTER: return "MIPFILTER";
  case D3DTSS_MIPMAPLODBIAS: return "MIPMAPLODBIAS";
  case D3DTSS_MAXMIPLEVEL: return "MAXMIPLEVEL";
  default: return nullptr;
  }
}

std::string CompareShadowToDevice(CaptureDevice *capture) {
  IDirect3DDevice8 *device = capture->inner_;
  std::string out;
  char line[320];
  auto add = [&](const char *fmt, auto... args) {
    std::snprintf(line, sizeof(line), fmt, args...);
    out += line;
  };
  auto name_or_number = [](const char *name, uint32_t id, char *buffer, size_t size) {
    if (name != nullptr) {
      std::snprintf(buffer, size, "%s", name);
    } else {
      std::snprintf(buffer, size, "state %u", id);
    }
    return buffer;
  };
  uint32_t checked = 0, bad = 0;
  char label[64];

  // The three switches that make the forwarded state differ from the mirror ON PURPOSE. Named up
  // front so a mismatch they caused is attributable rather than alarming.
  if (ForceLightingOff || ForceStage1Off || ForceSpecularOff || ForceNoMipmap || ForceNoCull ||
      ForceNoZTest || ForceNoAlphaTest || ForceNoBlend) {
    out += "NOTE: a GKPLUS_NO_* switch is set, so the device is MEANT to differ from the mirror\n";
  }

  for (const auto &entry : TheStats.render_states) {
    const uint32_t state = entry.first;
    if (state == 0 || state >= kMaxRenderState) {
      continue;
    }
    DWORD value = 0;
    if (FAILED(device->GetRenderState(static_cast<D3DRENDERSTATETYPE>(state), &value))) {
      continue;
    }
    ++checked;
    if (static_cast<uint32_t>(value) != State.render_states[state]) {
      ++bad;
      add("  %-26s mirror 0x%08x  device 0x%08x\n",
          name_or_number(RenderStateName(state), state, label, sizeof(label)),
          State.render_states[state], (unsigned)value);
    }
  }

  for (const auto &entry : TheStats.stage_states) {
    const uint32_t stage = entry.first >> 16, type = entry.first & 0xffff;
    if (stage >= kStages || type == 0 || type >= kMaxStageState) {
      continue;
    }
    DWORD value = 0;
    if (FAILED(device->GetTextureStageState(stage, static_cast<D3DTEXTURESTAGESTATETYPE>(type),
                                            &value))) {
      continue;
    }
    ++checked;
    if (static_cast<uint32_t>(value) != State.stage_states[stage][type]) {
      ++bad;
      add("  stage %u %-20s mirror 0x%08x  device 0x%08x\n", stage,
          name_or_number(StageStateName(type), type, label, sizeof(label)),
          State.stage_states[stage][type], (unsigned)value);
    }
  }

  for (uint32_t stage = 0; stage < kStages; ++stage) {
    IDirect3DBaseTexture8 *bound = nullptr;
    if (FAILED(device->GetTexture(stage, &bound))) {
      continue;
    }
    ++checked;
    // The mirror holds our WRAPPER and the device holds the inner object, so the comparison has
    // to unwrap - a raw pointer compare here would report every textured stage as a mismatch.
    IDirect3DBaseTexture8 *expected = UnwrapTexture(State.textures[stage]);
    if (bound != expected) {
      ++bad;
      add("  stage %u texture         mirror %p  device %p\n", stage, (void *)expected,
          (void *)bound);
    }
    if (bound != nullptr) {
      bound->Release();
    }
  }

  DWORD fvf = 0;
  if (SUCCEEDED(device->GetVertexShader(&fvf))) {
    ++checked;
    if (static_cast<uint32_t>(fvf) != State.fvf) {
      ++bad;
      add("  FVF                        mirror 0x%08x  device 0x%08x\n", State.fvf,
          (unsigned)fvf);
    }
  }

  struct TransformCheck {
    const char *name;
    D3DTRANSFORMSTATETYPE which;
    const D3DMATRIX *mirror;
    bool have;
  };
  const TransformCheck transforms[] = {
      {"world", D3DTS_WORLD, &State.world, State.have_world},
      {"view", D3DTS_VIEW, &State.view, State.have_view},
      {"projection", D3DTS_PROJECTION, &State.projection, State.have_projection}};
  for (const TransformCheck &transform : transforms) {
    if (!transform.have) {
      continue;
    }
    D3DMATRIX actual = {};
    if (FAILED(device->GetTransform(transform.which, &actual))) {
      continue;
    }
    ++checked;
    if (std::memcmp(&actual, transform.mirror, sizeof(D3DMATRIX)) != 0) {
      ++bad;
      add("  transform %-16s differs; mirror row3 %.3f %.3f %.3f  device %.3f %.3f %.3f\n",
          transform.name, transform.mirror->m[3][0], transform.mirror->m[3][1],
          transform.mirror->m[3][2], actual.m[3][0], actual.m[3][1], actual.m[3][2]);
    }
  }

  D3DVIEWPORT8 viewport = {};
  if (SUCCEEDED(device->GetViewport(&viewport))) {
    ++checked;
    if (static_cast<int32_t>(viewport.X) != State.viewport_x ||
        static_cast<int32_t>(viewport.Y) != State.viewport_y ||
        viewport.Width != State.viewport_width || viewport.Height != State.viewport_height ||
        viewport.MinZ != State.viewport_min_z || viewport.MaxZ != State.viewport_max_z) {
      ++bad;
      add("  viewport                   mirror %d,%d %ux%u %.4f..%.4f  "
          "device %u,%u %ux%u %.4f..%.4f\n",
          State.viewport_x, State.viewport_y, State.viewport_width, State.viewport_height,
          State.viewport_min_z, State.viewport_max_z, viewport.X, viewport.Y, viewport.Width,
          viewport.Height, viewport.MinZ, viewport.MaxZ);
    }
  }

  if (State.have_material) {
    D3DMATERIAL8 material = {};
    if (SUCCEEDED(device->GetMaterial(&material))) {
      ++checked;
      if (std::memcmp(&material, &State.material, sizeof(D3DMATERIAL8)) != 0) {
        ++bad;
        add("  material                   differs; mirror diffuse %.2f %.2f %.2f %.2f  device "
            "%.2f %.2f %.2f %.2f\n",
            State.material.Diffuse.r, State.material.Diffuse.g, State.material.Diffuse.b,
            State.material.Diffuse.a, material.Diffuse.r, material.Diffuse.g,
            material.Diffuse.b, material.Diffuse.a);
      }
    }
  }

  for (uint32_t i = 0; i < kLights; ++i) {
    if (!State.light_set[i] && !State.light_enabled[i]) {
      continue;
    }
    BOOL enabled = FALSE;
    if (SUCCEEDED(device->GetLightEnable(i, &enabled))) {
      ++checked;
      if ((enabled != FALSE) != State.light_enabled[i]) {
        ++bad;
        add("  light %u enable             mirror %s  device %s\n", i,
            State.light_enabled[i] ? "on" : "off", enabled ? "on" : "off");
      }
    }
    if (!State.light_set[i]) {
      continue;
    }
    D3DLIGHT8 light = {};
    if (SUCCEEDED(device->GetLight(i, &light))) {
      ++checked;
      if (std::memcmp(&light, &State.lights[i], sizeof(D3DLIGHT8)) != 0) {
        ++bad;
        add("  light %u                    contents differ\n", i);
      }
    }
  }

  // The stream bindings, which are state exactly as much as a render state is and are the only
  // part of a draw the mirror can get wrong *without* getting a single D3DRS_ wrong. A draw whose
  // states all match and whose geometry comes from the wrong buffer looks like a draw in the
  // wrong place - which is precisely the shape left over once everything above matches.
  {
    IDirect3DVertexBuffer8 *stream = nullptr;
    UINT stride = 0;
    if (SUCCEEDED(device->GetStreamSource(0, &stream, &stride))) {
      ++checked;
      IDirect3DVertexBuffer8 *expected = Unwrap(capture->stream0_);
      if (stream != expected || stride != capture->stream0_stride_) {
        ++bad;
        add("  stream 0                   mirror %p stride %u  device %p stride %u\n",
            (void *)expected, capture->stream0_stride_, (void *)stream, (unsigned)stride);
      }
      if (stream != nullptr) {
        stream->Release();
      }
    }
    IDirect3DIndexBuffer8 *indices = nullptr;
    UINT base_vertex = 0;
    if (SUCCEEDED(device->GetIndices(&indices, &base_vertex))) {
      ++checked;
      IDirect3DIndexBuffer8 *expected = Unwrap(capture->indices_);
      if (indices != expected || base_vertex != capture->base_vertex_) {
        ++bad;
        add("  indices                    mirror %p base %u  device %p base %u\n",
            (void *)expected, capture->base_vertex_, (void *)indices, (unsigned)base_vertex);
      }
      if (indices != nullptr) {
        indices->Release();
      }
    }
  }

  char header[128];
  std::snprintf(header, sizeof(header), "%u/%u states match the device\n", checked - bad,
                checked);
  return std::string(header) + out;
}

// The lighting equation's inputs, read off the device at the moment a draw is issued. See the
// note on VerifyDrawLighting in D3D8CaptureInternal.h for why this is not the same question as
// "does the mirror match".
std::string DescribeDeviceLighting(CaptureDevice *capture) {
  IDirect3DDevice8 *device = capture->inner_;
  std::string out;
  char line[320];
  auto add = [&](const char *fmt, auto... args) {
    std::snprintf(line, sizeof(line), fmt, args...);
    out += line;
  };
  auto state_of = [&](D3DRENDERSTATETYPE which) {
    DWORD value = 0;
    return SUCCEEDED(device->GetRenderState(which, &value)) ? static_cast<uint32_t>(value) : 0u;
  };

  // The four *MATERIALSOURCE states decide whether each C* below is the material's colour or a
  // vertex's, and D3D falls back to the material when the FVF does not carry the one named -
  // which for D3DMCS_COLOR2 (the default for specular) it usually does not.
  add("  SPECULARENABLE %u  LOCALVIEWER %u  COLORVERTEX %u  NORMALIZENORMALS %u  LIGHTING %u\n",
      state_of(D3DRS_SPECULARENABLE), state_of(D3DRS_LOCALVIEWER), state_of(D3DRS_COLORVERTEX),
      state_of(D3DRS_NORMALIZENORMALS), state_of(D3DRS_LIGHTING));
  add("  material sources: diffuse %u  specular %u  ambient %u  emissive %u   (0 material, 1 "
      "COLOR1, 2 COLOR2)\n",
      state_of(D3DRS_DIFFUSEMATERIALSOURCE), state_of(D3DRS_SPECULARMATERIALSOURCE),
      state_of(D3DRS_AMBIENTMATERIALSOURCE), state_of(D3DRS_EMISSIVEMATERIALSOURCE));
  add("  FVF 0x%03x   global ambient 0x%08x\n", State.fvf, state_of(D3DRS_AMBIENT));

  D3DMATERIAL8 material = {};
  if (SUCCEEDED(device->GetMaterial(&material))) {
    // **Power is the field to read first.** It is the exponent on N.H, so it is the one input
    // whose effect over a broad surface is a multiplier rather than a tint, and the report in
    // `render.state` shows only whatever the frame's last SetMaterial left behind.
    add("  material: diffuse %.2f %.2f %.2f %.2f  ambient %.2f %.2f %.2f\n", material.Diffuse.r,
        material.Diffuse.g, material.Diffuse.b, material.Diffuse.a, material.Ambient.r,
        material.Ambient.g, material.Ambient.b);
    add("            specular %.2f %.2f %.2f  POWER %.3f  emissive %.2f %.2f %.2f\n",
        material.Specular.r, material.Specular.g, material.Specular.b, material.Power,
        material.Emissive.r, material.Emissive.g, material.Emissive.b);
  }

  D3DMATRIX view = {};
  if (SUCCEEDED(device->GetTransform(D3DTS_VIEW, &view))) {
    float eye[4] = {};
    StoreEye(eye, view);
    add("  eye (world) %.3f %.3f %.3f\n", eye[0], eye[1], eye[2]);
    // The matrix it came from. StoreEye assumes the view is RIGID - rotation plus translation -
    // and inverts it by transposing the 3x3 and carrying the translation back through it. That
    // assumption is worth printing rather than trusting: a scale or a shear in the view makes
    // the transpose not the inverse, and the resulting eye is wrong by an amount too small to
    // see in anything except a grazing specular term.
    for (int row = 0; row < 4; ++row) {
      add("    view[%d] %9.5f %9.5f %9.5f %9.5f\n", row, view.m[row][0], view.m[row][1],
          view.m[row][2], view.m[row][3]);
    }
  }

  uint32_t enabled_count = 0;
  for (uint32_t i = 0; i < kLights; ++i) {
    BOOL enabled = FALSE;
    if (FAILED(device->GetLightEnable(i, &enabled)) || !enabled) {
      continue;
    }
    D3DLIGHT8 light = {};
    if (FAILED(device->GetLight(i, &light))) {
      continue;
    }
    ++enabled_count;
    add("  light %u ON type %u  diffuse %.2f %.2f %.2f  specular %.2f %.2f %.2f\n", i,
        (unsigned)light.Type, light.Diffuse.r, light.Diffuse.g, light.Diffuse.b,
        light.Specular.r, light.Specular.g, light.Specular.b);
    add("          position %.2f %.2f %.2f  range %.2f  atten %.4f %.4f %.4f\n",
        light.Position.x, light.Position.y, light.Position.z, light.Range, light.Attenuation0,
        light.Attenuation1, light.Attenuation2);
  }
  add("  %u lights enabled at this draw\n", enabled_count);
  return out;
}

// The armed per-draw form. Rewritten every frame the index comes round, so the report is always
// the most recent one - which is what makes it usable on a frame that is not paused.

void MaybeVerifyStateForDraw(CaptureDevice *capture, const vulkan::DrawItem &item,
                             uint32_t vertex_bias) {
  if (VerifyDrawIndex < 0 || capture == nullptr) {
    return;
  }
  if (vulkan::PendingDrawIndex() != static_cast<uint32_t>(VerifyDrawIndex)) {
    return;
  }
  VerifyDrawReport = CompareShadowToDevice(capture);
  VerifyDrawLighting = DescribeDeviceLighting(capture);
  VerifyDrawValid = true;
  VerifyDrawGeometry.valid = true;
  VerifyDrawGeometry.item = item;
  VerifyDrawGeometry.vertex_bias = vertex_bias;
  VerifyDrawGeometry.vertices =
      LiveVertexWrappers.count(capture->stream0_) != 0
          ? static_cast<CaptureVertexBuffer *>(capture->stream0_)
          : nullptr;
  VerifyDrawGeometry.indices = LiveIndexWrappers.count(capture->indices_) != 0
                                   ? static_cast<CaptureIndexBuffer *>(capture->indices_)
                                   : nullptr;

  // Read the game's own vertices HERE, while the draw is being issued, rather than at report
  // time. A read-only lock mid-frame may stall the runtime; a diagnostic can afford that, and
  // there is no other moment at which this question can be asked.
  VerifyDrawGeometry.at_draw.clear();
  VerifyDrawGeometry.at_draw_read = false;
  if (CaptureVertexBuffer *source = VerifyDrawGeometry.vertices; source != nullptr) {
    const uint32_t stride = vulkan::FvfStride(source->fvf_);
    BYTE *data = nullptr;
    if (stride != 0 && SUCCEEDED(source->LockForRead(&data)) && data != nullptr) {
      const uint32_t count = source->length_ / stride;
      VerifyDrawGeometry.at_draw.resize(count);
      if (vulkan::ConvertVertices(source->fvf_, data, count,
                                  VerifyDrawGeometry.at_draw.data())) {
        VerifyDrawGeometry.at_draw_read = true;
      } else {
        VerifyDrawGeometry.at_draw.clear();
      }
      source->UnlockAfterRead();
    }

    char book[256];
    std::snprintf(book, sizeof(book),
                  "  buffer %p: %u bytes fvf 0x%03x, slot %s at %u, unlocks %llu, frame %llu, "
                  "drawn in frame %llu (%u draws), scratch version from frame %llu\n",
                  (void *)source, source->length_, source->fvf_,
                  source->slot_.valid ? "valid" : "NONE", source->slot_.offset,
                  (unsigned long long)source->unlocks_, (unsigned long long)TheStats.frames,
                  (unsigned long long)source->drawn_frame_, source->draws_this_frame_,
                  (unsigned long long)source->version_frame_);
    VerifyDrawGeometry.book = book;

    // The arena as it stands right now, at the draw, over the same span the report prints.
    // ReadArena submits and waits, so this stalls the frame being measured - which is what a
    // diagnostic armed on one draw can afford, and the only way to ask this at all.
    VerifyDrawGeometry.arena_at_draw.clear();
    VerifyDrawGeometry.arena_at_draw_read = false;
    if (item.vertex_source == vulkan::DrawSource::Arena) {
      VerifyDrawGeometry.arena_at_draw.resize(kWatchedVertices);
      VerifyDrawGeometry.arena_at_draw_read = vulkan::ReadArena(
          true, uint64_t(item.base_vertex) * sizeof(vulkan::CanonicalVertex),
          kWatchedVertices * uint32_t(sizeof(vulkan::CanonicalVertex)),
          VerifyDrawGeometry.arena_at_draw.data());
      if (!VerifyDrawGeometry.arena_at_draw_read) {
        VerifyDrawGeometry.arena_at_draw.clear();
      }
    }
  }
  // The two mappings a stage goes through, recorded side by side. The verifier above proves the
  // same texture OBJECT is bound as the device holds; whether our bindless index names that
  // object's pixels is a second mapping and a second chance to be wrong, and nothing checked it.
  for (uint32_t stage = 0; stage < 2; ++stage) {
    VerifyDrawGeometry.bound_name[stage].clear();
    VerifyDrawGeometry.image_name[stage].clear();
    VerifyDrawGeometry.image_index[stage] = 0xffffffffu;
    IDirect3DBaseTexture8 *bound = State.textures[stage];
    if (bound == nullptr || LiveTextureWrappers.count(bound) == 0) {
      continue;
    }
    auto &texture = *static_cast<CaptureTexture *>(bound);
    VerifyDrawGeometry.bound_name[stage] =
        texture.rim_path_.empty() ? "<unnamed>" : texture.rim_path_;
    if (texture.image_.valid) {
      VerifyDrawGeometry.image_index[stage] = texture.image_.index;
    }
  }
}

void CaptureDevice::NoteIndexedRange(D3DPRIMITIVETYPE type, UINT min_index, UINT num_vertices,
                                     UINT start_index, UINT primitive_count) {
  if (stream0_ == nullptr || indices_ == nullptr || stream0_stride_ == 0 ||
      LiveVertexWrappers.count(stream0_) == 0 || LiveIndexWrappers.count(indices_) == 0) {
    return;
  }
  const auto &vertex_buffer = *static_cast<CaptureVertexBuffer *>(stream0_);
  const auto &index_buffer = *static_cast<CaptureIndexBuffer *>(indices_);
  const uint64_t vertices_available = vertex_buffer.length_ / stream0_stride_;
  const uint64_t indices_available =
      index_buffer.index_stride_ == 0 ? 0 : index_buffer.length_ / index_buffer.index_stride_;
  const uint64_t vertices_wanted = static_cast<uint64_t>(min_index) + num_vertices;
  const uint64_t indices_wanted =
      static_cast<uint64_t>(start_index) + ElementCount(type, primitive_count);
  const bool vertices_over = vertices_wanted > vertices_available;
  const bool indices_over = indices_wanted > indices_available;
  if (!vertices_over && !indices_over) {
    return;
  }
  ++TheStats.draws_out_of_range;
  // The first few in full, because a count says it happens and only the numbers say what the
  // game asked for - and whether the overrun is one vertex or a whole buffer.
  if (OutOfRangeDraws.size() < 8) {
    char line[256];
    std::snprintf(line, sizeof(line),
                  "    draw %llu: vertices %llu+%u of %llu%s, indices %u+%u of %llu%s",
                  (unsigned long long)TheStats.draws_buffered, (unsigned long long)min_index,
                  num_vertices, (unsigned long long)vertices_available,
                  vertices_over ? "  <== over" : "", start_index,
                  ElementCount(type, primitive_count), (unsigned long long)indices_available,
                  indices_over ? "  <== over" : "");
    OutOfRangeDraws.emplace_back(line);
  }
}

// Recorded whether or not the draw is drawn, so the description is available while the
// topologies are switched off - which is the state they are shipped in.
void NoteOddTopology(D3DPRIMITIVETYPE type, bool user_pointer, uint32_t primitives,
                     const void *vertex_data, uint32_t vertex_stride, uint32_t vertex_count) {
  if (type == D3DPT_TRIANGLELIST) {
    return;
  }
  OddTopology odd;
  odd.type = static_cast<uint32_t>(type);
  odd.fvf = State.fvf;
  odd.user_pointer = user_pointer ? 1u : 0u;
  odd.primitives = primitives;
  odd.stages = ActiveStages();
  odd.texture_index = vulkan::kNoTexture;
  IDirect3DBaseTexture8 *const bound = State.textures[0];
  if (bound != nullptr && LiveTextureWrappers.count(bound) != 0) {
    auto &texture = *static_cast<CaptureTexture *>(bound);
    if (texture.image_.valid) {
      odd.texture_index = texture.image_.index;
    }
  }
  odd.blend = State.render_states[D3DRS_ALPHABLENDENABLE];
  odd.depth_test = State.render_states[D3DRS_ZENABLE];
  odd.stencil = State.render_states[D3DRS_STENCILENABLE];
  odd.stencil_func = State.render_states[D3DRS_STENCILFUNC];
  odd.stencil_ref = State.render_states[D3DRS_STENCILREF];
  if (vertex_data != nullptr && vertex_count > 0 && (State.fvf & 0x004u) != 0) {
    const uint32_t stride =
        vertex_stride != 0 ? vertex_stride : vulkan::FvfStride(State.fvf);
    float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
    for (uint32_t i = 0; i < vertex_count && stride != 0; ++i) {
      const auto *p = static_cast<const uint8_t *>(vertex_data) + size_t(i) * stride;
      float x = 0.0f;
      float y = 0.0f;
      std::memcpy(&x, p, sizeof(x));
      std::memcpy(&y, p + 4, sizeof(y));
      min_x = (std::min)(min_x, x);
      min_y = (std::min)(min_y, y);
      max_x = (std::max)(max_x, x);
      max_y = (std::max)(max_y, y);
    }
    odd.x0 = static_cast<int32_t>(min_x);
    odd.y0 = static_cast<int32_t>(min_y);
    odd.x1 = static_cast<int32_t>(max_x);
    odd.y1 = static_cast<int32_t>(max_y);
    const auto *first = static_cast<const uint8_t *>(vertex_data);
    std::memcpy(&odd.first_z, first + 8, sizeof(odd.first_z));
    std::memcpy(&odd.first_rhw, first + 12, sizeof(odd.first_rhw));
    // The first vertex's D3DCOLOR, which for an untextured draw IS the colour it paints.
    if ((State.fvf & 0x040u) != 0) {
      std::memcpy(&odd.first_colour, first + 16, sizeof(odd.first_colour));
    }
  }
  ++OddTopologies[odd];
}

// One line per draw of the frame just gone, built from the shadow state and the call's own
// arguments - so it exists in `d3d8` and `d3d9` mode, where there is no Vulkan draw list and
// `render.draw_info` has nothing to describe. It is the list `render.ref_range` indexes into,
// and without it that switch can only be aimed by guessing: a draw's index is a position in a
// list the game rebuilds every frame, and the two runs being compared do not have the same
// number of draws (347 against 348 on two launches of one camera).
//
// Kept for the LAST COMPLETE frame rather than the one being built, because the REPL runs at
// Present and would otherwise read a list one draw long.
void LogDraw(D3DPRIMITIVETYPE type, bool user_pointer, uint32_t primitives) {
  // The cap is a memory bound, not a measurement: a frame over it would lose its tail, which
  // `render.frame_draws` says out loud rather than silently truncating.
  if (DrawLog.size() >= 8192) {
    return;
  }
  LoggedDraw entry;
  entry.index = static_cast<uint32_t>(TheStats.draws_this_frame - 1);
  entry.type = static_cast<uint32_t>(type);
  entry.primitives = primitives;
  entry.fvf = State.fvf;
  entry.blend = State.render_states[D3DRS_ALPHABLENDENABLE];
  entry.src_blend = State.render_states[D3DRS_SRCBLEND];
  entry.dest_blend = State.render_states[D3DRS_DESTBLEND];
  entry.z_test = State.render_states[D3DRS_ZENABLE];
  entry.z_write = State.render_states[D3DRS_ZWRITEENABLE];
  entry.cull = State.render_states[D3DRS_CULLMODE];
  entry.alpha_test = State.render_states[D3DRS_ALPHATESTENABLE];
  entry.min_z = State.viewport_min_z;
  entry.max_z = State.viewport_max_z;
  entry.viewport_x = State.viewport_x;
  entry.viewport_y = State.viewport_y;
  entry.viewport_width = State.viewport_width;
  entry.viewport_height = State.viewport_height;
  entry.user_pointer = user_pointer;
  IDirect3DBaseTexture8 *const bound = State.textures[0];
  if (bound != nullptr && LiveTextureWrappers.count(bound) != 0) {
    const auto &texture = *static_cast<CaptureTexture *>(bound);
    entry.texture = texture.rim_path_.empty() ? "<unnamed>" : texture.rim_path_;
  }
  DrawLog.push_back(std::move(entry));
}

// The frame's distinct lights, expanded from one draw's run.
//
// Called from ResolveLightRun on every lit draw - including a cache hit, because the question is
// how many DRAWS each light reaches and the mask cache exists precisely so most draws are hits.
// Up to eight map inserts a draw, which is why it is here rather than on the recorder's path.
void NoteLightRun(const vulkan::GpuLight *lights, uint32_t count, uint64_t frame) {
  for (uint32_t i = 0; i < count; ++i) {
    const vulkan::GpuLight &light = lights[i];
    LightKey key;
    key.type = static_cast<uint32_t>(light.spot[3]);
    std::memcpy(key.position, light.position, sizeof(key.position));
    std::memcpy(key.direction, light.direction, sizeof(key.direction));
    std::memcpy(key.diffuse, light.diffuse, sizeof(key.diffuse));
    std::memcpy(&key.range, &light.attenuation[3], sizeof(key.range));
    std::memcpy(key.attenuation, light.attenuation, sizeof(key.attenuation));
    std::memcpy(&key.theta, &light.spot[0], sizeof(key.theta));
    std::memcpy(&key.phi, &light.spot[1], sizeof(key.phi));
    std::memcpy(&key.falloff, &light.spot[2], sizeof(key.falloff));

    LightCensusEntry &entry = LightCensus[key];
    ++entry.draws;
    LightCensusEntry &session = LightCensusSession[key];
    // The session row counts FRAMES, not draws, and it is the staticness reading: a light that
    // never moves accumulates frames under one key, and a light that does leaves a new key
    // behind every frame with `frames == 1`.
    if (session.last_frame != frame || session.frames == 0) {
      session.last_frame = frame;
      if (session.frames == 0) {
        session.first_frame = frame;
      }
      ++session.frames;
    }
  }
}

void RotateLightCensus(uint64_t frame) {
  if (!LightCensus.empty()) {
    ++LightCensusFramesWithLights;
    if (LightCensus.size() > LightCensusMaxPerFrame) {
      LightCensusMaxPerFrame = LightCensus.size();
    }
  }
  LightCensusLastFrame.swap(LightCensus);
  LightCensus.clear();
  (void)frame;
}

// Did the runtime we forward to accept this draw? A refused call is the one way the reference
// can render fewer pixels than this renderer while agreeing about every state, every vertex and
// every texture - which is precisely the residue §4.40 was left with, and nothing here had ever
// looked at a draw call's return value. `which` is the entry point, so a refusal is attributable
// without a debugger.
void NoteDrawResult(HRESULT hr, const char *which, D3DPRIMITIVETYPE type,
                    uint32_t primitive_count) {
  if (SUCCEEDED(hr)) {
    return;
  }
  ++TheStats.draws_refused;
  if (RefusedDraws.size() < 8) {
    char line[256];
    std::snprintf(line, sizeof(line),
                  "    draw %llu of the frame: %s type %u, %u primitives, hr 0x%08x",
                  (unsigned long long)TheStats.draws_this_frame, which, (unsigned)type,
                  primitive_count, (unsigned)hr);
    RefusedDraws.emplace_back(line);
  }
}

std::string VerifyShadowState() {
  if (TheCaptureDevice == nullptr) {
    return "no device yet\n";
  }
  return CompareShadowToDevice(TheCaptureDevice);
}

void WatchDrawState(int64_t index) {
  VerifyDrawIndex = index;
  VerifyDrawValid = false;
  VerifyDrawReport.clear();
}

int64_t WatchedDrawState() { return VerifyDrawIndex; }

std::string DescribeWatchedDrawState() {
  if (VerifyDrawIndex < 0) {
    return "no draw is being watched; set render.draw_state = <index> and let a frame pass\n";
  }
  if (!VerifyDrawValid) {
    return "draw " + std::to_string(VerifyDrawIndex) +
           " has not been issued since it was watched - let a frame pass, and note this needs "
           "GKPLUS_RENDERER=vulkan, since it indexes the Vulkan draw list\n";
  }
  return "draw " + std::to_string(VerifyDrawIndex) +
         ", device state at the moment it was issued:\n" + VerifyDrawReport +
         "lighting inputs at that draw, read off the device:\n" + VerifyDrawLighting;
}

// What the watched draw actually pulled, both sides: the indices and vertices the shader reads
// out of the arena, and the ones D3D reads out of the game's own buffer for the same draw.
//
// This is the reading nothing else here could give. `verify_buffers` proves a slot holds what its
// buffer holds; `draw_info` prints the offsets a draw was given; neither says the draw addressed
// the right place, and the arena is one buffer that every slot shares, so addressing it wrongly
// yields *other geometry* rather than garbage - which looks like a draw in the wrong position and
// nothing like a bug (§4.16).
std::string DescribeWatchedDrawGeometry() {
  if (VerifyDrawIndex < 0) {
    return "no draw is being watched; set render.draw_state = <index> and let a frame pass\n";
  }
  if (!VerifyDrawGeometry.valid) {
    return "draw " + std::to_string(VerifyDrawIndex) +
           " has not been issued since it was watched - let a frame pass\n";
  }
  const vulkan::DrawItem &item = VerifyDrawGeometry.item;
  std::string out;
  char line[256];
  auto add = [&](const char *fmt, auto... args) {
    std::snprintf(line, sizeof(line), fmt, args...);
    out += line;
  };

  add("draw %lld: %s, %u %s, base_vertex %u first_index %u, D3D bias %u\n",
      (long long)VerifyDrawIndex, item.indexed ? "indexed" : "non-indexed", item.count,
      item.indexed ? "indices" : "vertices", item.base_vertex, item.first_index,
      VerifyDrawGeometry.vertex_bias);

  // The two mappings a stage goes through. `verify_state` proves the bound OBJECT matches the
  // device; this is the second hop - from that object to the bindless image the shader samples -
  // which is a separate mapping and was never checked against anything.
  {
    const std::vector<vulkan::TextureImageInfo> images = vulkan::TextureImages();
    for (uint32_t stage = 0; stage < 2; ++stage) {
      if (VerifyDrawGeometry.bound_name[stage].empty()) {
        continue;
      }
      const uint32_t index = VerifyDrawGeometry.image_index[stage];
      std::string sampled = "<no image>";
      for (const vulkan::TextureImageInfo &info : images) {
        if (info.index == index) {
          sampled = info.name.empty() ? "<unnamed>" : info.name;
          break;
        }
      }
      const bool agree = sampled == VerifyDrawGeometry.bound_name[stage];
      add("  stage %u: game bound \"%s\" -> samples image %d \"%s\"%s\n", stage,
          VerifyDrawGeometry.bound_name[stage].c_str(), (int)index, sampled.c_str(),
          agree ? "" : "   <== THE TWO DISAGREE");
    }
  }
  if (item.vertex_source != vulkan::DrawSource::Arena) {
    out += "  vertices came from the frame scratch - use render.draw_vertices for those\n";
    return out;
  }

  // The indices first, because they decide which vertices to read at all.
  std::vector<uint16_t> indices;
  if (item.indexed) {
    if (item.index_stride != 2 || item.index_source != vulkan::DrawSource::Arena) {
      out += "  indices are not 16-bit arena indices; not read back\n";
      return out;
    }
    indices.resize(item.count);
    if (!vulkan::ReadArena(false, uint64_t(item.first_index) * 2, item.count * 2,
                           indices.data())) {
      out += "  could not read the index arena\n";
      return out;
    }
  } else {
    indices.resize(item.count);
    for (uint32_t i = 0; i < item.count; ++i) {
      indices[i] = static_cast<uint16_t>(i);
    }
  }

  uint32_t lowest = 0xffffu, highest = 0;
  for (uint16_t i : indices) {
    lowest = i < lowest ? i : lowest;
    highest = i > highest ? i : highest;
  }
  add("  indices %u..%u:", lowest, highest);
  for (size_t i = 0; i < indices.size() && i < 36; ++i) {
    add(" %u", indices[i]);
  }
  out += indices.size() > 36 ? " ...\n" : "\n";

  const uint32_t span = highest - lowest + 1;
  std::vector<vulkan::CanonicalVertex> arena(span);
  if (!vulkan::ReadArena(true, uint64_t(item.base_vertex + lowest) *
                                   sizeof(vulkan::CanonicalVertex),
                         span * uint32_t(sizeof(vulkan::CanonicalVertex)), arena.data())) {
    out += "  could not read the vertex arena\n";
    return out;
  }

  // The other side: the game's own buffer, converted the way the uploader would have converted
  // it. If these two disagree the draw is reading somewhere it should not be.
  std::vector<vulkan::CanonicalVertex> expected;
  CaptureVertexBuffer *source = VerifyDrawGeometry.vertices;
  if (source != nullptr) {
    const uint32_t stride = vulkan::FvfStride(source->fvf_);
    BYTE *data = nullptr;
    if (stride != 0 && SUCCEEDED(source->LockForRead(&data)) && data != nullptr) {
      const uint32_t count = source->length_ / stride;
      expected.resize(count);
      if (!vulkan::ConvertVertices(source->fvf_, data, count, expected.data())) {
        expected.clear();
      }
      source->UnlockAfterRead();
    }
  }

  const std::vector<vulkan::CanonicalVertex> &at_draw = VerifyDrawGeometry.at_draw;
  const std::vector<vulkan::CanonicalVertex> &arena_then = VerifyDrawGeometry.arena_at_draw;
  out += VerifyDrawGeometry.book;
  if (!arena_then.empty()) {
    uint32_t moved = 0;
    for (uint32_t i = 0; i < arena_then.size() && i - lowest < 12 && i < arena.size(); ++i) {
      if (std::memcmp(&arena[i], &arena_then[i], sizeof(arena[i])) != 0) {
        ++moved;
      }
    }
    add("  the arena AT THE DRAW differs from the arena read back later in %u of %u vertices - "
        "%s\n",
        moved, (unsigned)arena_then.size(),
        moved == 0 ? "so the late read is trustworthy here"
                   : "so the late read is a DIFFERENT question and the at-draw column is the "
                     "one to believe");
    for (uint32_t i = 0; i < arena_then.size() && i < 4; ++i) {
      add("    arena at draw  %u  %9.3f %9.3f %9.3f %8.5f %08x\n", i, arena_then[i].pos[0],
          arena_then[i].pos[1], arena_then[i].pos[2], arena_then[i].pos[3], arena_then[i].color);
    }
  }
  add("  %-4s %-34s %-34s %s\n", "idx", "arena (what the shader reads)",
      at_draw.empty() ? "" : "D3D buffer AT DRAW TIME",
      expected.empty() ? "" : "D3D buffer, read back later");
  uint32_t mismatches = 0, stale = 0;
  for (uint32_t i = lowest; i <= highest && i - lowest < 12; ++i) {
    const vulkan::CanonicalVertex &got = arena[i - lowest];
    add("  %-4u %9.3f %9.3f %9.3f %8.5f %08x", i, got.pos[0], got.pos[1], got.pos[2],
        got.pos[3], got.color);
    const uint32_t d3d_index = i + VerifyDrawGeometry.vertex_bias;
    if (d3d_index < at_draw.size()) {
      const vulkan::CanonicalVertex &now = at_draw[d3d_index];
      add("  %9.3f %9.3f %9.3f %8.5f %08x%s", now.pos[0], now.pos[1], now.pos[2], now.pos[3],
          now.color, std::memcmp(&got, &now, sizeof(got)) == 0 ? "" : " <== STALE");
      if (std::memcmp(&got, &now, sizeof(got)) != 0) {
        ++stale;
      }
    }
    if (d3d_index < expected.size()) {
      const vulkan::CanonicalVertex &want = expected[d3d_index];
      add("  %9.3f %9.3f %9.3f %8.5f %08x%s", want.pos[0], want.pos[1], want.pos[2],
          want.pos[3], want.color,
          std::memcmp(&got, &want, sizeof(got)) == 0 ? "" : "   <== differs");
      if (std::memcmp(&got, &want, sizeof(got)) != 0) {
        ++mismatches;
      }
    }
    out += "\n";
  }
  const uint32_t shown = span < 12 ? span : 12;
  if (VerifyDrawGeometry.at_draw_read) {
    add("  %u of the %u vertices shown are STALE - the arena disagrees with what D3D held when "
        "the draw was issued\n",
        stale, shown);
  } else {
    out += "  the game's buffer could not be locked at draw time\n";
  }
  if (!expected.empty()) {
    add("  %u of the %u vertices shown differ from the late read-back\n", mismatches, shown);
  } else {
    out += "  the game's buffer could not be read back, so only the arena side is shown\n";
  }
  return out;
}

std::string ArmProbeQuad(const std::string &name, double scale, bool mipmap, double offset,
                         bool alpha) {
  ProbeTexture = nullptr;
  if (name.empty()) {
    return "probe disarmed";
  }
  std::string want = name;
  for (char &c : want) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  for (const void *live : LiveTextureWrappers) {
    auto *const texture =
        static_cast<CaptureTexture *>(const_cast<void *>(live));
    std::string path = texture->rim_path_;
    for (char &c : path) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (path.find(want) != std::string::npos && texture->width_ > 0) {
      ProbeTexture = texture;
      break;
    }
  }
  if (ProbeTexture == nullptr) {
    return "no live texture matches \"" + name + "\"";
  }
  ProbeScale = static_cast<float>(scale);
  ProbeMipFilter = mipmap ? D3DTEXF_LINEAR : D3DTEXF_NONE;
  ProbeOffset = static_cast<float>(offset);
  ProbeAlpha = alpha;
  char line[256];
  std::snprintf(line, sizeof(line),
                "probing %s (%ux%u, %u levels) at scale %.4f -> %.0fx%.0f px at (%.2f,%.2f), "
                "mip filter %s, showing %s",
                ProbeTexture->rim_path_.c_str(), ProbeTexture->width_, ProbeTexture->height_,
                ProbeTexture->levels_, ProbeScale,
                ProbeTexture->width_ * ProbeScale, ProbeTexture->height_ * ProbeScale,
                16.0f + ProbeOffset, 16.0f + ProbeOffset, mipmap ? "LINEAR" : "NONE",
                alpha ? "ALPHA" : "colour");
  return line;
}

std::string FormatFrameDraws(uint32_t first, uint32_t last) {
  std::string out;
  char line[256];
  const auto add = [&](const char *format, auto... args) {
    std::snprintf(line, sizeof(line), format, args...);
    out += line;
  };
  add("%u draws in the last complete frame%s\n", (unsigned)DrawLogLastFrame.size(),
      DrawLogLastFrame.size() >= 8192 ? "  (LOG FULL - the tail is missing)" : "");
  out += "  idx  type prims  fvf   from  blend src dst   z zw cull atest  depth slice   "
         "viewport rect     stage 0 texture\n";
  for (const LoggedDraw &draw : DrawLogLastFrame) {
    if (draw.index < first || draw.index > last) {
      continue;
    }
    char rect[32];
    std::snprintf(rect, sizeof(rect), "%d,%d %ux%u", draw.viewport_x, draw.viewport_y,
                  draw.viewport_width, draw.viewport_height);
    add("  %4u  %4u %5u  0x%03x  %s  %5u %3u %3u  %2u %2u %4u %5u  %.4f..%.4f  %-16s  %s\n",
        draw.index, draw.type, draw.primitives, draw.fvf, draw.user_pointer ? "ptr" : "buf",
        draw.blend, draw.src_blend, draw.dest_blend, draw.z_test, draw.z_write, draw.cull,
        draw.alpha_test, draw.min_z, draw.max_z, rect, draw.texture.c_str());
  }
  return out;
}

// The frame's distinct D3D lights, and whether they are static in world space.
//
// **The three questions phase 5 has to answer before anything is designed**, and none of them had
// an instrument: how many distinct point and spot lights are enabled in a frame, over how many
// draws, and do they move? A per-frame shadow cube per light is affordable at two to six and is
// not at dozens - and if they never move, their occlusion by the map bakes exactly like §4.61's.
//
// The two counts to read together are `distinct this frame` and `distinct over the session`. A
// static rig makes the second converge on the first; a light the game re-authors every frame
// leaves a new key behind each time, so the session total climbs without bound and the per-light
// `frames` column reads 1.
std::string FormatFrameLights() {
  std::string out;
  char line[256];
  const auto add = [&](const char *format, auto... args) {
    std::snprintf(line, sizeof(line), format, args...);
    out += line;
  };
  const auto type_name = [](uint32_t type) {
    switch (type) {
    case D3DLIGHT_POINT: return "point";
    case D3DLIGHT_SPOT: return "spot";
    case D3DLIGHT_DIRECTIONAL: return "dir";
    default: return "?";
    }
  };
  const auto as_float = [](uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  };

  uint32_t local_this_frame = 0;
  for (const auto &[key, entry] : LightCensusLastFrame) {
    local_this_frame += key.type != D3DLIGHT_DIRECTIONAL ? 1u : 0u;
  }
  uint32_t local_session = 0;
  uint64_t local_session_frames = 0;
  for (const auto &[key, entry] : LightCensusSession) {
    if (key.type != D3DLIGHT_DIRECTIONAL) {
      ++local_session;
      local_session_frames += entry.frames;
    }
  }
  add("%u distinct lights in the last complete frame (%u point/spot), %llu at the frame peak\n",
      (unsigned)LightCensusLastFrame.size(), local_this_frame,
      (unsigned long long)LightCensusMaxPerFrame);
  add("%u distinct over the session (%u point/spot) across %llu frames with any light\n",
      (unsigned)LightCensusSession.size(), local_session,
      (unsigned long long)LightCensusFramesWithLights);
  // The one line that answers "are they static". A static rig has every light in nearly every
  // frame it could be in, so the mean is close to the frame count; a moving one is close to 1.
  if (local_session != 0) {
    add("mean frames a distinct point/spot light survives: %.1f  (1.0 = re-authored every frame, "
        "%llu = never moves)\n",
        double(local_session_frames) / double(local_session),
        (unsigned long long)LightCensusFramesWithLights);
  }
  out += "  type   draws  frames    position                  range   diffuse           "
         "attenuation\n";
  for (const auto &[key, entry] : LightCensusLastFrame) {
    const auto found = LightCensusSession.find(key);
    const uint64_t frames = found != LightCensusSession.end() ? found->second.frames : 0;
    char position[48];
    std::snprintf(position, sizeof(position), "%.2f %.2f %.2f", as_float(key.position[0]),
                  as_float(key.position[1]), as_float(key.position[2]));
    char diffuse[32];
    std::snprintf(diffuse, sizeof(diffuse), "%.2f %.2f %.2f", as_float(key.diffuse[0]),
                  as_float(key.diffuse[1]), as_float(key.diffuse[2]));
    add("  %-5s  %5llu  %6llu    %-24s  %6.2f  %-16s  %.3f %.4f %.5f\n", type_name(key.type),
        (unsigned long long)entry.draws, (unsigned long long)frames, position,
        key.type == D3DLIGHT_DIRECTIONAL ? 0.0f : as_float(key.range), diffuse,
        as_float(key.attenuation[0]), as_float(key.attenuation[1]), as_float(key.attenuation[2]));
  }
  return out;
}

void SetRefRange(uint32_t first, uint32_t last) {
  RefRangeFirst = first;
  RefRangeLast = last;
}

void GetRefRange(uint32_t &first, uint32_t &last) {
  first = RefRangeFirst;
  last = RefRangeLast;
}

void SetRefHide(uint32_t first, uint32_t last) {
  RefHideFirst = first;
  RefHideLast = last;
}

void GetRefHide(uint32_t &first, uint32_t &last) {
  first = RefHideFirst;
  last = RefHideLast;
}

// Which dword of a cache record holds the texture wrapper, measured rather than assumed.
//
// `AwTexture+0x00` is what rendering_notes.md derives from AwMaterial_ApplyStage, and taking
// that on trust named 5 textures out of 53 while 58 of 59 records had *something* stored. This
// scans every offset in the record against the set of live wrappers and reports the histogram,
// which says where the join really is - and whether there is more than one.
std::string RimJoinHistogram() {
  std::map<uint32_t, uint32_t> hits;
  for (AwTexture *const record : RimRecords) {
    if (record == nullptr) {
      continue;
    }
    const auto *words = reinterpret_cast<void *const *>(record);
    for (uint32_t offset = 0; offset < sizeof(AwTexture); offset += 4) {
      if (LiveTextureWrappers.count(words[offset / 4]) != 0) {
        ++hits[offset];
      }
    }
  }
  std::string out = "rim record offsets holding a live texture wrapper:";
  char line[64];
  for (const auto &[offset, count] : hits) {
    std::snprintf(line, sizeof(line), " +0x%02x=%u", offset, count);
    out += line;
  }
  return hits.empty() ? out + " none" : out;
}

std::string VerifyTextureImages() {
  // Or every blit still queued reads back as a stale texture, and the check reports its own
  // impatience as a mismatch.
  vulkan::FlushUploads();
  uint32_t checked = 0;
  uint32_t matched = 0;
  std::string first_mismatch;
  for (const void *pointer : LiveTextureWrappers) {
    auto *const texture =
        static_cast<CaptureTexture *>(const_cast<void *>(pointer));
    if (!texture->image_.valid) {
      continue;
    }
    for (uint32_t level = 0; level < texture->levels_; ++level) {
      D3DLOCKED_RECT locked = {};
      if (!ReadTextureLevel(*texture, level, locked)) {
        continue;
      }
      ++checked;
      uint64_t differing = 0;
      uint64_t first = 0;
      uint64_t total = 0;
      if (vulkan::VerifyImageLevel(texture->image_, level, locked.pBits,
                                   static_cast<uint32_t>(locked.Pitch), &differing, &first,
                                   &total)) {
        ++matched;
      } else if (first_mismatch.empty()) {
        // A one-bit experiment that separates the two things a mismatch can mean. Re-upload
        // this very level from this very data and check again: if it now matches, the upload
        // path is correct and something wrote the texture without this layer seeing it. If it
        // still does not, the conversion or the copy itself is wrong.
        vulkan::UploadIntoTextureImage(texture->image_, level, 0, 0,
                                       (std::max)(1u, texture->width_ >> level),
                                       (std::max)(1u, texture->height_ >> level),
                                       locked.pBits,
                                       static_cast<uint32_t>(locked.Pitch));
        vulkan::FlushUploads();
        const bool fixed_by_reupload = vulkan::VerifyImageLevel(
            texture->image_, level, locked.pBits, static_cast<uint32_t>(locked.Pitch));
        char line[352];
        std::snprintf(line, sizeof(line),
                      "   first mismatch: image %u level %u, %ux%u format %u, pool %u, "
                      "%llu/%llu bytes differ from offset %llu; "
                      "game locks %llu, blits in %llu (levels 0x%x); usage 0x%x, levels %u; "
                      "re-upload fixes it: %s",
                      texture->image_.index, level, texture->width_, texture->height_,
                      texture->format_, texture->pool_, (unsigned long long)differing,
                      (unsigned long long)total, (unsigned long long)first,
                      (unsigned long long)texture->own_locks_,
                      (unsigned long long)texture->blits_in_, texture->levels_blitted_,
                      texture->usage_, texture->levels_,
                      fixed_by_reupload ? "yes" : "no");
        first_mismatch = line;
      }
      texture->inner_->UnlockRect(level);
    }
  }
  char out[128];
  std::snprintf(out, sizeof(out), "%u/%u levels match", matched, checked);
  return std::string(out) + (first_mismatch.empty() ? "" : "\n" + first_mismatch);
}

// The buffer half of VerifyTextureImages: every live buffer that owns an arena slot is read
// back off the GPU and compared against its own current contents.
//
// It is worth the code for the same reason the texture check was. A draw addresses the arenas
// by offset, so a slot holding the wrong bytes does not fail anywhere a counter can see - the
// vertices are somewhere, the indices are somewhere, and the picture is merely wrong. A bad
// index is the loudest form of it: one triangle reaching a vertex from an unrelated mesh
// megabytes away, which draws as a wedge across half the screen.
std::string VerifyBufferSlots() {
  vulkan::FlushUploads();
  uint32_t checked = 0;
  uint32_t matched = 0;
  std::string first_mismatch;

  uint32_t reported = 0;
  auto report = [&](const char *what, uint32_t index, uint32_t bytes, uint64_t differing,
                    uint64_t first, uint32_t offset, uint32_t fvf, uint64_t unlocks) {
    if (++reported > 8) {
      return;
    }
    char line[256];
    std::snprintf(line, sizeof(line),
                  "\n   %s %u, %u bytes at arena offset %u, fvf 0x%03x, "
                  "%llu unlocks; %llu bytes differ from %llu",
                  what, index, bytes, offset, fvf, (unsigned long long)unlocks,
                  (unsigned long long)differing, (unsigned long long)first);
    first_mismatch += line;
  };

  // Do the live slots overlap each other? An arena that hands the same bytes to two buffers
  // corrupts both, and it would look exactly like a lost upload from the outside.
  std::vector<std::pair<uint64_t, uint64_t>> spans[2];
  auto note_span = [&](const vulkan::BufferSlot &slot) {
    if (slot.valid) {
      spans[slot.vertex ? 0 : 1].push_back({slot.offset, uint64_t(slot.offset) + slot.bytes});
    }
  };
  for (const void *p : LiveVertexWrappers) {
    note_span(static_cast<const CaptureVertexBuffer *>(p)->slot_);
  }
  for (const void *p : LiveIndexWrappers) {
    note_span(static_cast<const CaptureIndexBuffer *>(p)->slot_);
  }
  uint32_t overlaps = 0;
  for (auto &list : spans) {
    std::sort(list.begin(), list.end());
    for (size_t i = 1; i < list.size(); ++i) {
      if (list[i].first < list[i - 1].second) {
        ++overlaps;
      }
    }
  }

  uint32_t which = 0;
  for (const void *pointer : LiveVertexWrappers) {
    auto &buffer = *static_cast<CaptureVertexBuffer *>(const_cast<void *>(pointer));
    ++which;
    const uint32_t stride = vulkan::FvfStride(buffer.fvf_);
    if (!buffer.slot_.valid || stride == 0) {
      continue;
    }
    const uint32_t count = buffer.length_ / stride;
    BYTE *data = nullptr;
    if (count == 0 || FAILED(buffer.LockForRead(&data)) || data == nullptr) {
      continue;
    }
    std::vector<vulkan::CanonicalVertex> expected(count);
    if (vulkan::ConvertVertices(buffer.fvf_, data, count, expected.data())) {
      ++checked;
      uint64_t differing = 0;
      uint64_t first = 0;
      uint8_t got[32] = {};
      const uint32_t bytes = count * static_cast<uint32_t>(sizeof(vulkan::CanonicalVertex));
      if (vulkan::VerifySlot(buffer.slot_, expected.data(), bytes, &differing, &first, got)) {
        ++matched;
      } else {
        report("vertex buffer", which, bytes, differing, first, buffer.slot_.offset,
               buffer.fvf_, buffer.unlocks_);
        {
          const auto *want = reinterpret_cast<const float *>(
              reinterpret_cast<const uint8_t *>(expected.data()) + first);
          const auto *have = reinterpret_cast<const float *>(got);
          char line[192];
          std::snprintf(line, sizeof(line),
                        "\n      want %.3f %.3f %.3f %.3f   arena holds %.3f %.3f %.3f %.3f",
                        want[0], want[1], want[2], want[3], have[0], have[1], have[2],
                        have[3]);
          first_mismatch += line;
        }
        // A buffer the game has refilled since a draw read its slot is EXPECTED to differ: the
        // slot is deliberately frozen for the rest of that frame and the later versions live in
        // the scratch (§4.23, §4.42), so this compares a frozen slot against a moved-on buffer
        // and is asking a question the design has already answered. Say so rather than let it
        // read as an upload defect - and do not re-upload, which would overwrite the very
        // version this frame's draws are pointing at.
        if (buffer.version_frame_ == TheStats.frames) {
          first_mismatch += "   (EXPECTED: the slot is frozen for this frame and the newer "
                            "version is in the scratch)";
        } else if (reported == 1) {
          // The same one-bit experiment VerifyTextureImages uses: send this very data again and
          // look once more. Fixed by a re-upload means the conversion and the copy are right
          // and something overwrote the slot afterwards; still wrong means the path itself is.
          vulkan::UploadIntoSlot(buffer.slot_, 0, expected.data(), bytes);
          vulkan::FlushUploads();
          first_mismatch +=
              vulkan::VerifySlot(buffer.slot_, expected.data(), bytes)
                  ? "   (a re-upload fixes it: something overwrote the slot)"
                  : "   (a re-upload does NOT fix it: the upload path itself is wrong)";
        }
      }
    }
    buffer.UnlockAfterRead();
  }

  which = 0;
  for (const void *pointer : LiveIndexWrappers) {
    auto &buffer = *static_cast<CaptureIndexBuffer *>(const_cast<void *>(pointer));
    ++which;
    if (!buffer.slot_.valid || buffer.length_ == 0) {
      continue;
    }
    BYTE *data = nullptr;
    if (FAILED(buffer.LockForRead(&data)) || data == nullptr) {
      continue;
    }
    ++checked;
    uint64_t differing = 0;
    uint64_t first = 0;
    if (vulkan::VerifySlot(buffer.slot_, data, buffer.length_, &differing, &first)) {
      ++matched;
    } else {
      report("index buffer", which, buffer.length_, differing, first, buffer.slot_.offset, 0,
             buffer.unlocks_);
    }
    buffer.UnlockAfterRead();
  }

  char out[96];
  std::snprintf(out, sizeof(out), "%u/%u buffers match, %u overlapping live slots (must be 0)",
                matched, checked, overlaps);
  return std::string(out) + first_mismatch;
}

std::string FormatStats() {
  const CaptureStats &s = TheStats;
  std::string out;
  char line[256];

  auto add = [&](const char *fmt, auto... args) {
    std::snprintf(line, sizeof(line), fmt, args...);
    out += line;
  };

  add("device created: %s\n", HaveDevice ? "yes" : "no");
  add("frames: %llu   draws: %llu buffered + %llu user-pointer   peak/frame: %llu\n",
      (unsigned long long)s.frames, (unsigned long long)s.draws_buffered,
      (unsigned long long)s.draws_user_ptr,
      (unsigned long long)s.max_draws_per_frame);
  add("resources: %llu textures, %llu VB (%llu bytes), %llu IB (%llu bytes)\n",
      (unsigned long long)s.textures_created,
      (unsigned long long)s.vertex_buffers_created, (unsigned long long)s.vertex_bytes,
      (unsigned long long)s.index_buffers_created, (unsigned long long)s.index_bytes);
  add("max texture stage: %u   max light index: %u\n", s.max_stage_used,
      s.max_light_index);
  add("state blocks: %llu recorded (%llu states, max %llu), %llu opaque, %llu applies\n",
      (unsigned long long)s.blocks_recorded, (unsigned long long)s.block_states_total,
      (unsigned long long)s.max_block_states, (unsigned long long)s.blocks_opaque,
      (unsigned long long)s.block_applies);
  add("materials: %llu distinct (peak %llu/frame)   pipelines: %llu   active stages: %llu\n",
      (unsigned long long)s.distinct_materials,
      (unsigned long long)s.max_materials_per_frame,
      (unsigned long long)s.distinct_pipelines,
      (unsigned long long)s.max_active_stages);
  add("applies of an unwitnessed block: %llu (must be 0, or the shadow state is wrong)\n",
      (unsigned long long)s.opaque_block_applies);
  add("LIVE buffers: %llu VB (%llu KB) + %llu IB (%llu KB)   peak %llu buffers, "
      "%llu KB vtx + %llu KB idx\n",
      (unsigned long long)s.live_vertex_buffers,
      (unsigned long long)(s.live_vertex_bytes >> 10),
      (unsigned long long)s.live_index_buffers,
      (unsigned long long)(s.live_index_bytes >> 10),
      (unsigned long long)s.peak_live_buffers,
      (unsigned long long)(s.peak_live_vertex_bytes >> 10),
      (unsigned long long)(s.peak_live_index_bytes >> 10));
  add("locks: %llu   peak locked bytes per frame: %llu KB\n", (unsigned long long)s.locks,
      (unsigned long long)(s.max_locked_bytes_per_frame >> 10));
  add("textures live: %llu   LockRect: %llu   GetSurfaceLevel: %llu\n",
      (unsigned long long)s.live_textures, (unsigned long long)s.texture_lock_rects,
      (unsigned long long)s.texture_surface_levels);
  add("surfaces live: %llu   LockRect: %llu (%llu on a texture level)\n",
      (unsigned long long)s.live_surfaces, (unsigned long long)s.surface_lock_rects,
      (unsigned long long)s.surface_texture_lock_rects);
  add("pixel routes past texture LockRect: %llu surface locks + %llu render targets"
      "   (both must be 0)\n",
      (unsigned long long)s.surface_texture_lock_rects,
      (unsigned long long)s.texture_render_targets);
  add("CopyRects into a texture: %llu   untracked source: %llu (must be 0)   sub-rect: %llu"
      "   source read failures: %llu (must be 0)\n",
      (unsigned long long)s.surface_copy_rects,
      (unsigned long long)s.copy_rects_untracked,
      (unsigned long long)s.copy_rects_partial,
      (unsigned long long)s.texture_read_failures);
  out += RimJoinHistogram() + "\n";
  add("images seeded: %llu (%llu named from the rim cache)   UpdateTexture: %llu   "
      "resource GetDevice: %llu\n",
      (unsigned long long)s.images_seeded, (unsigned long long)s.textures_named,
      (unsigned long long)s.texture_updates,
      (unsigned long long)s.resource_get_devices);
  add("foreign buffers: %llu   unconvertible FVFs: %llu   failed uploads: %llu"
      "   (all must be 0)\n",
      (unsigned long long)s.foreign_buffers,
      (unsigned long long)s.unconvertible_buffers,
      (unsigned long long)s.failed_uploads);
  add("buffers rewritten after being drawn this frame: %llu (%llu vertex, %llu index;"
      " %llu draws affected)\n",
      (unsigned long long)s.buffer_rewritten_after_draw,
      (unsigned long long)s.vertex_buffer_rewritten_after_draw,
      (unsigned long long)s.index_buffer_rewritten_after_draw,
      (unsigned long long)s.draws_reading_rewritten_buffers);
  add("  ... overlapping the drawn-from range: %llu   versioned into the scratch: %llu"
      "   NOT versioned: %llu (must be 0)\n",
      (unsigned long long)s.overlapping_rewrites_after_draw,
      (unsigned long long)s.buffer_versions_in_scratch,
      (unsigned long long)s.unversioned_rewrites);
  for (const auto &entry : RewriteLocks) {
    add("  lock flags 0x%04x %-10s %llu\n", (unsigned)(entry.first >> 1),
        (entry.first & 1) ? "overlapping" : "disjoint", (unsigned long long)entry.second);
  }

  out += "FVF / vertex shader handles:\n";
  for (const auto &[fvf, count] : s.fvf_counts) {
    add("  0x%08x  %llu\n", fvf, (unsigned long long)count);
  }

  out += "primitive types:\n";
  for (const auto &[type, count] : s.primitive_type_counts) {
    add("  %u  %llu\n", type, (unsigned long long)count);
  }

  out += "texture formats:\n";
  for (const auto &[format, count] : s.texture_formats) {
    add("  %u  %llu\n", format, (unsigned long long)count);
  }

  out += "texture pools (0 default, 1 managed, 2 systemmem, 3 scratch):\n";
  for (const auto &[pool, count] : s.texture_pools) {
    add("  %u  %llu\n", pool, (unsigned long long)count);
  }

  add("render states used: %u   (state: distinct values)\n",
      (unsigned)s.render_states.size());
  for (const auto &[state, values] : s.render_states) {
    add("  %3u: %u\n", state, (unsigned)values.size());
  }

  add("texture stage states used: %u   (stage.type: distinct values)\n",
      (unsigned)s.stage_states.size());
  for (const auto &[key, values] : s.stage_states) {
    add("  %u.%-3u: %u\n", key >> 16, key & 0xffff, (unsigned)values.size());
  }

  out += "transform states:";
  for (const uint32_t state : s.transform_states) {
    add(" %u", state);
  }
  out += "\n";
  return out;
}

std::string FormatShadowState() {
  std::string out;
  char line[256];
  auto add = [&](const char *fmt, auto... args) {
    std::snprintf(line, sizeof(line), fmt, args...);
    out += line;
  };

  // A float render state is a float's bits in a DWORD, so it has to be bit-cast rather than
  // converted - reading D3DRS_FOGSTART as an integer gives 1065353216 for 1.0.
  auto as_float = [](uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  };

  // Every state printed below, with the name it is printed under and whether it is a float.
  // The history is taken from the same table, so the two halves cannot drift apart.
  struct Entry {
    uint32_t state;
    const char *name;
    bool is_float;
  };
  static const Entry kEntries[] = {
      {D3DRS_FOGENABLE, "FOGENABLE", false},
      {D3DRS_FOGCOLOR, "FOGCOLOR", false},
      {D3DRS_FOGTABLEMODE, "FOGTABLEMODE", false},
      {D3DRS_FOGVERTEXMODE, "FOGVERTEXMODE", false},
      {D3DRS_FOGSTART, "FOGSTART", true},
      {D3DRS_FOGEND, "FOGEND", true},
      {D3DRS_FOGDENSITY, "FOGDENSITY", true},
      {D3DRS_RANGEFOGENABLE, "RANGEFOGENABLE", false},
      {D3DRS_LIGHTING, "LIGHTING", false},
      {D3DRS_AMBIENT, "AMBIENT", false},
      {D3DRS_SPECULARENABLE, "SPECULARENABLE", false},
      {D3DRS_COLORVERTEX, "COLORVERTEX", false},
      {D3DRS_NORMALIZENORMALS, "NORMALIZENORMALS", false},
      {D3DRS_LOCALVIEWER, "LOCALVIEWER", false},
      {D3DRS_DIFFUSEMATERIALSOURCE, "DIFFUSEMATERIALSOURCE", false},
      {D3DRS_SPECULARMATERIALSOURCE, "SPECULARMATERIALSOURCE", false},
      {D3DRS_AMBIENTMATERIALSOURCE, "AMBIENTMATERIALSOURCE", false},
      {D3DRS_EMISSIVEMATERIALSOURCE, "EMISSIVEMATERIALSOURCE", false},
      // The two colours a draw can carry that the canonical vertex and the shader do not: the
      // per-vertex specular, added after texturing whenever SPECULARENABLE is on, and the
      // texture factor, which a stage names as D3DTA_TFACTOR.
      {D3DRS_TEXTUREFACTOR, "TEXTUREFACTOR", false},
      {D3DRS_COLORWRITEENABLE, "COLORWRITEENABLE", false},
      // The pipeline half - what a VkPipeline would have to be bucketed by.
      {D3DRS_ALPHATESTENABLE, "ALPHATESTENABLE", false},
      {D3DRS_ALPHAREF, "ALPHAREF", false},
      {D3DRS_ALPHAFUNC, "ALPHAFUNC", false},
      {D3DRS_ALPHABLENDENABLE, "ALPHABLENDENABLE", false},
      {D3DRS_SRCBLEND, "SRCBLEND", false},
      {D3DRS_DESTBLEND, "DESTBLEND", false},
      {D3DRS_ZENABLE, "ZENABLE", false},
      {D3DRS_ZWRITEENABLE, "ZWRITEENABLE", false},
      {D3DRS_ZFUNC, "ZFUNC", false},
      {D3DRS_CULLMODE, "CULLMODE", false},
      // Stencil. This renderer has no stencil buffer at all, so if the game uses one, every
      // draw it masks is drawn here unmasked - which is a whole-screen effect for anything
      // shaped like a stencil shadow.
      {D3DRS_STENCILENABLE, "STENCILENABLE", false},
      {D3DRS_STENCILFAIL, "STENCILFAIL", false},
      {D3DRS_STENCILZFAIL, "STENCILZFAIL", false},
      {D3DRS_STENCILPASS, "STENCILPASS", false},
      {D3DRS_STENCILFUNC, "STENCILFUNC", false},
      {D3DRS_STENCILREF, "STENCILREF", false},
      {D3DRS_STENCILMASK, "STENCILMASK", false},
      {D3DRS_STENCILWRITEMASK, "STENCILWRITEMASK", false},
      // The states D3D9 does not have, which is to say the ones d3d8to9 has to invent a
      // translation for - and therefore the ones on which the A/B reference can disagree with
      // both this renderer and the real D3D8 (§4.29). ZVISIBLE, LINEPATTERN and PATCHSEGMENTS it
      // drops on the floor; EDGEANTIALIAS becomes ANTIALIASEDLINEENABLE; SOFTWAREVERTEXPROCESSING
      // becomes SetSoftwareVertexProcessing, but only on a mixed-mode device; and ZBIAS becomes
      // DEPTHBIAS scaled by -0.000005. Printed because "does the game set this at all" is the
      // first question about every one of them.
      {D3DRS_ZBIAS, "ZBIAS", false},
      {D3DRS_SOFTWAREVERTEXPROCESSING, "SOFTWAREVERTEXPROC", false},
      {D3DRS_EDGEANTIALIAS, "EDGEANTIALIAS", false},
      {D3DRS_ZVISIBLE, "ZVISIBLE", false},
      {D3DRS_LINEPATTERN, "LINEPATTERN", false},
      {D3DRS_CLIPPING, "CLIPPING", false},
      {D3DRS_SHADEMODE, "SHADEMODE", false},
      {D3DRS_FILLMODE, "FILLMODE", false},
  };

  out += "render states the renderer has to reproduce (now / every value ever set):\n";
  for (const Entry &entry : kEntries) {
    const uint32_t current = State.render_states[entry.state];
    if (entry.is_float) {
      add("  %-22s %10.3f  |", entry.name, as_float(current));
    } else {
      add("  %-22s 0x%08x  |", entry.name, current);
    }
    const auto found = TheStats.render_states.find(entry.state);
    if (found == TheStats.render_states.end()) {
      out += " never set";
    } else {
      for (const uint32_t value : found->second) {
        if (entry.is_float) {
          add(" %.3f", as_float(value));
        } else {
          add(" 0x%x", value);
        }
      }
    }
    out += "\n";
  }

  // Both halves of the live viewport. The depth slice rides on the DrawItem since §4.32 and the
  // rectangle since §4.47, so neither carries a "the renderer ignores this" marker any more -
  // what is worth seeing is the values, and the per-rectangle marker below.
  add("viewport: %d,%d %ux%u  depth range %.4f..%.4f   distinct ranges ever set: %u\n",
      State.viewport_x, State.viewport_y, State.viewport_width, State.viewport_height,
      State.viewport_min_z, State.viewport_max_z, (unsigned)ViewportDepthRanges.size());
  for (const uint64_t range : ViewportDepthRanges) {
    float min_z = 0.0f, max_z = 0.0f;
    const uint32_t min_bits = uint32_t(range >> 32), max_bits = uint32_t(range);
    std::memcpy(&min_z, &min_bits, sizeof(min_z));
    std::memcpy(&max_z, &max_bits, sizeof(max_z));
    add("    %.4f .. %.4f\n", min_z, max_z);
  }
  // The rectangle, separately from the depth slice, because they answer different questions.
  // Both are per draw now (§4.47): the render target is sized from the backbuffer, and a draw's
  // own rectangle becomes the Vulkan viewport and scissor. A rectangle that is not the whole
  // backbuffer is still worth marking - the offscreen target's SIZE assumes the backbuffer
  // (§4.37/§4.38), so the marker says which draws are the ones to check a scaling defect against
  // rather than announcing something unimplemented.
  {
    uint32_t bb_width = 0, bb_height = 0;
    const bool known = BackBufferExtent(bb_width, bb_height);
    add("backbuffer: %ux%u%s   distinct viewport rects ever set: %u\n", bb_width, bb_height,
        known ? "" : " (unset - windowed D3D takes the client area)",
        (unsigned)ViewportRects.size());
    // The depth buffer the game asked D3D for. It belongs next to the size because it is the
    // other property of the target this renderer has to match rather than choose: the depth
    // test compares quantised values, so a layer authored a hair in front of a wall can pass
    // in 16 bits and fail in 32 (§4.45).
    const char *depth_name = "?";
    switch (AutoDepthStencilFormat) {
    case 80: depth_name = "D3DFMT_D16"; break;
    case 79: depth_name = "D3DFMT_D24X4S4"; break;
    case 77: depth_name = "D3DFMT_D24X8"; break;
    case 75: depth_name = "D3DFMT_D24S8"; break;
    case 73: depth_name = "D3DFMT_D15S1"; break;
    case 71: depth_name = "D3DFMT_D32"; break;
    case 0:  depth_name = "none"; break;
    default: break;
    }
    add("depth buffer the game asked for: %s (%u)%s\n", depth_name, AutoDepthStencilFormat,
        AutoDepthStencilEnabled ? "" : "   <== EnableAutoDepthStencil is FALSE");
    // What the Vulkan path has to clear its own attachments to. Printed because getting it wrong
    // is invisible where the world covers the frame and looks like a translucency defect where it
    // does not - a blended draw over the background blends against this.
    add("clear: colour 0x%08x  z %.3f  stencil %u   %llu calls, %llu with rects%s\n",
        ClearState.colour, ClearState.z, ClearState.stencil,
        (unsigned long long)ClearState.clears,
        (unsigned long long)ClearState.partial_clears,
        ClearState.clears_target ? "" : "   <== the game has never cleared the target");
    for (const uint64_t rect : ViewportRects) {
      const uint32_t x = uint32_t(rect >> 48) & 0xffff, y = uint32_t(rect >> 32) & 0xffff;
      const uint32_t w = uint32_t(rect >> 16) & 0xffff, h = uint32_t(rect) & 0xffff;
      add("    %u,%u %ux%u%s\n", x, y, w, h,
          (x != 0 || y != 0 || (known && (w != bb_width || h != bb_height)))
              ? "   <== a sub-rectangle; honoured per draw since §4.47"
              : "");
    }
  }
  add("draws with fog on: %llu   with lighting on: %llu   of %llu\n",
      (unsigned long long)TheStats.draws_fogged, (unsigned long long)TheStats.draws_lit,
      (unsigned long long)(TheStats.draws_buffered + TheStats.draws_user_ptr));
  add("SetLight: %llu   LightEnable: %llu   SetMaterial: %llu\n",
      (unsigned long long)LightSets, (unsigned long long)LightEnables,
      (unsigned long long)MaterialSets);

  for (uint32_t i = 0; i < kLights; ++i) {
    if (!State.light_set[i] && !State.light_enabled[i]) {
      continue;
    }
    const D3DLIGHT8 &light = State.lights[i];
    add("  light %u: %s type %u  diffuse %.2f %.2f %.2f  ambient %.2f %.2f %.2f  "
        "specular %.2f %.2f %.2f\n",
        i, State.light_enabled[i] ? "ON " : "off", (unsigned)light.Type, light.Diffuse.r,
        light.Diffuse.g, light.Diffuse.b, light.Ambient.r, light.Ambient.g, light.Ambient.b,
        light.Specular.r, light.Specular.g, light.Specular.b);
    add("           direction %.3f %.3f %.3f  position %.1f %.1f %.1f  range %.1f  "
        "atten %.4f %.4f %.4f  theta %.2f phi %.2f falloff %.2f\n",
        light.Direction.x, light.Direction.y, light.Direction.z, light.Position.x,
        light.Position.y, light.Position.z, light.Range, light.Attenuation0,
        light.Attenuation1, light.Attenuation2, light.Theta, light.Phi, light.Falloff);
  }

  // The texture stages as the last draw of the frame left them. `ActiveStages` stops at the
  // first disabled COLOROP, so a stage printed past that one is configured but not in use.
  add("texture stages (%u active), FVF 0x%03x:\n", ActiveStages(), State.fvf);
  for (uint32_t i = 0; i < 3; ++i) {
    const uint32_t *s = State.stage_states[i];
    add("  %u: colorop %2u(%2u,%2u)  alphaop %2u(%2u,%2u)  texcoord %u  texture %p\n", i,
        s[D3DTSS_COLOROP], s[D3DTSS_COLORARG1], s[D3DTSS_COLORARG2], s[D3DTSS_ALPHAOP],
        s[D3DTSS_ALPHAARG1], s[D3DTSS_ALPHAARG2], s[D3DTSS_TEXCOORDINDEX],
        (void *)State.textures[i]);
  }

  // The sampler states, per stage, as they stand and as the game has ever set them **by a
  // direct SetTextureStageState call**. Gunlok configures its samplers inside state blocks, and
  // ApplyOp writes the shadow state without going through the recorder - so all seven read
  // "never set" here while the live values are whatever the blocks put there. The live column
  // is the one to read; "never set" only says a value was never set the direct way, which is
  // why the stage-configuration histogram below carries the filters per draw as well (§4.28).
  {
    static const std::pair<uint32_t, const char *> kSamplerStates[] = {
        {D3DTSS_MAGFILTER, "MAGFILTER"}, {D3DTSS_MINFILTER, "MINFILTER"},
        {D3DTSS_MIPFILTER, "MIPFILTER"}, {D3DTSS_ADDRESSU, "ADDRESSU"},
        {D3DTSS_ADDRESSV, "ADDRESSV"},   {D3DTSS_MIPMAPLODBIAS, "MIPMAPLODBIAS"},
        {D3DTSS_MAXMIPLEVEL, "MAXMIPLEVEL"}};
    out += "sampler stage states (now / ever set by a direct call - blocks bypass the "
           "recorder):\n";
    for (const auto &[state, name] : kSamplerStates) {
      add("  %-14s", name);
      for (uint32_t stage = 0; stage < 2; ++stage) {
        add(" s%u=%u", stage, State.stage_states[stage][state]);
      }
      out += "  |";
      bool any = false;
      for (uint32_t stage = 0; stage < kStages; ++stage) {
        const auto found = TheStats.stage_states.find((stage << 16) | state);
        if (found == TheStats.stage_states.end()) {
          continue;
        }
        any = true;
        add(" s%u:", stage);
        for (const uint32_t value : found->second) {
          add(" %u", value);
        }
      }
      out += any ? "\n" : " never set\n";
    }
  }

  // Every configuration actually drawn with, most used first. Ops are D3DTEXTUREOP (1
  // DISABLE, 2 SELECTARG1, 3 SELECTARG2, 4 MODULATE, 5 MODULATE2X, 7 ADD, ...); args are
  // D3DTA (0 DIFFUSE, 1 CURRENT, 2 TEXTURE, 3 TFACTOR, 4 SPECULAR, |0x10 COMPLEMENT,
  // |0x20 ALPHAREPLICATE).
  {
    std::vector<std::pair<uint64_t, const StageConfig *>> ordered;
    ordered.reserve(StageConfigs.size());
    for (const auto &[config, count] : StageConfigs) {
      ordered.emplace_back(count, &config);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });
    add("stage configurations drawn with: %u\n", (unsigned)ordered.size());
    for (const auto &[count, config] : ordered) {
      add("  %10llu draws  fvf 0x%03x  %u stage%s", (unsigned long long)count, config->fvf,
          config->stages, config->stages == 1 ? " " : "s");
      for (uint32_t i = 0; i < config->stages && i < 2; ++i) {
        const uint32_t *s = config->stage[i];
        // filter is mag/min/mip as D3DTEXTUREFILTERTYPE (0 NONE, 1 POINT, 2 LINEAR) and addr is
        // u/v as D3DTEXTUREADDRESS (1 WRAP, 2 MIRROR, 3 CLAMP). mip 0 means no mipmapping at
        // all, which is the D3D8 default and not a filter this renderer gets to choose.
        add("  | %u: c %2u(%2u,%2u) a %2u(%2u,%2u) uv%u %s filt %u%u%u addr %u%u", i, s[0], s[1],
            s[2], s[3], s[4], s[5], s[6], config->textured[i] != 0 ? "tex" : "---", s[7], s[8],
            s[9], s[10], s[11]);
      }
      out += "\n";
    }
  }

  // Blend factors are D3DBLEND (1 ZERO, 2 ONE, 5 SRCALPHA, 6 INVSRCALPHA, ...); the compare
  // functions are D3DCMPFUNC (1 NEVER, 2 LESS, ... 8 ALWAYS); cull is D3DCULL (1 NONE, 2 CW,
  // 3 CCW).
  {
    std::vector<std::pair<uint64_t, const PipelineConfig *>> ordered;
    ordered.reserve(PipelineConfigs.size());
    for (const auto &[config, count] : PipelineConfigs) {
      ordered.emplace_back(count, &config);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });
    add("pipeline configurations drawn with: %u\n", (unsigned)ordered.size());
    out += "                          fvf atest ref func  blend src dst   z zwrite cull cwrite"
           "   sten func pass zfail shade\n";
    for (const auto &[count, config] : ordered) {
      add("  %10llu draws  0x%03x %5u %3u %4u %6u %3u %3u %3u %6u %4u %5u %6u %4u %4u %5u %5u\n",
          (unsigned long long)count, config->fvf, config->state[0], config->state[1],
          config->state[2], config->state[3], config->state[4], config->state[5],
          config->state[6], config->state[7], config->state[8], config->state[9],
          config->state[10], config->state[11], config->state[12], config->state[13],
          config->state[14]);
    }
  }

  add("bound textures that did not reach the shader: stage 0 %llu foreign + %llu imageless, "
      "stage 1 %llu + %llu\n",
      (unsigned long long)UnresolvedForeign[0], (unsigned long long)UnresolvedNoImage[0],
      (unsigned long long)UnresolvedForeign[1], (unsigned long long)UnresolvedNoImage[1]);
  for (const auto &[key, count] : UnresolvedFormats) {
    add("  pool %u format %u: %llu draws\n", (unsigned)(key >> 32), (unsigned)(key & 0xffffffff),
        (unsigned long long)count);
  }

  {
    std::vector<std::pair<uint64_t, const LightingInputs *>> ordered;
    ordered.reserve(LightingByFvf.size());
    for (const auto &[inputs, count] : LightingByFvf) {
      ordered.emplace_back(count, &inputs);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });
    add("draws with NO vertex diffuse - what the fixed function colours them from: %u\n",
        (unsigned)ordered.size());
    out += "                          fvf  lit  lights  material diffuse   ambient  emissive\n";
    for (const auto &[count, in] : ordered) {
      add("  %10llu draws  0x%03x %4u %6u   0x%08x 0x%08x 0x%08x\n", (unsigned long long)count,
          in->fvf, in->lighting, in->enabled_lights, in->diffuse, in->ambient, in->emissive);
    }
  }

  // D3DPRIMITIVETYPE: 1 point list, 2 line list, 3 line strip, 4 triangle list, 5 strip, 6 fan.
  add("draws that are not triangle lists: %u distinct\n", (unsigned)OddTopologies.size());
  out += "                         type   fvf  from  prims  stages  texture  blend  ztest"
         "   screen box            colour        z      rhw  stencil(func,ref)\n";
  for (const auto &[odd, count] : OddTopologies) {
    float z = 0.0f;
    float rhw = 0.0f;
    std::memcpy(&z, &odd.first_z, sizeof(z));
    std::memcpy(&rhw, &odd.first_rhw, sizeof(rhw));
    add("  %10llu draws %5u 0x%03x %5s %6u %7u %8d %6u %6u   %5d,%-4d %5d,%-4d  0x%08x %8.3f "
        "%8.3f   %u(%u,%u)\n",
        (unsigned long long)count, odd.type, odd.fvf, odd.user_pointer != 0 ? "ptr" : "buf",
        odd.primitives, odd.stages,
        odd.texture_index == vulkan::kNoTexture ? -1 : (int)odd.texture_index, odd.blend,
        odd.depth_test, odd.x0, odd.y0, odd.x1, odd.y1, odd.first_colour, z, rhw, odd.stencil,
        odd.stencil_func, odd.stencil_ref);
  }

  add("first converted vertex colour of a user-pointer draw, by FVF: %u distinct\n",
      (unsigned)FirstVertexColours.size());
  for (const auto &[key, count] : FirstVertexColours) {
    add("  fvf 0x%03x  0x%08x: %llu draws\n", (unsigned)(key >> 32),
        (unsigned)(key & 0xffffffff), (unsigned long long)count);
  }

  add("indexed draws reaching past their bound buffer: %llu  (D3D8 tolerates, D3D9 rejects the "
      "call - the reference renderer's defect, not ours)\n",
      (unsigned long long)TheStats.draws_out_of_range);
  for (const std::string &line : OutOfRangeDraws) {
    out += line;
    out += "\n";
  }

  // A refused draw is the reference rendering fewer pixels than this renderer for a reason that
  // is not a state, a vertex or a texture - so it is the one explanation for "drawn here,
  // missing there" that survives every instrument agreeing.
  add("draw calls the forwarded runtime REFUSED: %llu\n",
      (unsigned long long)TheStats.draws_refused);
  for (const std::string &line : RefusedDraws) {
    out += line;
    out += "\n";
  }

  add("vertex buffers drawn from with no arena slot: %u distinct\n",
      (unsigned)UnslottedVertexBuffers.size());
  for (const auto &[key, count] : UnslottedVertexBuffers) {
    add("  fvf 0x%03x, %u bytes: %llu draws\n", (unsigned)(key >> 32),
        (unsigned)(key & 0xffffffff), (unsigned long long)count);
  }

  if (State.have_material) {
    const D3DMATERIAL8 &m = State.material;
    add("material: diffuse %.2f %.2f %.2f %.2f  ambient %.2f %.2f %.2f\n", m.Diffuse.r,
        m.Diffuse.g, m.Diffuse.b, m.Diffuse.a, m.Ambient.r, m.Ambient.g, m.Ambient.b);
    add("          specular %.2f %.2f %.2f power %.1f  emissive %.2f %.2f %.2f\n",
        m.Specular.r, m.Specular.g, m.Specular.b, m.Power, m.Emissive.r, m.Emissive.g,
        m.Emissive.b);
  } else {
    out += "material: never set\n";
  }
  return out;
}
} // namespace d3d8
} // namespace gk
