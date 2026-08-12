#pragma once

namespace gk {

// Adds "Advanced Graphics" to the front-end Options menu and builds the page it
// opens: the renderer knobs a player would want, as ordinary Gunlok menu rows.
//
// **Registration only** - it installs no detour and touches no game memory, so it
// is safe to call from DllMain and needs no *System of its own. Everything it
// registers is applied lazily by `ReconcileCustomMenu`, which `CustomMenuSystem`
// already drives from its hook on UpdateAndDrawMenuScreen; a second detour on
// that function would silently disable one of the two (see CLAUDE.md).
//
// The page lives in menu 19 (`Preferences`), which is dead in stock Gunlok - see
// ClaimCustomMenuPage in CustomMenu.h for why that is the only slot this can use.
//
// The whole page, and the Options item that reaches it, are absent unless the
// Vulkan renderer resolved - under `GKPLUS_RENDERER=d3d8`/`d3d9` none of these
// knobs does anything.
//
// Every click is written through to `core.render.*` in `src/Settings`, file and
// all, so the page has no apply step and nothing to lose on a crash.
void RegisterRenderMenu();

// Puts `core.render.*` back on the knobs. Idempotent, and called once from
// `FileHookSystem`'s first intercepted open - the anchor `src/ImageCodec` already
// uses, and the right one here for a different reason: it is inside `WinMain` and
// so ahead of the device, which means the stored settings are in place before the
// renderer initialises rather than a frame or a menu later.
//
// A knob whose `GKPLUS_*` companion is set is skipped, so a launch-time override
// always outranks the file (see Settings.h).
void ApplyStoredRenderSettings();

} // namespace gk
