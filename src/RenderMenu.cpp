#include "RenderMenu.h"

#include "CustomMenu.h"
#include "D3D8Capture.h"
#include "Menu.h"
#include "Settings.h"
#include "VkContext.h"
#include "VkDraw.h"
#include "VkLighting.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gk {
namespace {

// The dead menu we build the page in, and the title it borrows. There is no
// string in the table for a name of our own, and every menu label the game draws
// is a GL_RESOURCE_ID, so the honest choices are an existing localized title or
// menu 19's own "Preferences" - which describes this page less well than menu
// 24's "Graphic Detail Menu" does, at the cost of two menus sharing a title.
constexpr MenuIndex PageMenu = MenuIndex::Preferences;
constexpr unsigned PageTitle = 0x046a; // GL_MENUTITLE_GRAPHIC

// Every knob on this page belongs to the Vulkan renderer. Under d3d8/d3d9 they
// are inert, so the page and its entry point are simply not built - a row that
// visibly did nothing would be worse than an absent one.
bool VulkanIsDrawing() {
  return std::strcmp(d3d8::RendererName(), "vulkan") == 0;
}

bool AvailableUnderVulkan(void *) { return VulkanIsDrawing(); }

// The PN-triangle pass needs an optional device feature. Without it the
// tessellated pipelines are never built, so the setting could be turned on and
// would read back off forever; drop the row instead.
bool AvailableWithTessellation(void *) {
  return VulkanIsDrawing() && vulkan::Caps().tessellation_shader;
}

// Whether a `GKPLUS_*` companion is set, which is what makes the stored value
// stand aside. See Settings.h: an environment override is the instrument you
// reach for when the setting you need to change is the one keeping the game from
// starting, so it cannot lose to a file.
bool EnvOverridden(const char *name) {
  if (!name) {
    return false;
  }
  return ::GetEnvironmentVariableA(name, nullptr, 0) != 0;
}

// --- The boolean rows -------------------------------------------------------

struct BoolRow {
  const char *label;
  const char *key; // under `core.render.`, named as the `render.*` knob is
  const char *env; // the launch-time override that outranks the file, if any
  bool (*get)();
  void (*set)(bool);
  CustomMenuAvailable available;
};

// Non-const and at namespace scope: `&Rows[i]` is handed to the game side as the
// item's `user` and is read on every click and every frame the page is up.
BoolRow Rows[] = {
    {"Tessellation", "core.render.tessellation", nullptr,
     vulkan::TessellationEnabled, vulkan::SetTessellationEnabled,
     AvailableWithTessellation},
    {"Dynamic Shadows", "core.render.dynamic_shadows", nullptr,
     vulkan::DynamicShadows, vulkan::SetDynamicShadows, AvailableUnderVulkan},
    {"Sun Shadows", "core.render.sun_shadows", nullptr, vulkan::SunShadows,
     vulkan::SetSunShadows, AvailableUnderVulkan},
    {"Map Shadows", "core.render.map_shadows", nullptr, vulkan::MapShadows,
     vulkan::SetMapShadows, AvailableUnderVulkan},
    {"Ambient Occlusion", "core.render.ao", nullptr, vulkan::AmbientOcclusion,
     vulkan::SetAmbientOcclusion, AvailableUnderVulkan},
    {"Per-Pixel Lighting", "core.render.per_pixel_lighting",
     "GKPLUS_VK_PER_PIXEL_LIGHTING", vulkan::PerPixelLighting,
     vulkan::SetPerPixelLighting, AvailableUnderVulkan},
    {"Lighting Maps", "core.render.lighting_maps", nullptr, vulkan::LightingMaps,
     vulkan::SetLightingMaps, AvailableUnderVulkan},
    // The float target and the tonemap pass. `Hdr()` reads back as REQUESTED rather
    // than as effective, which is what makes it safe to bind a toggle to directly -
    // see GetHdr in src/JsRender.cpp for why that differs from the antialiasing row
    // below, which has to read `MsaaWanted` to get the same property.
    {"HDR", "core.render.hdr", "GKPLUS_VK_HDR", vulkan::Hdr, vulkan::SetHdr,
     AvailableUnderVulkan},
};

constexpr const char *MsaaKey = "core.render.msaa";
constexpr const char *MsaaEnv = "GKPLUS_VK_MSAA";

// --- Tone mapping -----------------------------------------------------------
//
// A cycle row for the same reason antialiasing is one: the operators are named,
// and the string table has no "Rolloff". Only meaningful with HDR on, but the row
// is left visible either way - a setting that vanishes when its companion is off
// is harder to find than one that reads as inert.
constexpr const char *TonemapKey = "core.render.tonemap";

const char *TonemapLabels[] = {"Clamp", "Rolloff", "Reinhard", "ACES"};
constexpr uint32_t TonemapCount = 4;

void BoolClicked(CustomMenuItem *item, void *user) {
  const BoolRow *row = static_cast<const BoolRow *>(user);
  // The dispatch has already flipped `value`; that flip is the request. Whether
  // it took is not asserted here - the next refresh reads the knob back, so a
  // setting that declined to change shows what it actually is.
  row->set(item->value != 0);
  // Stored as requested rather than as read back, for the same reason: a value
  // the device refused this run may be the one wanted on a machine that can.
  settings::SetBool(row->key, item->value != 0);
  settings::Save();
}

void BoolRefresh(CustomMenuItem *item, void *user) {
  item->value = static_cast<const BoolRow *>(user)->get() ? 1 : 0;
}

// --- Antialiasing -----------------------------------------------------------
//
// Not a toggle and not a MultiValue item: type 3 resolves its labels through
// GetResourceString, and the string table has no "2x". A LabelWithValue row is
// the only one whose right-hand text is a plain char *, so it is the only way to
// put a sample count on a Gunlok menu at all.

// VkSampleCountFlagBits is the count itself, so the mask can be tested with the
// number. 1 is always present (VkContext.h), which is what makes the wrap safe.
bool SampleCountSupported(uint32_t n) {
  return (vulkan::Caps().sample_counts & n) != 0;
}

void MsaaRefresh(CustomMenuItem *item, void *) {
  // The *wanted* count, not the effective one. `Msaa()` reads back what the
  // frame is drawing at and a write is only adopted by `ReconcileRenderTarget`
  // at the top of the next frame, so a row bound to it would show the old value
  // for one frame after every click. Cycling only over counts the device offers
  // is what keeps the two from ever disagreeing beyond that frame.
  const uint32_t samples = vulkan::MsaaWanted();
  if (samples <= 1) {
    SetCustomMenuValueText(item, "Off");
    return;
  }
  char text[16];
  std::snprintf(text, sizeof(text), "%ux", samples);
  SetCustomMenuValueText(item, text);
}

void MsaaClicked(CustomMenuItem *item, void *user) {
  const uint32_t current = vulkan::MsaaWanted();
  uint32_t next = 1; // nothing higher is offered: wrap to off
  for (uint32_t n = current * 2; n <= 8; n *= 2) {
    if (SampleCountSupported(n)) {
      next = n;
      break;
    }
  }
  vulkan::SetMsaa(next);
  settings::SetNumber(MsaaKey, next);
  settings::Save();
  MsaaRefresh(item, user);
}

void TonemapRefresh(CustomMenuItem *item, void *) {
  const uint32_t op = vulkan::Tonemap();
  SetCustomMenuValueText(item, TonemapLabels[op < TonemapCount ? op : 0]);
}

void TonemapClicked(CustomMenuItem *item, void *user) {
  const uint32_t next = (vulkan::Tonemap() + 1) % TonemapCount;
  vulkan::SetTonemap(next);
  settings::SetNumber(TonemapKey, next);
  settings::Save();
  TonemapRefresh(item, user);
}

void OpenPage(CustomMenuItem *, void *) { GoToMenu(PageMenu, true); }

} // namespace

void ApplyStoredRenderSettings() {
  static bool applied = false;
  if (applied) {
    return;
  }
  applied = true;

  for (const BoolRow &row : Rows) {
    if (EnvOverridden(row.env) || !settings::Has(row.key)) {
      continue;
    }
    row.set(settings::GetBool(row.key, false));
  }

  if (settings::Has(TonemapKey)) {
    const double op = settings::GetNumber(TonemapKey, 1.0);
    // No env companion: `GKPLUS_VK_HDR` already covers the case this rule exists for
    // (a setting that keeps the game from starting), and the operator cannot.
    if (op >= 0.0 && op < static_cast<double>(TonemapCount)) {
      vulkan::SetTonemap(static_cast<uint32_t>(op));
    }
  }

  if (!EnvOverridden(MsaaEnv) && settings::Has(MsaaKey)) {
    const double samples = settings::GetNumber(MsaaKey, 1.0);
    // SetMsaa clamps to what the device offers, so a file naming a count this
    // machine cannot do asks for the nearest one rather than being an error.
    if (samples >= 1.0 && samples <= 64.0) {
      vulkan::SetMsaa(static_cast<uint32_t>(samples));
    }
  }
}

void RegisterRenderMenu() {
  ClaimCustomMenuPage(PageMenu, PageTitle);

  CustomMenuItem *entry =
      AddCustomMenuItem(MenuIndex::Options, "Advanced Graphics", OpenPage,
                        nullptr, CustomMenuOwner::Native);
  SetCustomMenuAvailable(entry, AvailableUnderVulkan);

  CustomMenuItem *msaa = AddCustomMenuValue(PageMenu, "Antialiasing",
                                            MsaaClicked, nullptr,
                                            CustomMenuOwner::Native);
  SetCustomMenuRefresh(msaa, MsaaRefresh);
  SetCustomMenuAvailable(msaa, AvailableUnderVulkan);

  CustomMenuItem *tonemap = AddCustomMenuValue(PageMenu, "Tone Mapping",
                                               TonemapClicked, nullptr,
                                               CustomMenuOwner::Native);
  SetCustomMenuRefresh(tonemap, TonemapRefresh);
  SetCustomMenuAvailable(tonemap, AvailableUnderVulkan);

  for (BoolRow &row : Rows) {
    // The initial state is a placeholder, not a reading: registration runs from
    // DllMain, long before there is a renderer to ask. BoolRefresh replaces it
    // on the reconcile that precedes the page's first draw.
    CustomMenuItem *item = AddCustomMenuToggle(
        PageMenu, row.label, false, BoolClicked, &row, CustomMenuOwner::Native);
    SetCustomMenuRefresh(item, BoolRefresh);
    SetCustomMenuAvailable(item, row.available);
  }
}

} // namespace gk
