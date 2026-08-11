#include "HudFix.h"

#include "Core.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

// Not DetourUtils.h: its gk::DetourAttach overloads are for __thiscall member
// pointers, and merely declaring them inside namespace gk hides the global
// templates that handle a plain function pointer.

#include <string>

namespace gk {
namespace {
// --- the defect -------------------------------------------------------------
//
// Every character's health and armour meters and their item icons are invisible
// in the shipped game: the panel plate behind them is drawn opaquely on top.
// This is Gunlok's own bug - it reproduces with d3d8.dll renamed aside - and
// game_defects_notes.md 12 is the write-up. The short version:
//
// The engine layers 2D content by giving each camera its own slice of the depth
// range, and putting that slice in the D3DVIEWPORT8 it owns at +0x254.
// InitRenderCameras @ 0x004af4d0 carves them: Camera_Menu2D 0.00..0.02,
// Camera_Text 0.02..0.04, Camera_Hud 0.03..0.04, Camera_World 0.10..1.00.
//
// The panel plates are retained render-queue items, and all 11 submit sites in
// HudItem_DrawByKind @ 0x0055fbd0 pass Camera_Hud, so they are drawn under
// 0.03..0.04 when the queue is flushed. The meters and icons never reach the
// queue at all: they are appended to a shared immediate-mode vertex batch by
// Hud2D_DrawQuad @ 0x005695c0 with an explicitly authored z of **0.03f** -
// exactly Camera_Hud's MinZ, the front of the HUD slice. That is unambiguous
// intent: the meters belong in front of the plates.
//
// They do not get there, because of the frame order in RunInGameFrame:
//
//   0046e8b8  CALL Hud2D_BeginBatch    ; opens the batch
//   0046e8c1  CALL RenderHudItems      ; Camera_Hud current; the meter quads
//   0046e8ca  CALL DrawOrderMenu       ; Camera_World current again  <-- culprit
//   0046e8cf  CALL Hud2D_FlushBatch    ; the meters are drawn under 0.10..1.00
//   ...       RenderQueue_Flush        ; and only now the plates, under Camera_Hud
//
// D3D does not run the viewport transform over a pre-transformed vertex, it
// clamps - measured against the real runtime in vulkan_renderer_notes.md 4.45,
// `depth = clamp(z, MinZ, MaxZ)`, no scale and no bias. So the authored 0.03 is
// clamped **up** to 0.1 and lands behind the entire HUD slice. Both the depth
// test and the paint order then go to the plate.
//
// --- the fix ----------------------------------------------------------------
//
// Draw the HUD's own quads while Camera_Hud is still current, by ending the
// batch early - at the end of RenderHudItems, which is the last moment at which
// everything in it belongs to the HUD - and opening a fresh one for whatever
// comes after. The meters then clamp to 0.03 instead of 0.1.
//
// End-Draw-Begin in the middle of a batch is a state transition the engine
// already performs itself: Hud2D_DrawQuad does exactly this at 0x005695e6 when
// the vertex buffer fills. An empty batch is safe too - RenderBatch_Draw opens
// with `CMP dword ptr [ESI+0x50], 0` and returns, so a frame with no HUD items
// flushes nothing.
//
// **Splitting the batch rather than re-pointing the whole flush is the point.**
// DrawOrderMenu appends one more meter bar to the same batch (Hud2D_DrawMeterBar
// @ 0x0049b322) with z = 0.1f, authored for Camera_World - it is a unit's health
// bar floating over the world, not a HUD panel element. One flush is one
// DrawIndexedPrimitive under one viewport, so forcing Camera_Hud over the whole
// thing would drag that bar from 0.1 to clamp(0.1, 0.03, 0.04) = 0.04 and move a
// second thing while fixing the first. Cutting the batch in two leaves it exactly
// where the game put it.

// RenderHudItems @ 0x0055fb20 - walks HudItemList @ 0x007ba250 and calls vtable
// slot 2 on each item. void(void), bare RET, so the convention is unobservable
// and __cdecl models it exactly.
using RenderHudItemsFn = CDecl<>;
using Hud2DBatchFn = CDecl<>;

// Camera_ApplyViewportAndZFunc @ 0x00577550 - Camera_SetDeviceViewport(this),
// i.e. SetViewport(this + 0x254), then D3DRS_ZFUNC from this->+0x1d0. One ECX
// argument, bare RET. It returns the previous cached ZFUNC in EAX and no caller
// reads it; EAX is caller-saved, so declaring it void is safe.
using CameraApplyFn = ThisCall<void, void *>;

RenderHudItemsFn OriginalRenderHudItems = nullptr;
Hud2DBatchFn Hud2DFlushBatch = nullptr;
Hud2DBatchFn Hud2DBeginBatch = nullptr;
CameraApplyFn CameraApplyViewportAndZFunc = nullptr;
CameraApplyFn CameraApply = nullptr;

// Camera_Hud @ 0x007b4e40 (the object, not a pointer to one: the 11 HUD submits
// push this address as the camera). +0x250 is the projection kind, 0 here -
// InitRenderCameras runs Camera_SetOrthographic over it.
constexpr uintptr_t CameraHudObject = 0x007b4e40;
constexpr uintptr_t CurrentCameraGlobal = 0x007c146c;
constexpr uintptr_t CurrentCameraIsPerspectiveGlobal = 0x007c1470;
constexpr size_t CameraIsPerspectiveOffset = 0x250;

bool HudFixEnabled = true;
bool HudFixHooked = false;

void ReadHudFixMode() {
  char value[32] = {};
  const DWORD len = ::GetEnvironmentVariableA("GKPLUS_HUD_FIX", value, sizeof(value));
  const std::string mode(value, len);
  if (mode.empty() || mode == "gkplus") {
    return;
  }
  HudFixEnabled = false;
  if (mode != "raw") {
    DebugWrite("gkplus: unknown GKPLUS_HUD_FIX '" + mode +
               "'; leaving the game's own HUD compositing alone\n");
  }
}

// The engine's own four-step camera switch, verbatim from RenderHudItems
// @ 0x0055fb4d and used identically by DrawItemList_Render, DrawOrderMenu and
// RenderSceneAndPresent: set the global, apply the viewport and ZFUNC, copy the
// projection kind, apply the matrices. Guarded on the global the same way, so
// re-asserting a camera that is already current costs nothing.
void MakeCameraCurrent(void *camera) {
  void **current = nullptr;
  GetObjectAtOffset(current, CurrentCameraGlobal);
  if (*current == camera) {
    return;
  }
  *current = camera;
  CameraApplyViewportAndZFunc(camera);

  unsigned char *is_perspective = nullptr;
  GetObjectAtOffset(is_perspective, CurrentCameraIsPerspectiveGlobal);
  *is_perspective =
      *(static_cast<unsigned char *>(camera) + CameraIsPerspectiveOffset);

  CameraApply(camera);
}

void __cdecl HookedRenderHudItems() {
  OriginalRenderHudItems();

  // Camera_Hud is already current unless the item list was empty, in which case
  // the original returned before switching - and then the batch is empty too and
  // the flush is a no-op. Assert it anyway: it is what makes the depth the quads
  // were authored for the depth they are drawn at, rather than an inherited one.
  void *camera = nullptr;
  GetObjectAtOffset(camera, CameraHudObject);
  MakeCameraCurrent(camera);

  Hud2DFlushBatch();
  Hud2DBeginBatch();
}

} // namespace

HudFixSystem::HudFixSystem() {
  ReadHudFixMode();
  if (!HudFixEnabled) {
    return;
  }
  GetObjectAtOffset(OriginalRenderHudItems, 0x0055fb20);
  GetObjectAtOffset(Hud2DFlushBatch, 0x00569ed0);
  GetObjectAtOffset(Hud2DBeginBatch, 0x005695a0);
  GetObjectAtOffset(CameraApplyViewportAndZFunc, 0x00577550);
  GetObjectAtOffset(CameraApply, 0x005774c0);

  ::DetourAttach(reinterpret_cast<void **>(&OriginalRenderHudItems),
                 reinterpret_cast<void *>(HookedRenderHudItems));
  HudFixHooked = true;
}

HudFixSystem::~HudFixSystem() {
  if (!HudFixHooked) {
    return;
  }
  ::DetourDetach(reinterpret_cast<void **>(&OriginalRenderHudItems),
                 reinterpret_cast<void *>(HookedRenderHudItems));
  HudFixHooked = false;
}
} // namespace gk
