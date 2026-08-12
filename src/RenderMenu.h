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
// Every knob here is a live `vulkan::` setting and nothing is written to disk, so
// the page reflects and edits the current session only: settings return to their
// `GKPLUS_*` / built-in defaults on the next launch. The whole page, and the
// Options item that reaches it, are absent unless the Vulkan renderer resolved -
// under `GKPLUS_RENDERER=d3d8`/`d3d9` none of these knobs does anything.
void RegisterRenderMenu();

} // namespace gk
