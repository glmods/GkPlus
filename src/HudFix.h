#pragma once

namespace gk {
// Hook-only subsystem (no scripting surface). Makes the in-game HUD's health,
// armour and item meters visible again: the game draws them under the *world*
// viewport, where its own opaque panel plates cover them. See HudFix.cpp for the
// mechanism and game_defects_notes.md 12 for the defect.
//
// GKPLUS_HUD_FIX=raw leaves the game's own behaviour alone.
class HudFixSystem {
public:
  HudFixSystem();
  ~HudFixSystem();
};
} // namespace gk
