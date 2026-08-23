#pragma once

#include "Math.h"

namespace gk {
// The level's atmosphere: the sun, the ambient light and the fog. Everything
// here is **client-side** - none of the commands it stands in for broadcasts, so
// a binding is the setter and nothing else. That is what separates this cluster
// from the 27 that carry an update id (see console_command_notes.md §4.1).

// --- the sun -----------------------------------------------------------------
//
// The engine keeps no sun *angle*: SetSunAngle @ 0x0043e540 turns degrees into a
// normalised direction vector at SunDirection @ 0x007b9ce0 and throws the angle
// away, so the getter recomputes it from the vector (GetSunAngle @ 0x0043fcd0,
// which is what `SUNANGLE2` uses to re-apply the first angle after changing the
// second). Round-tripping is therefore lossy in the last bits.
//
// Both angles are degrees. `SunAngle2` @ 0x006a311c is stored in **BAM**
// (4096 to a turn) rather than degrees, unlike everything else here.
float GetSunAngle();
/// Sets the sun's first angle from \p degrees and rebuilds the direction
/// vector from both angles. Round-tripping through GetSunAngle() is lossy in
/// the last bits, because the vector is what is stored.
void SetSunAngle(float degrees); // 0x0043e540, __stdcall, RET 0x4

/// The sun's second angle, in degrees. The stored global is in BAM, so this
/// converts on the way out.
float GetSunAngle2();
/// Sets the sun's second angle from \p degrees, converting to BAM, and
/// re-applies the first angle the way `SUNANGLE2` does.
void SetSunAngle2(float degrees);

// 0x0043e710, __stdcall(r, g, b, a), RET 0x10. Components run 0..1.
void SetSunBrightness(float r, float g, float b, float a);

// SunDirection @ 0x007b9ce0, the normalised vector SetSunAngle derives. Read
// only: writing it without renormalising and without the shadow rebuild
// SetSunAngle does would leave the two inconsistent.
Vec3 GetSunDirection();

// --- the scene light set's emissive colour ------------------------------------

// LightSet_SetEmissiveColour @ 0x00579ef0,
// __thiscall(LightSet *this @ 0x007c18cc, AwColour *), RET 0x4.
//
// **It sets no ambient light**, despite being what the `AMBIENT` console command
// calls and despite being named SetAmbientLight here until it was read. It
// writes `this+0x48` - the Emissive member of the D3DMATERIAL8 starting at
// `this+0x18` - from colour->rgba[0..3], and `this+0x24` (Diffuse.a) from
// rgba[3]. If `this` is the current light set it re-pushes the material through
// SetD3DMaterial (IDirect3DDevice8::SetMaterial); otherwise the colour reaches
// D3D lazily through LightSet_Apply, the light set's vtable slot 3.
//
// The flags word at +0x14 selects how the colour is read: with **2** the four
// floats are authoritative, and the packed-byte conversion at the top of the
// function is skipped. `AMBIENT` sets exactly that, which is also the only
// reason it gets away with leaving +0x00 uninitialised.
//
// This is *not* `DARK`: that command also tears down the dynamic light list
// (FUN_0057a780) before setting a constant colour, and only this half is
// reproduced here.
//
// The JS binding stays `world.set_ambient` - it is a published scripting API,
// and renaming it would break every script that uses it.
void LightSet_SetEmissiveColour(float r, float g, float b, float a);

// --- fog ---------------------------------------------------------------------
//
// All five fog commands are registered by `BeginLevelSession`, not by
// `SetupConsoleCommands`, and every one of them dereferences `FogSystem`
// @ 0x006b0144 - a pointer that is **null outside a level**. `CommandFogColour`
// null-checks it and the other four do not, which is a latent crash in the game;
// every accessor here checks, and the getters return false/0 when it is null.
bool HasFog(); // is FogSystem non-null - i.e. is a level loaded

// 0x00472230, __stdcall(bool). It writes a fog *mode* (1, 2 or 3) rather than a
// flag - 3 when the device supports the two extensions it probes - so there is
// no matching getter and `GetFogMode` reports the raw value instead.
void SetFogEnabled(bool enabled);
/// The fog mode in force: 1, 2 or 3, and 0 when there is no level. Not a
/// boolean: SetFogEnabled(true) picks 3 on a device that supports both
/// extensions it probes and a lower mode otherwise.
int GetFogMode(); // 0 when there is no level

// Plain floats on the fog object. `transition` also maintains the reciprocal the
// renderer actually reads, at +0xb0, which is why it has a setter of its own
// rather than being a raw field.
float GetFogValue();      void SetFogValue(float value);       // +0xa4
float GetFogUpdateRate(); void SetFogUpdateRate(float rate);   // +0xa8
float GetFogTransition(); void SetFogTransition(float metres); // +0xac, +0xb0

// `FogOfWar_SetColour` @ 0x00469270, __thiscall(FogSystem, float rgba[4]),
// RET 0x4: stores the colour at +0xb8..+0xc4, rebuilds the packed D3D texture
// factor and pushes it as a render state. Components run 0..1, unclamped.
//
// **Do not call this per frame.** It is not a setter - it also marks the whole
// 256x256 fog grid dirty (+0x90..+0x9c) and calls `FogOfWar_UploadTexture`,
// which re-expands all 65,536 cells into a locked `D3DPOOL_SYSTEMMEM` surface
// and `CopyRects` the whole surface onto the managed video texture. Measured at
// **~85 us a call** (80-90 us under `GKPLUS_RENDERER=d3d8`, 90-100 us under
// `vulkan` - the two agreeing is the reading: the cost is the CPU expansion
// loop, not the upload path), about 0.5% of a 16.6 ms frame. Nothing in the
// shipped game calls it per frame; its only two callers are the `FOGCOLOUR`
// console command and `FogOfWar_BuildTextures` at level load / device reset.
//
// And with the `D3DFMT_A8` texture the game normally gets, none of that
// re-upload changes a pixel: `FogOfWar_ExpandRect8bpp` writes alpha only and
// never reads the packed colour at +0xb4, so the colour reaches the screen
// through `D3DRS_TEXTUREFACTOR` alone. See `stealth_and_fog_notes.md` 6.4.
//
// It also overwrites the **global backbuffer clear colour** `ClearColour`
// @ 0x007c1204, which `ClearBackBufferAndZ` passes to `Clear()` - so changing
// the fog colour changes what the whole frame is cleared to.
//
// PROPOSED, not implemented: a cheap path would reimplement steps 1-3 of
// `FogOfWar_SetColour` here (store, pack `ClearColour` + `SetRenderState`,
// rebuild +0xb4) and skip the upload, guarded on
// `*(int *)(*(int **)(FogSystem + 0xc8) + 0x20) == 28` (D3DFMT_A8) with a
// fallback to the game's own function on any other format. The guard is the
// load-bearing part: at 16/32 bpp the expanders do OR the packed colour into
// every texel, so skipping the upload there would leave the old colour on
// screen.
void GetFogColor(float *rgba);
/// Sets the fog colour through the engine's own `FogOfWar_SetColour`, which
/// re-expands and re-uploads the fog texture, the expensive path described
/// above. Components run 0..1. A no-op when no level is loaded, since the fog
/// system is then null.
void SetFogColor(float r, float g, float b, float a);
} // namespace gk
