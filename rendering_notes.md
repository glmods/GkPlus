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

`RunInGameFrame` @ 0x0046e6c0 (called from `WinMain`'s loop; the front end runs `RunFrontEndFrame`
instead) does:

1. `BeginFrameAndClear`, script-queue pump, AI/input, camera update
2. `BeginScene` @ 0x0058ffe0
3. **the submit phase** — every producer pushes into the one global `RenderQueue` @ 0x00803e98
   through `RenderQueue_Submit` @ 0x0059d760. Nothing is drawn. The producers are
   `DrawWorldEffects` (world effects), `Map::SubmitAndFlushMapGeometry` (the map, only when `TheMap` is set),
   `DrawInventoryScreen`, `RenderHudItems` @ 0x0055fb20 (HUD), `DrawOrderMenu` @ 0x00498610,
   `Hud2D_BeginBatch` @ 0x005695a0, `DrawConsole` (console).

   `Hud2D_BeginBatch` is not a producer and was mislabelled "(in-game menu)" here: it opens the
   **immediate-mode 2D batch**, a second path that bypasses the queue entirely. See §4.3.
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
read the two as one table. It is `vtbl_AwLight`, and it is what `Light`'s constructor installs; see
"`Light`, `SceneLight`, and the single `SetLight` call site" below. The table before *that*,
`TextDrawNode_vtbl` @ 0x0066ccc0, is one slot, not three — the database had it as a `pointer[3]`
overlapping `vtbl_Light` until 2026-08, which `address_map.md` had already recorded correctly.)

The object is `{vptr, int refcount, List<Light *> lights @ +0x08, D3DMATERIAL8 material @ +0x18}`
= 0x5c, and `LightSet::AddLight` @ 0x0057aa20 takes **one argument** — a `const D3DLIGHT8 *`, copied
whole into the new light by a `REP MOVSD` of 0x68 bytes (both it and `SceneLightSet`'s override are
`RET 0x4`). `src/Render.h` declared that slot as `AddLight()` with no parameter until 2026-08.

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

`SceneLightSet` is a **subclass**, not the base object: 0x6c bytes = `LightSet` (0x5c) plus its own
`List<SceneLight *>` at +0x5c, with vtable **0x006523b8** and three overrides. `WinMain` builds it inline
(`PUSH 0x6c; CALL malloc` @ 0x0046b815, base ctor, `MOV dword ptr [ESI],0x6523b8` @ 0x0046b83f, a
fresh 0xc-byte list sentinel into +0x5c, then `InstallSceneLightSet`).

```
+0x00  0x0046c140                                   (base 0x0057ac50, the deleting dtor)
+0x04  SceneLightSet_SelectLightsForBounds 0x00488400   override — base 0x0057ac80 is an empty RET 0xc
+0x08  SceneLightSet::RemoveLight 0x00488640        override of LightSet_RemoveLight 0x0057a6d0
+0x0c  LightSet_Apply                               inherited
+0x10  Light_Disable                                inherited
+0x14  SceneLightSet::AddLight 0x00488740           override of LightSet_AddLight 0x0057aa20
```

**Those two overrides exist to split the lights into two lists by type**, and that is the fact the
rest of this section rests on. Both call `Light::IsDirectional` @ 0x005799b0 (`d3d.Type == 3`): a
directional light goes in the **inherited** list at +0x08, everything else in `SceneLightSet`'s
**own** list at +0x5c. Only the +0x5c list is walked by the per-mesh selection below, so a
directional light is never fog-attenuated at all; and because the D3D slot index starts at +0x0c —
the inherited list's *count* — directional lights own slots `0..n-1` and selected point lights start
at `n`. (The base `LightSet::AddLight` does no such thing: it puts every light in +0x08 regardless.)
`SceneLightSet::AddLight` also gates on `DynamicLightsOn` @ 0x006abdfc: with it clear, only a
directional `src` is accepted at all.

One defect, benign: `SceneLightSet::RemoveLight` frees the light with `operator delete(light, 0x6c)`
at 0x0048872a — the *base* size — where `AddLight` allocated 0x90 at 0x00488787 and the destructor
frees 0x90 at 0x0048831a. Gunlok's pool allocator ignores the size argument.

Because lighting is state-sorted through this interface, that slot-1 override — not the base stub —
is **the** lighting path for world geometry, and it does something nothing else in the renderer
does:

- **Every point and spot light is attenuated by the fog of war at the light's own position — in
  its `D3DLIGHT8.Specular`, and nowhere else.**
  `SceneLightSet_SelectLightsForBounds` @ 0x00488400 (`void __thiscall(SceneLightSet *, const Vec3
  *bounds_max, const Vec3 *bounds_min, const AwMatrix *world)`, `RET 0xc`) early-outs unless
  `direct3d_device != 0 && DynamicLightsOn @ 0x006abdfc`, then walks the light list at +0x5c testing
  each light's sphere against the AABB translated by the world matrix's translation row
  (`world+0x30/0x34/0x38`) on all three axes. A light that passes is scaled by
  `1.0 - FogOfWar_SampleTotal(lightPos) * (1/254)` through its own vtable slot 3 before
  `Light_Apply` binds it — `FLOAT_00663f54` is 0.003937, the same 1/254 divisor `Unit_Draw` uses.
  D3D slot indices start at `this+0xc`, wrap back to 1 at `MaxD3DLights`, and every unused slot up
  to `MaxD3DLights` gets `LightEnable(i, FALSE)` afterwards. Its only reference is the vtable entry
  at 0x006523bc; there are no direct callers.

  Four details of that scaling are load-bearing, and the first was recorded wrongly here until
  2026-08:

  - Slot 3 is `SceneLight::SetFogScale` @ 0x00488250, and it writes **only**
    `D3DLIGHT8.Specular.r/g/b` (light `+0x18/+0x1c/+0x20`, via `Light::SetSpecularRGB`
    @ 0x005799c0). `Specular.a`, `Diffuse` and `Ambient` are untouched. The fog therefore dims
    the *specular* response of a light and nothing else — which is visible because
    `D3DRS_SPECULARENABLE` is TRUE for the whole session (set once at 0x0059047a in
    `ResetDefaultRenderStates`, and none of the 105 `SetRenderState` sites touches it again) with
    `D3DRS_SPECULARMATERIALSOURCE = D3DMCS_COLOR2` at 0x005908e7.
  - **It floors at 0.5.** `FogOfWar_SampleTotal` returns 0..0x7f and the divisor is exactly 254, so
    a fully fogged light is halved, never extinguished.
  - **It never compounds.** `SetFogScale` multiplies the *authored* colour cached at
    `SceneLight+0x6c` — seeded from the source `D3DLIGHT8.Specular` when the light is added, and
    re-cached by the slot-2 override `SceneLight::SetSpecular` @ 0x00488200, which re-applies the
    current scale immediately. Authored colour and fog scale are orthogonal, in either order.
  - It runs **per mesh**, not per frame: `SceneMesh_Render` is the caller.

  Measured in the running game, not just read: on level02 the two `Rol_OilFire` lights (which
  author `specular 1.5 0.5 0` in `oilfire.gsh`) crossed `SetLight` at scales 0.53533, 0.52733 and
  0.50000 — `1 - n/254` for n = 118, 120 and 127, the last being the fully-fogged cap — and the
  scale changed when a defogging unit moved away with nothing else about the light changing. The
  full evidence and the on-screen numbers are in `vulkan_renderer_notes.md` §4.90.
- **A particle emitter attaches a dynamic light through `SceneLightSet_AddDynamicLight`**
  @ 0x0057a040 — `__thiscall`, `RET 0x1c`, i.e. seven stack dwords *plus* the receiver. All five
  call sites load `ECX` from `[0x007c18cc]`, so it is a method on the scene light set and not the
  free function its decompiled call in `ParticleEmitter_Ctor` looks like. It builds a 0x6c-byte
  light record — two 16-byte values evaluated out of a pair of `PGenChannel`s, a position, a colour
  lazily unpacked from the `LightInfo` at 0x006ab218, a lifetime, and two terms derived from that
  lifetime — and installs it through vtable slot 5. The 0x6c matches what `LightSet_Reset`
  @ 0x0057a780 frees, which is what the name rests on.
- **`LightSet_SetEmissiveColour` @ 0x00579ef0 sets no ambient light**, despite ~90 call sites across
  the HUD, menus, console and world effects and despite `src/World.h` still wrapping it as
  `SetAmbientLight`. It writes `this+0x48` — `D3DMATERIAL8.Emissive`, the material starting at
  +0x18 — and `this+0x24`, `Diffuse.a`; if `this == CurrentLightSet` it re-pushes via
  `SetD3DMaterial`, otherwise the colour reaches D3D lazily through `LightSet_Apply`. There is no
  ambient term anywhere in it. (`src/World.h` is not this file's to fix; the header's `AwColour`
  layout and its "flags == 2 means the four floats are authoritative" rule are both correct.)

### `Light`, `SceneLight`, and the single `SetLight` call site

A `Light` is **0x6c bytes and is a `D3DLIGHT8`**: `{+0x00 vptr, +0x04 D3DLIGHT8}`, with nothing else
in it. (In Ghidra the type is `AwLight`, not `Light`: the root-category `Light` there is the
0x1c-byte record `ToLight` @ 0x0047e220 builds from the GLS `light` section, and Ghidra resolves a
`__thiscall` class name against the type manager by name alone — so a class called `Light` would
have silently typed every method here with the 28-byte GLS struct. `src/Render.h` and this file call
it `Light`; the functions are `AwLight::SetPosition` and so on.)

Vtable **0x0066ccc4**, three slots, and **no virtual destructor** —
`SetPosition3f(float,float,float)` 0x00579920, `SetPosition(const Vec3 *)` 0x005798f0,
`SetSpecular(D3DCOLORVALUE)` 0x00579950. The layout is pinned five independent ways, of which the
blunt one is `Light::SetAll` @ 0x00579a00: a `REP MOVSD` with `ECX = 0x1a`, i.e. 26 dwords = 0x68 =
`sizeof(D3DLIGHT8)`, into `this+4`. Derived field offsets from the object base: `Type` +0x04,
`Diffuse` +0x08, `Specular` +0x18, `Ambient` +0x28, `Position` +0x38, `Direction` +0x44, `Range`
+0x50, `Falloff` +0x54, `Atten0/1/2` +0x58/+0x5c/+0x60, `Theta` +0x64, `Phi` +0x68.

A `SceneLight` is 0x90 and derives from it — vtable **0x00663e4c**, four slots: 0 and 1 inherited,
**2 overridden** by `SetSpecular` 0x00488200, **3 new** = `SetFogScale` 0x00488250. Its extra fields
are the authored specular colour at +0x6c, sixteen bytes at +0x7c with **no reader or writer
anywhere in `.text`**, and `float fog_scale` at +0x8c, initialised to 1.0f at 0x004887b1. Only
`SceneLightSet::AddLight` builds one, so only the +0x5c list holds them — which is what makes the
slot-3 call in `SelectLightsForBounds` safe on a 3-slot base vtable.

**`Light_Apply` @ 0x00579840 is the only `IDirect3DDevice8::SetLight` call site in `gl.exe`.** Its
whole body is `SetLight(index, (D3DLIGHT8 *)((char *)this + 4))` followed by
`LightEnable(index, TRUE)` — no marshalling, no copy, the light object's own bytes. Verified by
scanning all 676,244 `.text` instructions for `CALL [reg+0xb0|0xb4|0xb8]`: seven hits — one
`SetLight` (0x00579858), three `LightEnable` (0x004885fb, 0x0057a907, 0x0057a95c), two `GetLight`
(0x0057a8a1, and 0x004b9fc7 which is a false positive — a `__thiscall` virtual on a `Unit`). Three
call sites reach it: 0x004885c7 (the fog path), 0x0057a8e1 (`LightSet_Apply`) and 0x0057aade
(`LightSet_AddLight`).

That is worth stating in those terms because of what it buys: **wrapping `Direct3DCreate8` is a
complete description of Gunlok's lighting.** Everything the fixed-function pipeline is ever told
about a light crosses that one instruction, which is why GkPlus's Vulkan renderer reproduces the
fog-of-war dimming exactly without implementing any of it — the attenuated `D3DLIGHT8` simply
arrives at `SetLight` already scaled.

Device vtable offsets confirmed along the way, by argument counts and by `SetD3DMaterial`'s
`LEA [ECX+0x18]`: **+0xa8** `SetMaterial`, **+0xac** `GetMaterial`, **+0xb0** `SetLight`, **+0xb4**
`GetLight`, **+0xb8** `LightEnable`, **+0xc8** `SetRenderState`. Note that **+0x88 is `BeginScene`,
not `SetLight`**.

`ReadBackD3DLightState` @ 0x0057a850 is the seventh hit and is dead: it reads the material and every
light slot back into stack locals, discards all of it, and has no callers at all.

### The per-`DrawItem` callback pair

`RenderQueue_Submit`'s 9th argument lands at `DrawItem+0x2c`. If non-null, `DrawItemList_Render`
calls its vtable slot 1 (+0x04) immediately **before** `DrawItem_RenderGeometry` and slot 2 (+0x08)
immediately **after**, both with `this` = the `Renderable`. This is an engine mechanism in active
use: of the 102 submit sites, **56 pass null and 46 pass an object** — 20 of
`UpdateAndDrawMenuScreen`'s 29, plus sites in the HUD (`HudItem_DrawByKind`), the world-effects pass
(`DrawWorldEffects`) and `DrawInventoryScreen`.

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
 │    Font_FlushQueuedText 0x00578180 / Font_RenderTextItem 0x00578a00 /
 │    Font_EmitGlyphQuad   0x005792d0            text, see 4.2
 │    Hud2D_DrawQuad 0x005695c0 / 0x00569b10 /
 │    Hud2D_FlushBatch 0x00569ed0            the HUD's 2D quads, see 4.4
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
  inside it. That is why the hit list contains things like `Actor::Update` (slot 70) and
  `ApplyUpdateMessage`. A displacement-only scan is not a call-site scan; intersect it with the
  device global.
- **Two of the four take a user pointer**, so for the shadow, scanner, level-load-overlay and
  world-effect draws there is no `IDirect3DVertexBuffer8` in existence at any point. Any scheme that
  captures geometry by intercepting `CreateVertexBuffer`/`Lock` silently misses all of them.
- **`ShadowRenderer_Quality23` has no call edge at all.** Its only two references are DATA refs
  inside `ApplyShadowQuality` (0x0054fc1e, 0x0054fc44), so a caller walk reports it as a root and a
  reachability query says nothing reaches it. This is CLAUDE.md's function-pointer trap in its
  purest form.

A `RenderBatch` auto-flushes at 0x800 vertices / 0xc00 indices; `Font_EmitGlyphQuad` @ 0x005792d0,
the glyph quad emitter, is the clearest read on the structure (`+0x2c` D3DPRIMITIVETYPE, `+0x50`
vertex count, `+0x54` index count, and it emits 4 verts / 6 indices per glyph).

## 4.2 Text is a third queue, drained once a frame

Text has **its own retained path**, parallel to the render queue and invisible to it. The function
that every producer calls does not draw:

```
producers (39 call sites)
  └─ Font_QueueText 0x005782e0   lays the text out, appends a List_Member<TextDrawItem>
                                 to font+0xb08. Touches no device, no vertex buffer,
                                 and not CurrentCamera.

RenderSceneAndPresent 0x00574ccd
  └─ ScenePass_Overlay2D  0x00578ee0   walks the font registry List at DAT_007c14a0
       └─ Font_FlushQueuedText 0x00578180   per font: pop, render, free item.text, unlink
            └─ Font_RenderTextItem 0x00578a00   per item; breaks the batch when item.target
                 │                              differs from font+0xb04 (camera switch)
                 └─ Font_EmitGlyphQuad 0x005792d0   per glyph
            └─ RenderBatch_Draw(4, 1)
```

Consequences worth carrying:

- **Layout happens at submit time, rasterization at flush time.** Wrapping, scrolling, clipping and
  the `'?'` substitution are all done inside `Font_QueueText`; the item carries pixel coordinates and
  a plain substring. A hook that wants the *string* belongs on `Font_QueueText`; one that wants the
  *geometry* belongs on `Font_EmitGlyphQuad`.
- **`ScenePass_Overlay2D` is the seam** — one call, 14 instructions, and it is the only caller of
  `Font_FlushQueuedText`. Suppressing every text overlay in the frame is one detour.
- **There are four fonts**, constructed by `InitConsole` via `Font_Ctor` @ 0x00577c70 and destroyed
  by `Font_Dtor` @ 0x005780d0 (which is a **tail `JMP`**, not a `RET` — it has no epilogue of its
  own). Each registers itself into the `DAT_007c14a0` list; the flush is per font, in queue order.
  They are `SmallFont` / `LargeFont` / `HudSmallFont` / `HeadingFont` @ 0x007b6a54/58/5c/60, and
  three of those names are corrections — see `address_map.md`, which had three of the four filed as
  console fonts when only `SmallFont` is one.
- **A font's size is not its line height.** All four are constructed with `line_height = 25`;
  `Font_GetNormalizedLineHeight` returns `line_height * scale / ResolutionHeightF`, and `scale`
  (+0xaf4) is 1.0 out of the constructor with **exactly one other writer in the binary** —
  `ScaleFontsForClientWidth` @ 0x004d79f0, which sets it to 2.0/2.5/3.0 for `HeadingFont` alone.
  That same function also rewrites every font's *advance* table per client width, so glyph metrics
  are resolution-dependent in a way the constructor's arguments do not show. `LargeFont` and
  `HeadingFont` are otherwise the same font: same texture, same widths, same line height.
- `Font_QueueText`'s `depth` parameter is a 0..1 fraction lerped between the target camera's two z
  planes to produce the vertex z, with `rhw = k/z` — so text participates in depth when
  `target[+0x250]` is set, and is flat at z=0 otherwise.

GkPlus's mirror of this is `src/Font.h/cpp` (`GetFont` / `LineHeight` / `QueueText`, and the
`text` JS namespace). Two things it does not pass through: it truncates at 1027 characters, because
past that the engine smashes its own stack, and it normalises `max_chars <= 0` to the whole string,
because the engine's clamp is an unsigned compare where a negative reads as "no limit". The
ownership contract is measured — `Font_QueueText` copies the text into its own pool block
(`malloc(strlen+1)` + inline `strcpy` @ 0x005787f4), so a caller may pass a temporary or a literal;
`target` is the one argument stored raw and read again at flush time.

`Font_QueueText`'s ten parameters, the nine `TextFlags` bits and the 0x28-byte `TextDrawItem` are
all modelled in the Ghidra DB (enum `TextFlags`, struct `TextDrawItem`) with plate comments; the
`Font` layout is measured but deliberately **not** typed, since it is 0xb18+ and reaches many
functions. The one defect on this path is `Font_QueueText`'s unbounded copy of the caller's string
into a 1028-byte frame buffer — `game_defects_notes.md` §1.

## 4.3 A fourth path that draws nothing at all: the software transform, and mouse picking

There is one more way geometry reaches D3D, and it is the odd one out because **nothing is ever
drawn from it**. `Aw_ProcessVertices` @ 0x005a3fa0 turns `D3DRS_SOFTWAREVERTEXPROCESSING` on, binds
a source vertex buffer, and calls the device's `ProcessVertices` to have the runtime transform the
vertices into screen space (`D3DFVF_XYZRHW`) in a **`D3DPOOL_SYSTEMMEM`** destination. The game then
locks that destination `D3DLOCK_READONLY` and reads the transformed positions back with the CPU.

It is the **mouse-picking hit test** — the "what is the cursor over" query behind unit selection.
Two passes: `Picking_TestNodeBoundingBox` @ 0x005a7930 projects a node's 8 bounding-box corners once
per drawn item, and if any picker survives `Picking_PointInProjectedBox` @ 0x005a73c0, `SceneMesh_Render`
projects the entire mesh — up to 10,000 vertices — for a per-triangle test. `ParticleSystem_Render`
uses the same machinery to project particle positions.

Three things about it are worth knowing before hooking anything on the buffer path:

- **A `SetStreamSource` from this path is never followed by a `Draw*`.** Only the *source* of a
  `ProcessVertices` is bound; the destination is passed as `pDestBuffer` and nothing else. That is
  why the FVF census over `SetVertexShader` contains 0x002/0x112/0x152/0x1c4/0x212/0x252 and
  **never 0x004** — a layout that exists only as transform output.
- **It runs at full rate on a completely still camera**, because the camera not moving does not stop
  the cursor being somewhere. On level02 that is ~28 read-back locks a frame, ~126,600 vertices.
- **From a `Lock`/`Unlock` hook, a read is indistinguishable from a refill.** The Vulkan renderer's
  capture layer converted and uploaded all of it for three sections before anyone asked why; the
  flags are the only thing that distinguishes them. `vulkan_renderer_notes.md` §4.84 is the whole
  story, and `address_map.md` has the scratch-set globals and the `VertexBufferSet` API.

## 4.4 2D layer order is a camera per depth slice, and the HUD's meters miss theirs

**There is no per-draw "layer" anywhere in the render queue.** What decides whether a 2D element
lands in front of another is *which camera it was drawn with*. Every camera owns a `D3DVIEWPORT8`
at `+0x254`, and `InitRenderCameras` @ 0x004af4d0 (called from `WinMain` and from `LoadLevel`)
carves the depth range into slices, one camera per slice:

| camera | global | MinZ..MaxZ | near/far |
|---|---|---|---|
| `Camera_Menu2D` | 0x007f5c10 | 0.00 .. 0.02 | — |
| `Camera_Text` | 0x007b5800 | 0.02 .. 0.04 | 0 / 10 |
| `Camera_Hud` | 0x007b4e40 | **0.03 .. 0.04** | 0 / 10 |
| (unidentified) | 0x007b4930 | 0.06 .. 0.30 | 0 / 10 |
| (unidentified) | 0x007b5320 | 0.02 .. 0.03 | 0 / 10 |
| `Camera_World` | 0x007b4ba0 | **0.10 .. 1.00** | 1 / 200 |
| the sky/backdrop camera | 0x007b5a70 | 1.00 .. 1.00 | 1 / 1000 |
| (unidentified) | 0x007b50b0 | 0.02 .. 0.04 | 0 / 10 |
| (unidentified) | 0x007b5590 | 0.04 .. 0.06 | 0 / 10 |

`Camera_SetDeviceViewport` @ 0x00577490 is the **only** `SetViewport` in the binary. The canonical
four-step switch — used verbatim by `RenderHudItems`, `DrawOrderMenu`, `RenderSceneAndPresent` and
`DrawItemList_Render` @ 0x005a875f — is: write `CurrentCamera` @ 0x007c146c; call
`Camera_ApplyViewportAndZFunc` @ 0x00577550 (the viewport, plus `D3DRS_ZFUNC` from `cam+0x1d0`);
copy `cam+0x250` into `CurrentCameraIsPerspective` @ 0x007c1470; call `Camera_Apply` @ 0x005774c0
(the three `SetTransform`s, nothing else). `DrawItemList_Render` runs it whenever the next
`DrawItem`'s camera (`DrawItem+0x18`) differs from the current one, which is what makes the queue's
sort respect the slices.

**Every camera's ZFUNC is `D3DCMP_LESSEQUAL`** (`Camera_Ctor` @ 0x00576470 writes 4 to `+0x1cc` and
`+0x1d0`; `Camera_SetZFunc` @ 0x00576b50 has zero call sites and nothing else writes them). So a
depth *tie* goes to whichever draw is issued **later**, which matters whenever two 2D elements are
authored at the same z.

All the HUD cameras are **orthographic** — `InitRenderCameras` passes every one of them except
`Camera_World` and the sky camera through `Camera_SetOrthographic` @ 0x004b04e0, which clears
`+0x250`. That is what makes §4.2's glyph emitter take its `z = 0` branch: `Font_EmitGlyphQuad`
only computes `MinZ + t * (MaxZ - MinZ)` for a perspective camera, and otherwise emits `z = 0`,
`rhw = 1`, which D3D then clamps up to the slice's `MinZ`.

### The HUD's meters are drawn under the world's slice

`game_defects_notes.md` §12 is the defect; the part worth carrying here is the shape of it, because
it is what the two paths cost. The HUD's panel plates are *retained* — all 11 `RenderQueue_Submit`
sites in `HudItem_DrawByKind` @ 0x0055fbd0 pass `Camera_Hud`, so they get the 0.03..0.04 slice for
free when the queue drains. The meters and item icons are *immediate*: `Hud2D_DrawQuad` @ 0x005695c0
appends them to the shared batch with an authored `z = 0.03f`, and a batch has no camera — it is
drawn under whatever viewport happens to be current at the single `Hud2D_FlushBatch`
@ 0x00569ed0, which `RunInGameFrame` reaches one instruction after `DrawOrderMenu` has switched
back to `Camera_World`.

**So a batch's depth is decided by frame position, not by the values in it**, and the two halves of
one HUD element can end up in different slices without anything in either path looking wrong. It
is the same hazard §4.2 records for text and §4.1 for the shadow draws: a path that bypasses the
queue also bypasses the queue's only mechanism for ordering.

## 5. The producers

All 31 of them, with what they draw. The evidence for the UI ones is the **localized string ids they
fetch** — `glreseng.dll` carries 1527 strings and `GetResourceString`'s argument names the screen
outright, which is far more reliable than reading the drawing code. (Parse the string table straight
out of the DLL's `RT_STRING` resources; string *id* N lives in block `N/16 + 1` at index `N % 16`.)

| Producer | Address | Sites | Reached from |
|---|---|---:|---|
| `UpdateAndDrawMenuScreen` | 0x004ea8e0 | 29 | the front-end frame `RunFrontEndFrame` |
| `DrawWorldEffects` | 0x005201c0 | 15 | `RunInGameFrame` **and** `ScenePass_WorldEffects` |
| `HudItem_DrawByKind` | 0x0055fbd0 | 11 | `HudItem_Draw` @ 0x0056a7b0 |
| `DrawInventoryScreen` | 0x0049f4f0 | 6 | `RunInGameFrame` |
| `Unit_Draw` | 0x004b6ae0 | 4 | Unit vtable slot 68 |
| `DrawInventoryItemPanel` | 0x004a7890 | 3 | `DrawInventoryScreen` |
| `Unit_DrawWithTeamState` | 0x004be830 | 3 | Unit vtable slot 68 |
| `DrawItemValueBrackets` | 0x004fa930 | 3 | `UpdateAndDrawMenuScreen` |
| `DrawItemLabelBrackets` | 0x004fb500 | 3 | `UpdateAndDrawMenuScreen` |
| `DrawOrderMenu` | 0x00498610 | 2 | `RunInGameFrame` |
| `DrawInventoryFrame` | 0x0049eba0 | 2 | `UpdateReconCamera` |
| `InGameMenuWidget_Draw` | 0x0056c3c0 | 2 | widget vtable slot 2 |
| `SubmitAndFlushMapGeometry` | 0x004720a0 | 1 | `RunInGameFrame` |
| `DrawCtfFlagMarker` / `…2` | 0x004963b0 / 0x004963f0 | 1 each | `DrawUnits` / `Unit_DrawWithTeamState` |
| `SubmitSkyDome` | 0x004a0ed0 | 1 | `UpdateReconCamera`, `UpdateSelectedUnitCamera` |
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
| `DrawTeamSelectOverlay` | 0x00566bd0 | 1 | `DrawInGameOverlay` |
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
  no-op stub, quality 1 `ShadowRenderer_Quality1`, quality 2 and 3 `ShadowRenderer_Quality23` (3 additionally
  bakes static shadows). So **the shadow setting changes which functions are producers at all.**
- **The inventory screen replaces the world rather than overlaying it.** `RunInGameFrame` branches
  on `InventoryScreenOpen` @ 0x007b6e50 — which is a **`Renderable *`, not a flag**: it holds the
  screen's model, built by `Renderable_CtorCopy` in `OpenUpgradeScreen` @ 0x004e5ad0 — and on
  `InGameInterface` @ 0x007b3f38 `+0x60`; when the screen
  is up, `DrawInventoryScreen` runs *instead of* `DrawWorldEffects` +
  `SubmitAndFlushMapGeometry` + `DrawUnits`. Anything counting draws per frame sees the world
  disappear entirely, which is correct behaviour and not a dropped hook.
- **`RunGameFrame` is a producer too, and a bulky one.** Before it calls `RenderSceneAndPresent` it
  builds ~22 UI sprites through `UiSprite_Ctor` and submits each with `DrawUiSprite`. The submit
  phase is therefore split across `RunInGameFrame` *and* `RunGameFrame`, not confined to the former.

Two producers keep a hedge in their names: `DrawWorldEffect_Unknown` (which effect it draws is not
established) and `DrawTeamSelectOverlay` (named from its neighbours — the least-verified name here).

## 5.1 The client `Unit` hierarchy: sixteen classes, and the base is 92 slots

The biggest producer dispatches through these, so their **bounds** are load-bearing: read a slot
off the wrong table and you get a function from the next class down. Sixteen adjacent tables in
`.rdata`, each start and each length fixed by the reference test — a vtable start is referenced
from `.text` by its class's ctor/dtor, an interior slot by nothing.

All sixteen classes are now named, and the tree is a **structural mirror of the executor `Actor`
tree**: the same sixteen classes with the same edges. Sizes come from **vtable slot 35, which is
`GetSize()`** — a two-instruction `MOV EAX,<imm32>; RET` — and each is independently corroborated
by the highest field its constructor writes.

```
Unit                                    0x006647ac  92  0x130  ctor 0x004b4620  dtor 0x004b5640
 ├── MobileUnit                         0x0066491c 107  0x238  ctor 0x004ba050  dtor 0x004ba630
 │    ├── CharacterUnit                 0x00664ac8 112  0x2e0  ctor 0x004c1100  dtor 0x004c1300
 │    │    ├── CentibodyUnit            0x006656f0 112  0x2e8  ctor 0x004cbb90  dtor 0x004cbbd0
 │    │    │    └── CentipedeUnit       0x006658b0 112  0x2e8  ctor 0x004cbc70  dtor 0x004cbca0
 │    │    └── PopupUnit                0x00665a70 112  0x2e8  ctor 0x004cc4f0  dtor 0x004cc530
 │    │         └── TurretUnit          0x00665c30 112  0x2f0  ctor 0x004cc9a0  dtor 0x004cca10
 │    ├── NodeUnit                      0x00665544 107  0x240  ctor 0x004cbb30  dtor 0x004cbb60
 │    └── PresidentUnit                 0x00665f60 108  0x248  ctor 0x004cdfe0  dtor 0x004ce060
 ├── ProjectileUnit                     0x00664c88  94  0x180  ctor 0x004c47e0  dtor 0x004c4a50
 ├── PickupUnit                         0x00664e00  93  0x150  ctor 0x004c6d70  dtor 0x004c6e60
 ├── TrackObjectUnit                    0x00664f74  92  0x1d0  ctor 0x004c6f50  dtor 0x004c70b0
 ├── TumbleweedUnit                     0x006650e4  92  0x148  ctor 0x004c8390  dtor 0x004c84b0
 ├── BackgroundCreatureUnit             0x00665254  94  0x178  ctor 0x004c9730  dtor 0x004c9af0
 │    └── FlyingBackgroundCreatureUnit  0x006653cc  94  0x190  ctor 0x004cb530  dtor 0x004cb590
 └── BlockerUnit                        0x00665df0  92  0x140  ctor 0x004cd570  dtor 0x004cd620
```

Three independent methods agree on that shape:

1. **`CreateUnit` @ 0x004fd450's `role->ai` dispatch** — a 21-entry jump table at 0x004fd8d4
   (`CMP EAX,0x14`), which matches the `AIType` enum mirrored at `src/Roles.h:21` value for value.
2. **The constructor call edges.** Each ctor writes exactly **one** vtable pointer at `this+0x00`,
   so it is single inheritance throughout with no multiple inheritance anywhere: 0x004ba0ad→
   0x004b4620, 0x004c113a/0x004cbb49/0x004ce018→0x004ba050, 0x004cbba9/0x004cc509→0x004c1100,
   0x004cbc89→0x004cbb90, 0x004cc9b9→0x004cc4f0, 0x004cb549→0x004c9730, 0x004cd5aa→0x004b4620,
   and the remaining five call 0x004b4620 directly.
3. **A 15-wide RTTI predicate ladder in slots 36–50** — one slot per derived class,
   `XOR AL,AL; RET` in the base and `MOV AL,0x1; RET` (bytes `B0 01 C3`) in the owning class. This
   is the client twin of `src/ActorClasses.inc.h`'s `Predicate` column, and the index→predicate
   mapping is **the same as the executor tree's**: 36 `IsMobile`, 37 `IsCharacter`,
   38 `IsProjectile`, 39 `IsTrackObject`, 40 `IsNode`, 41 `IsCentipede`, 42 `IsCentibody`,
   43 `IsBackgroundCreature`, 44 `IsFlyingBackgroundCreature`, 45 `IsPickup`, 46 `IsTumbleweed`,
   47 `IsPopup`, 48 `IsBlocker`, 49 `IsPresident`, 50 `IsTurret`. Each class's TRUE-set is
   therefore a fingerprint, and it is what pins ownership of every other slot.

**Sizes and non-ladder slot indices remain non-comparable across the two trees.** The ladder is the
one thing that maps across, because both trees number the same fifteen predicates the same way.
Everything else does not: client `PresidentUnit` is 0x248 against executor `PresidentActor` 0x240,
and client `BlockerUnit` is 0x140 against `BlockerActor` 0x130. Same class names, different objects.

Twelve of the sixteen were raw undefined bytes until this run was defined, so any slot count taken
from the tables before then was a guess at bytes rather than a measurement — **no table in the run
is shorter than 92**. Two things pin the numbers:

- **The last table is bounded by the reference test, not by adjacency.** 0x00665f60 has no
  successor vtable to end it; the first referenced dword past it belongs to the string
  `"blobarrel"`, which puts the table at 108 slots. This is the same trap that once undercounted
  `PresidentActor` by 12 slots (`actor_vtable_notes.md`) — "it ends where the next vtable starts"
  is not a bound when there is no next vtable.
- **The base is 92 slots, so an offset of 0x170 or more read off 0x006647ac is already inside the
  *next* class's table.** That is not hypothetical: the shipped names `Unit_IsConcealed`
  @ 0x004cfe70 and `Unit_SetConcealed` @ 0x004cf5d0 were filed as slots 100 and 101 by exactly that
  arithmetic. They are slots **8 and 9**.

Slot 68 (`Draw`) is `Unit_Draw` @ 0x004b6ae0 / `Unit_DrawWithTeamState` @ 0x004be830 /
`TumbleweedUnit::Draw` @ 0x004c96e0 / `PopupUnit::Draw` @ 0x004cc7d0 in every table, which is the
alignment cross-check that the starts above are right. The other identified slots, with the owner
now taken from the ladder rather than from where a body happens to sit:

| slot | +off | base implementation | owner of the override | meaning |
|---|---|---|---|---|
| 8 | 0x20 | 0x004cfe60 — `XOR AL,AL; RET` | `MobileUnit` — `Unit_IsConcealed` @ 0x004cfe70 | concealment flag getter |
| 9 | 0x24 | 0x004cf5c0 — bare `RET 0x4` | `MobileUnit` — `Unit_SetConcealed` @ 0x004cf5d0 | its setter |
| 33 | 0x84 | `Unit_SetTeam` @ 0x004cf3b0 — `*(int *)(this+0xb4) = arg; RET 0x4` | `MobileUnit` (`Unit_SetTeamWithInventory`), `PresidentUnit` — 3 bodies | `SetTeam` |
| 35 | 0x8c | `Unit::GetSize` @ 0x004cf650 — `MOV EAX,0x130; RET` | one stub per class, all 16 distinct | **`GetSize()`** — the size oracle for this tree |
| 36–50 | 0x90–0xc8 | `XOR AL,AL; RET` | one class each | the RTTI predicate ladder above |
| 51 | 0xcc | `Unit_EnterWorld` @ 0x004b57c0 | **15 distinct bodies** — every class but `TurretUnit`, which inherits `PopupUnit`'s | ends with `UnitList_Add` @ 0x004d0310 |
| 55 | 0xdc | 0x004cf5b0 — bare `RET` stub | `MobileUnit` — `Unit_Dissociate` @ 0x004bc740 | `Dissociate` |
| 57 | 0xe4 | `Unit_Update` @ 0x004b5d50, `RET 0xc` | **14 distinct bodies**; `BlockerUnit` keeps the base one and `NodeUnit` keeps `MobileUnit`'s | the per-tick `Update` |
| 67 | 0x10c | `Unit_ComputeLodLevel` @ 0x004b6930 (all sixteen tables) | — | the LOD level, below |
| 68 | 0x110 | four distinct bodies across sixteen classes | `MobileUnit`, `TumbleweedUnit`, `PopupUnit` | `Draw` — the biggest producer |
| 80 | 0x140 | 0x004cff60 — bare `RET 0x4` stub | `CharacterUnit` — `Unit_SendThrowDecoy` @ 0x004c4040 | throw a decoy at a ground position (command `0x2b`) |
| 91 | 0x16c | `Unit_LeaveWorld` @ 0x004b8c20 | four classes, each chaining back | the inverse of slot 51; `UnitList_Remove` @ 0x004d0380 |

Three of those rows correct earlier readings. Slots **8 and 9** are `MobileUnit`'s overrides, not
base methods — the base's own bodies are the two stubs at 0x004cfe60/0x004cf5c0; the slot numbers
were already right and only the owning class was wrong. Slot **80** is not an unidentified
move-to-position variant: it is the decoy throw, gated in its *caller* (§ below, and
`gadgets_notes.md`). Slot **91** is a base `Unit` slot — the base table's last — and it is
`Unit_LeaveWorld`, a chained "leave world" teardown; it is **not** `Dissociate`, which is slot 55.
All eight DATA references to `Unit_Dissociate` @ 0x004bc740 sit at `table+0xdc`, never at `+0x16c`.

Slot 91's overrides each tail-chain to the base body: `MobileUnit::LeaveWorld` @ 0x004c0f80 (in
eight tables, and it ends `JMP 0x004b8c20` @ 0x004c10f3 with no `RET` of its own),
`ProjectileUnit::LeaveWorld` @ 0x004c4c20, `BackgroundCreatureUnit::LeaveWorld` @ 0x004cb510 and
`BlockerUnit::Unblock` @ 0x004cdf10. Its two callers are `Unit_Destroy` @ 0x004b6530 (the slot 63
base body, in 11 tables, reached from update `0x49`) and `Unit_UpdateMovement` @ 0x004bc0ef, which
latches a deferred remove-me on `+0x1a8`.

Ownership of the *order-sending* slots also moved a level up, and `orders_notes.md` §8 has the
corrected version: slots **100–104** (`Unit_SendInteract`, `Unit_SendEquip`, `Unit_SendDropItem`,
`Unit_SendBoard`, `Unit_SendUseItem`) are added by `MobileUnit` @ 0x0066491c, not by
`CharacterUnit`. `MobileUnit` also adds 92 `Unit_IsCrouched` and 93
`Unit_SetCrouchedAndConcealed`, and overrides 33, 55, 57, 73, 77, 83 and 91; `CharacterUnit`
overrides base slots 74, 75, 76, 78, 79, 80 and adds 108–111.

**Slot 67 is the model's detail level**, and it is the client's whole LOD policy in one function.
It is `int __thiscall(Unit *)`, bare `RET`, and it occupies slot 67 in all sixteen tables, so it
belongs to the base `Unit`:

```
base    = ModelDetailLevelBase @ 0x007b9c78          ; written once by LoadLevel @ 0x004e0d0d
maxDist = role->+0x1c ? *(int *)(role->+0x1c + 0x44) : 20000
          and * 1.4f when this->team (+0xb4) is neither 0 nor 2
dist    = |Camera_World position (0x007b4d7c) - this->+0x98|, via RsqrtMantissaTable @ 0x007fef80
return clamp(dist * 1000.0f * (float)[0x007b4e34] / 0.4f > maxDist ? base + 2 : base, 0, 9)
```

Read that off the **disassembly**: the decompiled C drops the `COMISS` and shows the distance being
discarded, so it reads as if nothing depends on the camera. The `+2` is the only distance term —
there is no per-mesh LOD curve, just "near" and "two levels coarser". `ModelDetailLevelBase` has
exactly two references in the image, the `LoadLevel` write and this read. There is one virtual call
site in the `Unit` tree, inside `Unit_Draw` @ 0x004b707d; a displacement scan for `+0x10c` also
finds `MineDetonate` and three others, but those are **`Actor`**-tree objects and a different slot
67 — the two trees' slot numbers are not comparable (CLAUDE.md, Analysis Traps).

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
- `SceneNode::name` (+0x11c) is the string `SceneNode_GetWorldPositionByName` @ 0x0059ab20
  matches case-insensitively — the same name a RIF `OBJHIERD` binding carries. That function
  **returns a `bool`, not a node**: on a match it builds the transform for the given animation time
  and writes the translation column (`m[0x0c]`, `m[0x1c]`, `m[0x2c]`) into a caller-supplied
  `Vec3 *`, recursing over children and post-multiplying on the way back up. It is
  `__thiscall bool(SceneNode *, Vec3 *out, const char *name, int anim_time)`, `RET 0xc`, and
  `Renderable_GetNodeWorldPosition` @ 0x0059d270 is the five-instruction thunk that reaches it
  through `Renderable::root_node` (+0x17c). `src/Render.h` still calls it `SceneNode_FindByName`,
  which describes a lookup it does not perform — it never returns the node. `SceneNode::textures` (+0x9c) is the list a
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
- (`LightSet` vtable slots +0x08 and +0x14 were listed here: they are `LightSet_RemoveLight`
  @ 0x0057a6d0 and `LightSet_AddLight` @ 0x0057aa20, both `RET 0x4`, and both are described in the
  `LightSet` section above.)
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
  written by `BuildShapeVertexBuffers` and `SceneNode_BindHierarchyNode`, neither of which is dissected here.
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
- The `Unit` hierarchy's remaining slots. Its sixteen vtables and their real bounds are §5.1;
  what is identified there is slots 8, 9, 33, 51, 55, 57, 67 and 68, out of 92 on the base.
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
