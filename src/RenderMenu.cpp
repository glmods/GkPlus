#include "RenderMenu.h"

#include "CustomMenu.h"
#include "D3D8Capture.h"
#include "Menu.h"
#include "VkContext.h"
#include "VkDraw.h"
#include "VkLighting.h"

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

// --- The boolean rows -------------------------------------------------------

struct BoolKnob {
  bool (*get)();
  void (*set)(bool);
};

void BoolClicked(CustomMenuItem *item, void *user) {
  // The dispatch has already flipped `value`; that flip is the request. Whether
  // it took is not asserted here - the next refresh reads the knob back, so a
  // setting that declined to change shows what it actually is.
  static_cast<const BoolKnob *>(user)->set(item->value != 0);
}

void BoolRefresh(CustomMenuItem *item, void *user) {
  item->value = static_cast<const BoolKnob *>(user)->get() ? 1 : 0;
}

struct BoolRow {
  const char *label;
  BoolKnob knob;
  CustomMenuAvailable available;
};

// Non-const and at namespace scope: `&Rows[i].knob` is handed to the game side as
// the item's `user` and is read on every click and every frame the page is up.
BoolRow Rows[] = {
    {"Tessellation",
     {vulkan::TessellationEnabled, vulkan::SetTessellationEnabled},
     AvailableWithTessellation},
    {"Dynamic Shadows",
     {vulkan::DynamicShadows, vulkan::SetDynamicShadows},
     AvailableUnderVulkan},
    {"Sun Shadows",
     {vulkan::SunShadows, vulkan::SetSunShadows},
     AvailableUnderVulkan},
    {"Map Shadows",
     {vulkan::MapShadows, vulkan::SetMapShadows},
     AvailableUnderVulkan},
    {"Ambient Occlusion",
     {vulkan::AmbientOcclusion, vulkan::SetAmbientOcclusion},
     AvailableUnderVulkan},
    {"Per-Pixel Lighting",
     {vulkan::PerPixelLighting, vulkan::SetPerPixelLighting},
     AvailableUnderVulkan},
    {"Lighting Maps",
     {vulkan::LightingMaps, vulkan::SetLightingMaps},
     AvailableUnderVulkan},
};

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
  MsaaRefresh(item, user);
}

void OpenPage(CustomMenuItem *, void *) { GoToMenu(PageMenu, true); }

} // namespace

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

  for (BoolRow &row : Rows) {
    // The initial state is a placeholder, not a reading: registration runs from
    // DllMain, long before there is a renderer to ask. BoolRefresh replaces it
    // on the reconcile that precedes the page's first draw.
    CustomMenuItem *item = AddCustomMenuToggle(
        PageMenu, row.label, false, BoolClicked, &row.knob,
        CustomMenuOwner::Native);
    SetCustomMenuRefresh(item, BoolRefresh);
    SetCustomMenuAvailable(item, row.available);
  }
}

} // namespace gk
