#pragma once

#include <cstdint>

namespace gk::vulkan {
enum class TessSet : std::uint32_t;
enum class BloomBlend : std::uint32_t;
} // namespace gk::vulkan

namespace gk::render_settings {

// The renderer's knobs as persistent settings, in **one table** that runs in both
// directions - which is the whole point of the file existing.
//
// Before this, `src/RenderMenu.cpp` persisted the eleven knobs its front-end page
// happened to expose and nothing else, so clicking HDR on the Advanced Graphics
// page survived a restart while `render.hdr = true` from a script did not. Two
// writers to one knob with two different persistence rules, and no way to tell
// from the JS side which you had.
//
// Now there is one list of what a *setting* is, and:
//
//   * `ApplyStored` walks it document -> knobs, from FileHookSystem's first
//     intercepted open - inside WinMain and therefore ahead of the device, so a
//     stored value is in place before the renderer initialises rather than a
//     frame or a menu later;
//   * `SyncToSettings` walks it knobs -> document once a frame, writing only
//     what differs. That catches a write from *any* source - a script, the menu
//     page, the REPL - instead of needing every one of ~70 setters to remember
//     to persist. `settings::SaveSettled` then debounces the disk write, so a
//     script sweeping a knob every frame still costs one file write.
//
// **What is deliberately not here is the measurement surface.** `render.debug.*`
// - the probes, censuses, capture controls, the A/B knobs whose default is
// already the correct value, and every read-only report - persists nothing. A
// stored `draw_hide` would hide a draw on the next launch with nothing on screen
// to say why. That split is the same one `src/JsRender.cpp` draws between its two
// property tables, and this file is the authority for which side a knob is on.
//
// An **environment variable still outranks the file**, and the check lives here
// rather than in each setter for the reason it always did: doing it in one place
// keeps the rule from depending on which setter happens to latch its own
// env-read flag.

// Document -> knobs. Idempotent; safe to call before the device exists.
void ApplyStored();

// Knobs -> document, for anything that differs. Cheap enough to call every frame
// (~70 comparisons, no allocation unless something changed).
void SyncToSettings();

// --- the two enumerated knobs, spelled once ---------------------------------
//
// Shared with `src/JsRender.cpp` rather than duplicated there, because a name is
// what gets written to `settings.json`: if the two sides disagreed about the
// spelling of an operator, a stored value would silently fail to restore. Both
// reverse lookups **refuse an unknown name** rather than approximating one - the
// rule `SetTonemapValue` already followed, since a typo behaving as `clamp`
// reads as "HDR does nothing on this machine".

const char *TonemapName(std::uint32_t op);
/// The tonemap operator \p name stands for, written to \p out.
/// \return false, leaving \p out untouched, for a name that is not one of the
///         operators, and never an approximation.
bool TonemapFromName(const char *name, std::uint32_t *out);

// A bloom layer's blend mode, spelled once for the same reason: it is written to
// `settings.json` and handed back by `render.bloom_layers`.
const char *BloomBlendName(vulkan::BloomBlend blend);
/// The blend mode \p name stands for, written to \p out. False, and \p out
/// untouched, for an unknown name.
bool BloomBlendFromName(const char *name, vulkan::BloomBlend *out);

/// The name a tessellation set is stored under.
const char *TessSetName(vulkan::TessSet set);
/// The tessellation set \p name stands for, written to \p out. False, and
/// \p out untouched, for an unknown name.
bool TessSetFromName(const char *name, vulkan::TessSet *out);

} // namespace gk::render_settings
