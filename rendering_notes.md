# The rendering system

Gunlok's renderer is a **separate library, `AWAPI`**, not part of the game code. Two `__FILE__`
strings survive in the binary and are the only direct evidence of the source layout:

```
0x0066d980  c:\projects\classics\gunlok\code\awapi\aw3d.cpp     (referenced from ResetDefaultRenderStates @ 0x00590250)
0x0066db6c  c:\projects\classics\gunlok\code\awapi\vertbuff.cpp (referenced from AwMaterial_Compile   @ 0x005a2460)
0x00686c18  C:\Projects\Classics\GunLok\Code\Gl\ReleaseDX8\Gl.pdb
```

So the tree is `Code\awapi\` beside `Code\Gl\`, and the `Aw*` prefix already in the DB
(`AwMaterial`, `AwShape`, `Aw_DrawIndexedPrimitive`, `AwSharedVB`) is the library's own. Four more
of its identifiers are visible as data: `"Made from an AwShape"`, `"Made from an AwObject"`,
`"AwSharedVB"`, `"AwBBSharedVB"`.

**AWAPI is not in the AvP source drop.** The `3dc` chunk library is shared (see CLAUDE.md), but
`D:\Documenti\GitHub\aliens-vs-predator` has no `awapi`, `aw3d` or `vertbuff` under any casing, so
unlike the RIF layer there is no upstream to read — everything here is decompiled.

The code occupies roughly `0x00574000`-`0x005a6000` (device, camera, lights, materials, scene
graph, vertex buffers) plus `0x005a6000`-`0x005aa000` (the render queue) and
`0x005c6000`-`0x005c9000` (surfaces/textures). ~578 functions, of which ~144 are named.

## 1. The shape of a frame: submit, then drain once

This is the single most important thing about the renderer, and it is why looking for a
"render the world" traversal finds nothing.

`RunInGameFrame` @ 0x0046e6c0 (called from `WinMain`'s loop; the front end runs `FUN_0046eae0`
instead) does:

1. `BeginFrameAndClear`, script-queue pump, AI/input, camera update
2. `BeginScene` @ 0x0058ffe0
3. **the submit phase** — every producer pushes into the one global `RenderQueue` @ 0x00803e98
   through `RenderQueue_Submit` @ 0x0059d760. Nothing is drawn. The producers are
   `FUN_005201c0` (world effects), `FUN_004720a0` (the map, only when `TheMap` is set),
   `FUN_0049f4f0`, `FUN_0055fb20` (HUD), `FUN_00498610`, `FUN_005695a0` (in-game menu),
   `FUN_004d5060` (console).
4. `RunGameFrame` @ 0x0046d410 — which submits ~22 UI sprites of its own first — then
   `RenderSceneAndPresent` @ 0x00574c50 → `RenderQueue_Flush` @ 0x005a90c0, where the queue is
   state-sorted and drawn.

The word "then" is doing less work than it looks: the queue is **not** flushed exactly once per
frame. `SubmitAndFlushMapGeometry` calls `RenderQueue_Flush` itself at step 3, so the world geometry
drains before the rest of the submit phase has even run. See §5.

`RenderQueue_Submit` has **102 call sites in 31 functions** — the menus
(`UpdateAndDrawMenuScreen` alone accounts for 29), the HUD, the sky, effects, the map, the
console. It is the universal "draw this" verb.

The drain is a two-level state sort followed by a depth sort:

```
RenderQueue_Flush
  ├─ opaque  list (+0x00) : per MaterialBucket → MaterialBucket_Draw
  ├─ blended list (+0x10) : same
  └─ sorted  list (+0x20) : DrawItemList_Render(.., 0)   -- back-to-front, drawn LAST

MaterialBucket_Draw   = ApplyMaterial once, then one TextureBucket_Draw per texture
TextureBucket_Draw    = DrawItemList_Render(this, this->texture)
DrawItemList_Render   → DrawItem_RenderGeometry @ 0x0059d390
                      → SceneNode_Render  @ 0x0059ae60  (recursive over children)
                      → SceneMesh_Render  @ 0x00599230
                      → SubMesh_DrawIndexed / SubMesh_DrawWireframe
                      → Aw_DrawIndexedPrimitive @ 0x005a3c20 → D3D
```

`RenderQueue_Add` routes on the `AwMaterial`: `needs_depth_sort` (+0x2c) → the sorted list;
otherwise `alpha_blend_enable` (+0x14) picks opaque vs blended. A material with `next_pass`
(+0x38) causes the item to be **cloned** and re-added, so multi-pass materials fan out into
extra draws. The queue is emptied by flushing and refilled from scratch every frame.

The C++ mirror of everything below is `src/Render.h` / `src/Render.cpp`, and its `static_assert`s
are the check on any layout edit — build after touching them rather than eyeballing.

## 2. Class hierarchy

There is one, it is three levels deep, and it is recovered from **base-constructor call edges plus
the vtable overwrites MSVC emits in a constructor** — a derived ctor calls the base ctor (which
installs the base vtable) and then stores its own over it. That overwrite is the only reason the
inheritance is visible at all.

```
RefCountedBase                 8 bytes {vptr, refcount}   vtbl_RefCountedBase 0x006522e8
                               its one slot frees 8 bytes

AwFrame                        ctor AwFrame_Ctor   @ 0x0059f020   vtbl_AwFrame  0x0066da08
  │                            2 slots: {AwFrame_EnsureMatrix, AwFrame_EnsureInverseMatrix}
  │
  ├── AwNode : AwFrame, RefCountedBase@+0x9c
  │            ctor AwNode_Ctor @ 0x0059f110
  │            vtbl_AwNode 0x0066da1c (+0x00, the world-space overrides)
  │            vtbl_AwNode_RefCounted 0x0066da24 (+0x9c)
  │     │
  │     └── Renderable          0x1f0 bytes
  │                ctors Renderable_CtorFromShape  @ 0x0059c0f0  (177 call sites)
  │                      Renderable_CtorFromObject @ 0x0059c3a0
  │                      Renderable_CtorCopy       @ 0x0059c840
  │                dtor  Renderable_Dtor           @ 0x0059bee0
  │                vtbl_Renderable 0x0066da40 (+0x00) / 0x0066da48 (+0x9c)
  │
  └── SceneNode                 0x1a0 bytes
               ctors CreateSceneObjectFromRifObject @ 0x00599f80
                     SceneNode_CtorFrom            @ 0x0059a390
               dtor  SceneNode_Dtor                @ 0x0059a910
               vtbl_SceneNode 0x0066da34 (3 slots: the two AwFrame slots, inherited
                              unchanged, plus its own deleting destructor)
```

**`Renderable` is `RenderQueue_Submit`'s `this`.** The fields that function touches line up
exactly: sort key at +0x94, refcount at +0xa0 — which is `RefCountedBase`'s member, the
subobject's vptr being at +0x9c — and the default `List<AwTexture*>` at +0x18c. It is the object
game code creates for everything it wants drawn: `ToMap`, `CreateLaserFence`,
`CreateMenuBracketMeshes`, `EnterMainMenuScreen`, the console backdrop sprite, and 30-odd more.

**`SceneNode` is what `DrawItem+0x0c` points at**, and what `SceneNode_Render` walks: LOD array at
+0xac (indexed by distance), world matrix at +0xd4, flags at +0x134, child list header **pointer**
at +0x188. A `Renderable` constructor allocates one of these internally, so the renderable owns its
root node.

The distinction between the two is worth keeping straight because both are ~0x1a0-0x1f0 bytes and
both descend from `AwFrame`: only `Renderable` carries the refcount base, and only `SceneNode`
treats +0x188 as a list.

### `AwFrame`: a lazily-cached transform, and what its two virtuals are

`AwFrame_EnsureMatrix` @ 0x0057aef0 rebuilds the 3×4 forward matrix at `this+0x2c` from the euler
triple at +0x10/+0x14/+0x18, guarded by the flag word at +0x8c (bit 0 = valid, bit 2 = rotation
built, bit 3 = translation built; 0x800 and 0x200 mean the caller supplied the matrix or the
translation directly and no build happens).

`AwFrame_EnsureInverseMatrix` @ 0x0057b120 calls slot 0 first, then builds the inverse into
+0x5c..+0x88 by **transposing the 3×3 and rotating the negated translation** through it — the
three `XOR 0x80000000` float sign flips in the decompilation are that negation. Transpose is the
inverse only because the rotation part is orthonormal, which it is by construction.

`AwNode` overrides both with `AwNode_EnsureMatrix` @ 0x0057b730 / `AwNode_EnsureInverseMatrix`
@ 0x0057b830, which call the base versions for the local part and then compose with the parent
transform reached via +0xa4/+0xbc, caching under bit 0x20 of the same flag word.

Nothing below `AwNode` overrides either slot again — the three concrete classes inherit them
unchanged.

## 3. Vtable inventory: why there is so little polymorphism

Scanning `.rdata` for runs of function pointers referenced from code yields 585 vtables program-wide
and 141 that point into the render range. **Almost all of them are one slot holding a scalar
deleting destructor** — they are `List_Member<T>` node classes (one per payload type, matching
`src/List.h` and AvP's `list_tem.hpp`) and refcounted objects. `vtbl_DrawItem` @ 0x0066da04 is
typical: its single slot frees 0x30 bytes.

Counting actual virtual dispatch in `0x00574000`-`0x005c9000`: **291 calls through slot 0** (nearly
all of them the `(**(code**)*p)(1)` deleting-destructor idiom) and a couple of dozen elsewhere, most
of which are `IDirect3DDevice8` COM calls at large offsets (+0x94 `SetTransform`, +0x130
`DrawIndexedPrimitive`, …) rather than game vtables. The renderer gets its polymorphism from D3D8,
not from itself.

The exceptions — the genuinely polymorphic seams — are these, and there are only three:

| Interface | vtable | Slots | Dispatched from |
|---|---|---|---|
| `AwFrame` / `AwNode` transform | 0x0066da08 / 0x0066da1c | 2 | everywhere a matrix is needed |
| **`LightSet`** | `vtbl_LightSet` 0x0066ccd0 | 6 | `DrawItemList_Render`, `SceneMesh_Render` |
| per-`DrawItem` pre/post pair | (caller-supplied) | ≥3 | `DrawItemList_Render` |

### `LightSet` — the one real interface

Six slots at 0x0066ccd0:

```
+0x00  scalar deleting destructor          0x0057ac50
+0x04  LightSet_SelectLightsForBounds      0x0057ac80
+0x08  (unnamed)                           0x0057a6d0
+0x0c  LightSet_Apply                      0x0057a8c0
+0x10  Light_Disable                       0x0057a940
+0x14  (unnamed)                           0x0057aa20
```

(0x0066ccc4 is a *different*, 3-slot vtable that happens to sit immediately before it — do not
read the two as one table.)

`LightSet_Apply` walks the light list at +0x08, calls `Light_Apply` into D3D light slots 0..n,
`LightEnable(false)` on the remaining slots up to `MaxD3DLights` @ 0x006ab97c, calls
`SetD3DMaterial`, and then publishes itself as `CurrentLightSet` @ 0x007c18bc. `DrawItemList_Render`
invokes it through the vtable whenever a `DrawItem`'s own light set (`DrawItem+0x1c`) differs from
`CurrentLightSet` — **lighting is state-sorted exactly the way materials and textures are.**

`SceneLightSet` @ 0x007c18cc is the scene-wide one, installed by `InstallSceneLightSet`
@ 0x0057aaf0 along with three 0x5c-byte scratch sets at 0x007c18c0/c4/c8. `SceneMesh_Render` calls
slot +0x04 with the mesh's own bounds (+0x48, +0x54) and world matrix — per-mesh light selection
from a bounding volume — but **only while `CurrentLightSet == SceneLightSet`**, i.e. only when no
draw item has overridden the lighting.

### The per-`DrawItem` callback pair

`RenderQueue_Submit`'s 9th argument lands at `DrawItem+0x2c`. If non-null, `DrawItemList_Render`
calls its vtable slot 1 (+0x04) immediately **before** `DrawItem_RenderGeometry` and slot 2 (+0x08)
immediately **after**, both with `this` = the `Renderable`. This is an engine mechanism in active
use: of the 102 submit sites, **56 pass null and 46 pass an object** — 20 of
`UpdateAndDrawMenuScreen`'s 29, plus sites in the HUD (`FUN_0055fbd0`), the world-effects pass
(`FUN_005201c0`) and `FUN_0049f4f0`.

## 4. Hook points

Ranked by how much they see for how little they cost. Everything here is main-thread; the renderer
does no work on the executor thread (`file_io_notes.md` §1 makes the same observation about I/O).

| # | Function | Address | Fires | Sees |
|---|---|---|---|---|
| 1 | `RenderQueue_Submit` | 0x0059d760 | 102 sites / 31 callers, many times per frame | every *retained* draw, before any sorting: the `Renderable`, the `SceneNode`, the `AwMaterial`, the camera, the `LightSet`, the flags, the texture list. **Not every draw the game issues** — see §4.1 |
| 2 | `RenderQueue_Flush` | 0x005a90c0 | **not** once per frame — see §5 | the whole queue as it stands, before anything in it is drawn — the place to add or drop draws wholesale |
| 3 | `DrawItem_RenderGeometry` | 0x0059d390 | once per drawn item | the item post-sort, with its material and lighting already bound |
| 4 | `ApplyMaterial` | 0x005a3a30 | once per material bucket | material binds only — a cheap "what is this frame made of" probe |
| 5 | `LightSet_Apply` | 0x0057a8c0 | once per lighting change | every D3D light change; also the publish point for `CurrentLightSet` |
| 6 | `SceneMesh_Render` | 0x00599230 | once per submesh list | the actual geometry, its textures and the translucent-pass filter |
| 7 | the `Aw_Draw*` quartet | 0x005a3c20/3d30/3e10/3ed0 | every primitive | the bottom of the funnel, and the **only** total seam — see §4.1 |
| 8 | `RunInGameFrame` | 0x0046e6c0 | once per in-game frame | the seam **between** submit and drain, if you want to inject a producer that behaves like the game's own |

Notes that matter before hooking any of them:

- **`RenderSceneAndPresent` @ 0x00574c50 wraps its whole body in `if (DAT_007c1230 != 0)`**, and
  that gate is cleared when the window loses focus. Nothing below it runs while the game is
  unfocused. This is the same measurement `src/GUI.h`'s `SetFrameWakeupEnabled` rests on — see
  CLAUDE.md's REPL section, which has the focus behaviour in full.
- `RenderQueue_Submit` is `__thiscall` with **nine stack arguments**; per CLAUDE.md's calling
  convention rule, check the `RET` operand (0x24) before declaring a mirror for it.
- Hooking at level 1 or 2 is the only way to see a draw that is later *dropped*: the sorted list
  can divert an item (`MaterialBucket_AddItem` sends anything with a translucent texture and a
  `blended_variant` material to the global sorted list), and a `next_pass` material clones items,
  so the count at level 3 does not match the count at level 1.
- The pre/post `DrawItem+0x2c` pair is a real extension point the engine already uses, but it is
  supplied *by the submitter*, so reaching it means intercepting submits — level 1 again, not a
  cheaper seam.

## 4.1 The render queue is not the only way to D3D

**There are two draw paths, and the queue is the smaller one.** An earlier revision of this file
called `Aw_DrawIndexedPrimitive` "the bottom of the funnel — 6 callers, nothing gets past it". The
second half is right and the first half is misleading: there are **four** `Aw_Draw*` wrappers, and
of the nine functions that call them, only **two** are downstream of `RenderQueue_Flush`.

Anything hooking `RenderQueue_Submit` or `RenderQueue_Flush` therefore never sees text, particles,
the in-game menus, the shadow renderer or the world-effect overlays.

```
Aw_DrawIndexedPrimitive   0x005a3c20   VertexBufferSet + IndexBufferSet
Aw_DrawPrimitive          0x005a3d30   VertexBufferSet
Aw_DrawPrimitiveUP        0x005a3e10   user pointer + FVF          <- no D3D buffer exists
Aw_DrawIndexedPrimitiveUP 0x005a3ed0   user pointer + FVF + indices <- no D3D buffer exists

 ├─ RETAINED (the queue)
 │    SubMesh_DrawIndexed      0x005a1110   ─┐ the only two callers reached
 │    SubMesh_DrawWireframe    0x005a1160   ─┘ from RenderQueue_Flush
 │
 ├─ IMMEDIATE — RenderBatch_Draw 0x005a3970, 8 callers
 │    0x00582d10  particle renderer
 │    0x00578180 / 0x00578a00 / 0x005792d0  font + 2D overlay (glyph quads)
 │    0x005695c0 / 0x00569b10 / 0x00569ed0  in-game menu widgets
 │    0x00496df0
 │
 └─ IMMEDIATE — shadows and world effects
      ShadowRenderer_Quality23 0x0054fe40  (installed as a fn ptr by ApplyShadowQuality)
      0x00559680 / 0x00559c80             (under ScenePass_Shadows, via 0x00558550)
      0x005c3ca0 / 0x005c3cc0 / 0x005c3d50 (UP draws, under ScenePass_Shadows and
                                            ScenePass_WorldEffects)
```

Three things this establishes, in order of how much they constrain a replacement renderer:

- **The quartet is total, and that is measured rather than assumed.** Scanning all 662,906 `.text`
  instructions for a `CALL dword ptr [reg + disp]` whose `disp` matches an `IDirect3DDevice8Vtbl`
  `Draw*` slot (0x118/0x11c/0x120/0x124) yields 24 candidate functions — but only these four also
  reference `direct3d_device` @ 0x007c121c. The other 20 are **displacement collisions on unrelated
  vtables**: an `Actor` vtable has 83-105 slots, so 0x118..0x124 is slots 70-73 and lands squarely
  inside it. That is why the hit list contains things like `SyncPositionAndBroadcast` and
  `ApplyUpdateMessage`. A displacement-only scan is not a call-site scan; intersect it with the
  device global.
- **Two of the four take a user pointer**, so for the shadow, scanner, level-load-overlay and
  world-effect draws there is no `IDirect3DVertexBuffer8` in existence at any point. Any scheme that
  captures geometry by intercepting `CreateVertexBuffer`/`Lock` silently misses all of them.
- **`ShadowRenderer_Quality23` has no call edge at all.** Its only two references are DATA refs
  inside `ApplyShadowQuality` (0x0054fc1e, 0x0054fc44), so a caller walk reports it as a root and a
  reachability query says nothing reaches it. This is CLAUDE.md's function-pointer trap in its
  purest form.

A `RenderBatch` auto-flushes at 0x800 vertices / 0xc00 indices; `FUN_005792d0`, the glyph quad
emitter, is the clearest read on the structure (`+0x2c` D3DPRIMITIVETYPE, `+0x50` vertex count,
`+0x54` index count, and it emits 4 verts / 6 indices per glyph).

## 5. The producers

All 31 of them, with what they draw. The evidence for the UI ones is the **localized string ids they
fetch** — `glreseng.dll` carries 1527 strings and `GetResourceString`'s argument names the screen
outright, which is far more reliable than reading the drawing code. (Parse the string table straight
out of the DLL's `RT_STRING` resources; string *id* N lives in block `N/16 + 1` at index `N % 16`.)

| Producer | Address | Sites | Reached from |
|---|---|---:|---|
| `UpdateAndDrawMenuScreen` | 0x004ea8e0 | 29 | the front-end frame `FUN_0046eae0` |
| `DrawWorldEffects` | 0x005201c0 | 15 | `RunInGameFrame` **and** `ScenePass_WorldEffects` |
| `DrawHud` | 0x0055fbd0 | 11 | `FUN_0056a7b0` |
| `DrawInventoryScreen` | 0x0049f4f0 | 6 | `RunInGameFrame` |
| `Unit_Draw` | 0x004b6ae0 | 4 | Unit vtable slot 68 |
| `DrawInventoryItemPanel` | 0x004a7890 | 3 | `DrawInventoryScreen` |
| `Unit_DrawWithTeamState` | 0x004be830 | 3 | Unit vtable slot 68 |
| `DrawItemValueBrackets` | 0x004fa930 | 3 | `UpdateAndDrawMenuScreen` |
| `DrawItemLabelBrackets` | 0x004fb500 | 3 | `UpdateAndDrawMenuScreen` |
| `DrawOrderMenu` | 0x00498610 | 2 | `RunInGameFrame` |
| `DrawInventoryFrame` | 0x0049eba0 | 2 | `FUN_00484e40` |
| `InGameMenuWidget_Draw` | 0x0056c3c0 | 2 | widget vtable slot 2 |
| `SubmitAndFlushMapGeometry` | 0x004720a0 | 1 | `RunInGameFrame` |
| `DrawCtfFlagMarker` / `…2` | 0x004963b0 / 0x004963f0 | 1 each | `DrawUnits` / `Unit_DrawWithTeamState` |
| `SubmitSkyDome` | 0x004a0ed0 | 1 | `FUN_00484e40`, `FUN_00497ca0` |
| `DrawTargetInfoPanel` | 0x004a86c0 | 1 | `DrawOrderMenu` |
| `DrawTargetingReticule` | 0x004ac540 | 1 | `DrawTargetInfoPanel` |
| `Sprite_Draw` | 0x004b0df0 | 1 | sprite vtable slot 1 |
| `DrawUiSprite` | 0x004b1370 | 1 | `RunGameFrame` |
| `DrawBriefingScreen` | 0x004b29d0 | 1 | its own frame body |
| `DrawScreenTextLines` | 0x004d06b0 | 1 | the console/message subsystem |
| `DrawConsole` | 0x004d5060 | 1 | four frame bodies |
| `CameraTrackObject_Draw` | 0x004dd580 | 1 | camera-track vtable slot 5 |
| `DrawScannerOverlay` | 0x005243a0 | 1 | `ScenePass_Shadows` |
| `DrawLevelLoadOverlay` | 0x00524740 | 1 | `ScenePass_Shadows` |
| `DrawWorldEffect_Unknown` | 0x00529750 | 1 | `DrawWorldEffects` |
| `ShadowRenderer_Quality1` | 0x005520a0 | 1 | `ShadowRendererFn` |
| `DrawTeamSelectOverlay` | 0x00566bd0 | 1 | `FUN_00565920` |
| `InGameMenuWidget_DrawLabelled` | 0x0056c6e0 | 1 | widget vtable slot 2 |
| `SubmitSkyBackdrop` | 0x00588ad0 | 1 | `RenderSceneAndPresent` |

### The five facts worth carrying

- **The biggest single producer is a virtual slot, not a function.** `DrawUnits` @ 0x004b84a0 walks
  `UnitsTable` @ 0x007b68f0 and calls **slot 68 (+0x110) of each `Unit`'s vtable**. Sixteen `Unit`
  subclasses implement it and only *four* distinct bodies exist: `Unit_Draw` (shared unchanged by
  seven classes), `Unit_DrawWithTeamState` (six), and two thin per-class overrides that delegate to
  those. So every unit on screen — health bars, selection state, name plates — reaches the queue
  through one slot.
  Getting the slot index right needs the **code-referenced vtable start**, not the start of the
  contiguous run of function pointers: walking back over consecutive `.text` dwords lands mid-table
  and reports slot 3 for what is really slot 68.
- **`SubmitAndFlushMapGeometry` flushes the queue itself.** It submits the map's `scene_object`
  (`Map+0xc8`) with `SceneLightSet`, the `FogSystem`'s material and `RenderStateFlags|0x70010000`,
  and then calls `RenderQueue_Flush` directly. The world geometry is therefore drained *ahead* of
  everything `RunInGameFrame` submits after it — the queue is flushed more than once a frame, and a
  hook that assumes one flush per frame will be wrong. It is also the one producer that passes the
  per-`DrawItem` pre/post callback object (`Map+0xa4`).
- **`DrawWorldEffects` runs twice per frame, from two different places** — once directly from
  `RunInGameFrame` during the submit phase, and again as `ScenePass_WorldEffects`, which
  `RenderSceneAndPresent` invokes as its `preCb` from *inside* the scene. `ApplyShadowQuality` is
  what installs that callback pair, along with `ShadowRendererFn`: quality 0 gets the `CommandRem`
  no-op stub, quality 1 `ShadowRenderer_Quality1`, quality 2 and 3 `FUN_0054fe40` (3 additionally
  bakes static shadows). So **the shadow setting changes which functions are producers at all.**
- **The inventory screen replaces the world rather than overlaying it.** `RunInGameFrame` branches
  on `InventoryScreenOpen` @ 0x007b6e50 and `InGameInterface` @ 0x007b3f38 `+0x60`; when the screen
  is up, `DrawInventoryScreen` runs *instead of* `DrawWorldEffects` +
  `SubmitAndFlushMapGeometry` + `DrawUnits`. Anything counting draws per frame sees the world
  disappear entirely, which is correct behaviour and not a dropped hook.
- **`RunGameFrame` is a producer too, and a bulky one.** Before it calls `RenderSceneAndPresent` it
  builds ~22 UI sprites through `FUN_004b1150` and submits each with `DrawUiSprite`. The submit
  phase is therefore split across `RunInGameFrame` *and* `RunGameFrame`, not confined to the former.

Two producers keep a hedge in their names: `DrawWorldEffect_Unknown` (which effect it draws is not
established) and `DrawTeamSelectOverlay` (named from its neighbours — the least-verified name here).

## 6. What the fields turned out to be

`src/Render.h` carries the full layouts. The findings worth stating in prose are the ones that
changed how the structures read, in decreasing order of how much they explain:

- **`AwFrame`'s two virtual slots return the matrix; they are not `void`.** Each early-outs on its
  cache bit and returns a pointer — `AwFrame::EnsureMatrix` gives `&matrix` (+0x2c),
  `AwNode`'s override gives `&world` (+0x11c). That return value is the whole point of the
  override, and it is what `DrawItem_RenderGeometry` feeds into `Matrix3x4ToD3DMATRIX` before
  `SetTransform`. Modelling them as `void` — the first attempt — loses the only piece of real
  dispatch on the draw path.
- **`AwNode`'s four matrices are two derived pairs.** `scale_matrix` (+0xbc) is `diag(scale)`,
  `world` (+0x11c) is `matrix * scale_matrix`, and the inverse side mirrors it exactly. Flag
  `0x80000000` short-circuits the multiply and copies `matrix` straight across. Every cached
  matrix has its own "built" bit and every input its own "this is the source" bit, so a setter
  ORs in one bit and masks out every derived bit in a single AND — which is why the flag word
  looked arbitrary until all eight cache bits were placed.
- **`LightSet` is a D3D material with a light list bolted on.** Its 0x44-byte tail is a
  `D3DMATERIAL8` — `SetD3DMaterial` hands `this + 0x18` to `IDirect3DDevice8::SetMaterial`, and
  the five accessor thunks return exactly +0x18/+0x28/+0x38/+0x48/+0x58. `0x08 + 0x10 + 0x44` is
  0x5c, the whole object, with nothing left over. That also explains why `LightSet_Apply` binds a
  material as well as lights, and why the engine's "ambient light" is really this object's
  Emissive term.
- **`AwMaterial` ends in `AwTextureStage stages[8]`.** `AwMaterial_Compile` walks from
  `this + 0x3c + num_stages * 0x30` downwards in 0x30 steps, and `0x3c + 8 * 0x30 == 0x1bc` is the
  object exactly. The stage record's field order is the order `AwMaterial_ApplyStage` issues
  `SetTextureStageState` in.
- **`SubMesh` is a two-mode record**, discriminated by `vertex_buffer_owner`: null takes the
  user-pointer path (`vertices` + `fvf` + `indices`), set takes the buffered path (`index_buffer`
  + `base_vertex`, FVF read off the vertex buffer set). Half its fields are dead in either mode.
  Its `+0x1c` is an **index** count, not a primitive count — the wireframe path divides it by 3
  and `Aw_Draw*` runs it through `PrimitiveCountFromVertexCount` — so the frame counter at
  0x00803c08 that accumulates it counts indices.
- **`DrawItem+0x0c` is a timestamp and `+0x24` is a LOD index**, neither of which looks like it at
  the call sites. The first drives `Renderable_AdvanceAnimations`, which advances the controller
  list at `Renderable+0x1e0` — `Sequence_Advance` scales it into the same 16.16 domain a RIF
  `OBASEQFR` keyframe's `time` uses (`rif_chunk_format.md`). The second selects
  `SceneNode::lods[]` and is passed unchanged to every child, so one draw item picks the LOD for
  a whole subtree.
- **`Renderable+0x17c` is the root `SceneNode`** — the object `DrawItem_RenderGeometry` hands to
  `SceneNode_Render`, with the matrix it just composed as the parent transform. That is the join
  between the two halves of the hierarchy.
- **`SceneNode::bounds` (+0x120) is a `{Vec3 centre, float radius}` sphere**, and null disables
  culling for that node. The verdict lands in `cull_result` (+0x118) and is threaded into every
  child as *their* incoming verdict, so a culled node prunes its subtree without each child
  retesting.

A second pass, driven by each type's **constructor / destructor pair** rather than by the draw
path, closed most of what was left. The constructor gives the *shape* — `eh_vector_constructor`
with a count of 3 or 4 is a `Vec3` or `Vec4`, `List_Ctor` or a self-linked `pool_alloc(0xc)` is a
`List<T>` — and the destructor gives *ownership*, since it names the size of everything it frees.
That is what turned Renderable's tail from sixteen loose dwords into six named fields plus four
typed ones, and it is worth reaching for before reading any more call sites:

- `Renderable::debug_name` (+0x180) is a 21-byte `malloc` holding `"Made from an AwShape"` — the
  only thing the two "from" constructors differ in.
- `Renderable::bounds_min`/`bounds_max` (+0x1a8/+0x1b4) and `box_corners` (+0x1c0): the third is
  built from the first two by `MakeBoxCorners`, which expands two opposite corners into eight
  `Vec3`s after a 0x18-byte header — `0x18 + 8*0xc == 0x78`, the allocation size. `has_bounds`
  (+0x1a4) is raised once they are filled and is what `DrawItem_RenderGeometry` tests.
- `Renderable::vertices` (+0x1a0) and its neighbour (+0x19c) are **counted arrays**: the element
  count sits in the dword *before* the pointer, which is how the destructor frees them
  (`count * 0xc + 4`).
- `SceneNode::name` (+0x11c) is the string `SceneNode_FindByName` matches case-insensitively —
  the same name a RIF `OBJHIERD` binding carries. `SceneNode::textures` (+0x9c) is the list a
  `Renderable` harvests into its own at construction, which is what lets it be bucketed by
  texture without walking the graph. `current_frame`/`previous_frame` (+0x198/+0x19c) are a
  rotating pair, which is what makes interpolation rather than snapping possible.
- **`AwMaterial` is fully named**, because `AwMaterial_Compile` issues its nine render states in
  field order: `z_write_enable`, `z_enable`, `alpha_blend_enable`, `alpha_test_enable`,
  `src_blend`, `dest_blend`, `clipping`, `lighting`, then `state_block` at +0x30.

Two corrections came out of these passes. `AwTexture` is **not** one of the `AwRefCounted` family
as an earlier revision had it — `AwMaterial_ApplyStage` takes the D3D texture as `**stage`,
straight off offset 0, so there is no vptr and no refcount at +0x04. It is in fact the texture
**cache record itself**, 0x34 bytes with its path at +0x2c: `AcquireRimTexture` mints it,
`AwShape_TouchTextures` looks it up by that path, `BuildShapeVertexBuffers` stores it into
`SubMesh::texture`, and `ApplyStage` binds it. And `AwMaterial::state_block` was at +0x0c in the
first draft, from reading the destructor's `param_1_00[0xc]` as a byte offset when it is a **dword
index** — it is +0x30. That is an easy mistake to repeat: the decompiler indexes `undefined4 *`
by element, so every offset in a decompiled body has to be multiplied by four before it means
anything here.

## 7. Not yet mapped

- `AwShape` and `AwObject` as classes — only their names are recovered, from two debug strings.
  `Renderable_CtorFromShape` / `Renderable_CtorFromObject` are the two ways in.
- `LightSet` vtable slots +0x08 (0x0057a6d0) and +0x14 (0x0057aa20).
**The fields still carrying a `field0xNN` name are down to 29, and they divide into exactly two
kinds** — worth stating, because neither is a matter of not having looked:

- **Constructed and destructed, read by nothing.** `SceneNode` +0x140 (a `Vec3`) and +0x14c (a
  `Vec4`), `Renderable` +0x1c8 and +0x1d4 (two `Vec3`s), `SceneMesh` +0x04 (a `Vec4`). The
  constructor builds them and the destructor tears them down and *that is the whole of their
  traffic* — not the render walk, not the culler, not the frame rotation. A `Vec3` + `Vec4` pair
  is the shape of a position and a quaternion, and there is no evidence for saying so, which is
  why they stay unnamed.
- **Filled by the geometry builder.** `SceneMesh`'s two extra lists (+0x14, +0x24) and its build
  state (+0x44, +0x60..+0x74, +0x7c, +0x8c, +0x94), and `SceneNode` +0x124/+0x130. These are
  written by `BuildShapeVertexBuffers` and `FUN_0059b7d0`, neither of which is dissected here.
  `SceneMesh_ResetBuildState` is what establishes that the whole +0x44..+0x94 run *is* build
  state, since it clears exactly that run and raises `needs_rebuild`.

Also open:

- `SceneMesh`'s Vec3 pair at +0x48/+0x54. They are read consecutively out of the `.cut` cache
  stream and handed to the light set, and `SHPHEAD1` supplies a min/max pair — but nothing here
  establishes which is which, so they stay `bounds_a` / `bounds_b`.
- `SubMesh::vertex_buffer_owner`'s type: the draw path reaches the `VertexBufferSet` as
  `*(*(submesh+0x08) + 0x08)`, and the intermediate object is not identified. It is *not* the
  `AwSharedVB` at 0x00803c70, whose +0x08 is a `CRITICAL_SECTION`.
- `ScenePass_Overlay2D` @ 0x00578ee0 — named but not analysed. (`ScenePass_WorldEffects` and
  `ScenePass_Shadows`, the `preCb`/`postCb` arguments of `RenderSceneAndPresent`, are covered in
  §5: `ApplyShadowQuality` installs them.)
- The `Unit` hierarchy itself — sixteen subclasses with ~69-slot vtables in
  0x006647ac..0x00665f60. Only slot 68 (`Draw`) and slot 67 (`FUN_004b6930`, shared by both draw
  families) are identified.
- `VertexBufferSet` / `IndexBufferSet` / `SharedVB` (`AwSharedVB`, `AwBBSharedVB`) — named, and
  clearly the `vertbuff.cpp` half of AWAPI, but their pooling policy is unexamined.
- Whether any of this is reachable from a `.gls` or a console command. Nothing in
  `console_command_notes.md` currently maps to the queue.

---

### The renderer's struct mirror (`src/Render.h/cpp`)

`rendering_notes.md` is the analysis; `src/Render` is the mirror, and it is pure struct +
native-API — no `*System`, because the renderer installs no detour of GkPlus's own (the
D3D-side hooks live in `src/GUI.cpp`).

Three things about it that are not obvious from the header alone:

- **It is the second place in the codebase to use real multiple inheritance**, after
  `Map : MapBase, RefCountedBase`. `AwNode : AwFrame, AwRefCounted` puts the refcounted
  subobject's vptr at +0x9c and its `refcount` at +0xa0 purely because `AwFrame` is 0x9c bytes —
  and +0xa0 is exactly the word `RenderQueue_Add` increments. The `static_assert`s on
  `offsetof(AwNode, refcount)` and `sizeof(AwFrame)` are what prove the split reproduces the
  original.
- **`AwRefCounted` and `Map.h`'s `RefCountedBase` are the same 8 bytes and are deliberately not
  merged.** They model different points on the same chain: the root vtable 0x006522e8 has one
  slot, which is what `AwRefCounted` declares, while Map's second base sits two levels above it
  (0x006522e8 <- 0x0065281c <- 0x00652828) and `Map.h` folds the middle base's extra slot into its
  own declaration. Merging them would make one of the two wrong about its slot count; the size is
  identical either way, which is all either `static_assert` pins.
- **`AwFrame` has no virtual destructor and `SceneNode` adds one as slot 2.** That ordering is
  load-bearing: `AwFrame`'s vtable (0x0066da08) is exactly `{EnsureMatrix, EnsureInverseMatrix}`,
  and `SceneNode`'s (0x0066da34) is those two *then* the deleting destructor. Declaring the
  destructor first — the reflex — would put it in slot 0 and silently mis-model every class in
  the tree. `AwNode` and `Renderable` add nothing to the primary table at all; their destructor
  lives on the secondary vtable at +0x9c.

Four field-level findings shape the rest of the header, and `rendering_notes.md` §6 has the
evidence:

- **`AwFrame`'s two virtual slots return the matrix**, they are not `void` — each early-outs on
  its cache bit and hands back a pointer, and *which* matrix comes back is the entire point of
  `AwNode`'s override (`&matrix` vs `&world`). That return value is what reaches `SetTransform`.
- **`LightSet`'s 0x44-byte tail is a `D3DMATERIAL8`** — `SetD3DMaterial` passes `this + 0x18` to
  `IDirect3DDevice8::SetMaterial`, and `0x08 + 0x10 + 0x44` is 0x5c exactly. The engine's "ambient
  light" is this object's Emissive term.
- **`AwMaterial` ends in `AwTextureStage stages[8]`** — `AwMaterial_Compile` walks
  `this + 0x3c + num_stages * 0x30` downwards, and `0x3c + 8 * 0x30 == 0x1bc` is the object.
- **`DrawItem+0x0c` is a timestamp and `+0x24` a LOD index**, neither of which looks like it at
  the call sites; the first drives the animation controllers at `Renderable+0x1e0`.

`SubmitDrawItem` wraps `RenderQueue_Submit`, whose nine stack arguments (`RET 0x24`) are **not**
in the order their fields land in — the mapping was read off the prologue, and swapping the
camera and the light set would not fault, it would just render with the wrong one. `AwTexture`
deliberately carries no `sizeof` assert — its size is not established, and the 0x34-byte record
`AcquireRimTexture` caches is a different, earlier object. It is also **not** one of the
`AwRefCounted` family, which an earlier revision had it as: `AwMaterial_ApplyStage` takes the D3D
texture as `**stage`, straight off offset 0, so there is no vptr and no refcount at +0x04.
