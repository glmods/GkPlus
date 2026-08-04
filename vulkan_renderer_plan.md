# Vulkan renderer: status and next steps

The working plan for the bindless Vulkan renderer. **`vulkan_renderer_notes.md` is the design
record and the evidence**; this file is what to do next and how to pick it up. Nothing here
restates a measurement — every claim of fact points at a section there instead, so there is
one place to correct when something turns out wrong.

## Where it stands

**`GKPLUS_RENDERER=vulkan` draws the game.** The world geometry, the textures, the lightmaps, the
units, the HUD, the **fixed-function lighting** and the **stencil shadows** all render through
Vulkan, and **every draw the game issues reaches the renderer**. Against the **real D3D8** on a
settled, paused level02 frame the whole frame is **0.13/255**, against a cross-launch d3d8-vs-d3d8
floor of **0.034** — and **93% of the frame is bit-identical** (§4.38).

**Compare against `GKPLUS_RENDERER=d3d8`, not d3d9** (§4.33). Windows 10 still ships a 32-bit
`d3d8.dll` in SysWOW64, so that mode runs the game on the original runtime with the capture layer
and the whole REPL harness intact — which is what makes the frame alignable, and a reference you
cannot drive is one you cannot align. `d3d9` is now the *second opinion*: it says whether a
difference lives in the translation layer or in the game. Thirty sections were measured against
d3d9 before anyone checked whether the real thing was on the machine.

No missing *feature* is known, and the 2.59 that stood for six sections is fixed:

- **It was one defect and it was a size mismatch** (§4.37, fixed in §4.38). Gunlok renders into a
  **640x480** backbuffer; the window's client area, and so the swapchain, is **628x468**. A
  pre-transformed draw's pixels-to-clip matrix is built from the D3D viewport, so a Vulkan
  viewport covering the swapchain scaled every 2D draw by 628/640 *during rasterisation* —
  resampling the texture, so no sample landed on a texel. The renderer now rasterises into an
  offscreen target at the backbuffer size and blits at the end, which is the order the original
  does it in. Worth **2.55/255 over 65% of the frame** on one paused frame; `render.offscreen` is
  the A/B, and off against the original reproduces 2.593 exactly.
- **The A4R4G4B4 probe quad is bit-exact** — 0.0000 MAD, 100.0%, from §4.36's 4.00 and 31.6%. The
  CPU expansion was never wrong, so matching D3D's own expansion arithmetic and mapping the format
  natively are both off the correctness list; native mapping is now only a memory question.
- **The HUD panel is 0.000 against the original**, from 1.26 — the open item §4.28 left.
- **Edge fringes** on every silhouette, and both halves are now accounted for. D3D8/9 sample a
  pixel at its integer coordinate where Vulkan samples at the centre, so every interpolated
  value was **half a pixel out** in every frame this renderer had drawn (§4.28), worth 1.34/255.
  That fix is confirmed correct against the original — 2.90 with it, 5.07 without (§4.33). What
  looked like "genuine filtering" afterwards was the resampling above, and what is left on a
  silhouette is the *game*: 93% of the pixels differing between d3d8 and vulkan also differ
  between two d3d8 launches, in the same bounding box — the two characters idle-animate (§4.38).
- **The effect layers are fixed, and it was one defect** (§4.42). The lattice rectangle over
  level02's fire, §4.29's "two bright HUD columns", and the objectives text rendering as garbage
  were all one event: a shared 64 KB dynamic vertex buffer refilled ~5 times a frame, whose arena
  slot was overwritten by the **second of two consecutive refills**. The freeze test asked "have
  there been draws since the last rewrite" where it had to ask "has any draw this frame read the
  slot" — and `draws_this_frame_` is zeroed by the first rewrite, so the second read 0 and wrote
  the slot on top of the version an earlier draw was still pointing at. Draw 222 is a HUD panel
  and it was rendering the fire's glow quad.

  Three earlier readings said that draw was correct and all of them were taken **a frame or more
  after it was issued** — see the readback rule below, which is the part to carry forward.

Five measurement rules, each of which cost a wrong conclusion. Read them before trusting an older
number:

- **A deferred readback proves consistency, not correctness** (§4.42). `verify_buffers`,
  `verify_textures` and both of `draw_geometry`'s original columns read *now*, and now is a frame
  or more after the draw they describe — long enough for the game to refill a dynamic buffer, and
  long enough for a wrapper to have been destroyed and its slot handed to another buffer (8,543
  vertex buffers created against 333 live). So the arena and the game's buffer can agree perfectly
  on a version neither held when the draw was issued, which is exactly how the plate quad survived
  "0 of 12 vertices differ". Anything the game rewrites within a frame has to be read **at the
  draw**: `render.draw_geometry` now does both and prints them side by side.
- **A feature is judged on its region, never on a whole-frame MAD** (§4.27).
- **...but mean RGB per region is blind to a real difference** (§4.33). It cancels a per-texel
  error with zero bias — the reported junk pile matched d3d8 to 0.1 mean RGB while differing by
  2.95 MAD against a 0.008 floor. Read both, against a floor; the real reference is what supplies
  one.
- **Count distinct values, not differences, when a texture looks wrong** (§4.37). Sixteen values
  all multiples of 17 means "one exact 4-bit texel per pixel, no filtering"; 256 means
  "resampled". That settled what three sections of difference images could not.
- **A cross-launch comparison is reproducible to 0.09** on a pinned frame, and **an amplified
  difference image cannot tell a sub-pixel offset from filtering** — resample one against the
  other and find where the difference minimises (§4.28). Region MADs still carry a per-launch
  spread of order 1 because the animation phase is not pinned (§4.31).

**Every "must be 0" counter reads zero for a draw that was never offered.** `DrawPrimitive` built
no draw for the whole life of this renderer with every skip counter clean, because they count
*reasons a draw was rejected* (§4.32). `draw calls seen` against `submitted` in `render.draws` is
the only reading that compares against what the **game** did rather than what the renderer chose —
check it before believing a frame is complete.

**Fog is not on that list, and that is a measurement**: `D3DRS_FOGENABLE` is 0 for every draw of
a session, and the cavern's fog is a texture stage rather than D3D fog (§4.19, reconfirmed at
0 of 12,045,221 draws in §4.25). What players read as fog was the light sum, and it is in.

**The light sum is the whole equation, not a special case** (§4.26): ambient, diffuse and
specular over directional, point and spot lights, with range, the three attenuation
coefficients, the spot cone, and each material colour tracked from whichever
`D3DRS_*MATERIALSOURCE` names it. §4.20's HUD collapse is subsumed — it is what the same
equation produces with no light enabled — and is kept only as `render.lighting = false`, so the
feature can be A/B'd inside one paused frame.

Two counters here were wrong and are worth knowing about before trusting a number:
`lit_draws_with_lights` read a frozen **845** because its FVF test preceded its light test; it
now reads 796,297 of 819,653. And the first `lit_draws_wanting_colour2` tested a render state
that is set on every draw, so it counted all of them. Both are §4.26.

The renderer replaces Gunlok's D3D8 usage from *behind `Direct3DCreate8`*, which GkPlus already
owns. It is two translation units: `src/D3D8Capture.cpp` is the **recorder** and
`src/D3D8CaptureReport.cpp` the **evidence** — the histograms, verifiers and `render.*` readings,
which were a third of one 5,000-line file and are on no frame's path. `src/D3D8CaptureInternal.h`
is the seam; nothing else may include it. The recorder is a
**state recorder, not a translation layer**: it mirrors the
fixed-function state, replays state blocks into that mirror, reduces each draw to a `DrawItem`,
and still forwards every call to d3d8to9 so the A/B stays available.

| | state |
|---|---|
| Seam chosen and proven total | ✅ `rendering_notes.md` §4.1, notes §1 |
| Vulkan device, 32-bit, all bindless features present | ✅ §4.3 |
| Swapchain on the game window, ImGui overlay on it | ✅ §4.4, §4.5 |
| Validation layers (self-built, 32-bit) | ✅ §4.6 — clean, and proven able to fail |
| Shadow state + state-block replay | ✅ §4.7 |
| Buffer residency (live, not cumulative) | ✅ §4.8 |
| Geometry uploaded into VMA arenas | ✅ §4.9 |
| One canonical 48-byte vertex format | ✅ §4.10 |
| Textures + surfaces wrapped, pixel path settled | ✅ §4.11, §4.12 |
| Texture pixels in `VkImage`s, content-verified by readback | ✅ §4.13, §4.24 — 158/158 mip levels |
| Buffer contents verified by readback (`render.verify_buffers()`) | ✅ §4.24 — 3467/3467 |
| Every image named by its `.rim` asset | ✅ §4.14 — 52/53 |
| Bindless descriptor array + samplers | ✅ §4.15 — 4096 slots, 5 samplers |
| **The world drawn by Vulkan**, textured | ✅ §4.16 — A/B matches d3d9 |
| RenderDoc capture from the REPL | ✅ §4.17 |
| **User-pointer draws** — text, particles, menus, units | ✅ §4.18, and §4.22 for the scratch's frame skew |
| **The second texture stage** — the lightmap, which was the whole flat-and-bright gap | ✅ §4.19 |
| **Pipeline buckets** (blend, depth, cull) and the alpha test | ✅ §4.19 — 5 pipelines |
| **The HUD's colour** — the material emissive, on vertices with no colour | ✅ §4.20 |
| **Every buffer resident**, including ones unlocked before the renderer existed | ✅ §4.20 |
| Line lists and triangle strips | ✅ §4.21, §4.27 — **on by default** now that stencil exists |
| **Upload ordering** — two copies to one arena slot in one batch | ✅ §4.24 |
| Fog | — measured absent, §4.19 and §4.25; nothing to do |
| **The real light sum** — the whole D3D8 equation, per vertex | ✅ §4.26 — within 1/255 on four of six regions |
| **`GpuDrawRecord`** — per-draw data in an array, push constants 120 → 72 bytes | ✅ §4.26 |
| **Stencil shadows** — depth format with a stencil aspect, stencil in `PipelineState` | ✅ §4.27 — the shadow region matches d3d9 to 0.5/255 |
| **The D3D9 pixel centre** — a half-pixel viewport origin | ✅ §4.28 — 4.02 → 2.68 over the whole frame |
| **The five sampler-state defaults**, and `D3DTEXF_NONE` as a LOD clamp | ✅ §4.28 |
| **`GpuMaterial`** — the texture stages in an interned per-frame table, push constants 72 → 48 bytes | ✅ §4.30 — 274 draws are 29 materials, and the frame is bit-identical |
| **The material key on the `.rim` name**, not the wrapper pointer | ✅ §4.30 — it predicts the table to within 2 |
| **`D3DRS_SHADEMODE`** — a flat copy of both colours, selected per material | ✅ §4.31 — 2% of draws, all of them the stencil shadow, 0 pixels changed |
| **`DrawPrimitive`** — the fourth draw entry point, which built no DrawItem at all | ✅ §4.32 — and `seen`/`submitted` now reconcile, which is what would have caught it |
| **The viewport's depth slice** — `D3DVIEWPORT8::MinZ`/`MaxZ`, six of them and never the default | ✅ §4.32 — the effect layers are no longer occluded by the world |
| The effect layers still being dimmer than d3d9 | ✅ **found** — not dimmer, *substituted*: a shared dynamic vertex buffer's arena slot was overwritten by the second of two consecutive refills, so a HUD draw rendered the fire's glow quad (§4.42). §4.29's HUD columns are the same event |
| **Bisecting the REFERENCE** — `render.ref_range` / `ref_hide` / `frame_draws`, working in `d3d8` mode | ✅ §4.42 — the reading that was missing on the reference side for three sections |
| **Compare against the original**, not d3d8to9 — `GKPLUS_RENDERER=d3d8` | ✅ §4.33 — d3d8 vs d3d9 is 0.017-0.051 on a settled level02 frame, which is the cross-launch floor, so the two references agree and the residual was 2.59 against the real thing |
| **The last residual against the original** — 2.9 on an oblique decal, and what a player sees as "the junk pile looks wrong" | ✅ **found**: the game renders 640x480 into a 628x468 swapchain, so every 2D draw was rasterised 2% small and resampled (§4.37). Not mips (§4.34), not alignment or alpha (§4.35), and A4R4G4B4 was the contrast agent rather than the cause (§4.36) |
| **An offscreen colour target at the backbuffer size, blitted to the swapchain** — the fix for it | ✅ §4.38 — whole frame 2.593 → **0.13** against a 0.034 cross-launch floor, **93% of it bit-identical**, the HUD panel 0.000 and the A4R4G4B4 probe quad **100% bit-exact** |
| **The overlay in its own pass on the swapchain**, so it stays 1:1 with the window | ✅ §4.38 |

Steady state on level01, in level, under validation:

```
draws: 370 this frame / 662 peak
skipped: 0 topology, 0 arena slot, 0 no transform, 0 unconvertible, 0 scratch full, 0 no record
lit draws: 1405937 (1360542 with a light on, 0 want COLOR2, 0 before any material, 0 lights dropped)
buffers seeded from their own contents: 9 (0 refused by pool, 0 read failures)
depth format: 130 = D24_UNORM_S8_UINT (with stencil)
stencil draws: 22302 (0 with no stencil buffer)
pipelines: 11 (0 failures)
stages: 0 unimplemented ops, 0 needing more than two, 0 bound textures unresolved
vertex: 10239 KB live / 32768 KB     index: 583 / 8192 KB     slots live: 3469
images: 56 live / 62 created, 112 MB      bindless: 4096 slots, 5 samplers
scratch: 1087 KB vtx + 84 KB idx peak, plus 185 KB draw records + 18 KB lights
render.verify_buffers() 3468/3469     render.verify_textures() 158/158
validation errors: 0
```

Two of those changed with §4.27 and are not regressions: **11 pipelines**, because the five
stencil fields are part of the key, and **0 topology skips**, because the strips and line lists
are drawn now. **3468/3469 is the reading, not 3469** — one pre-transformed vertex buffer that
the game refills while the verifier reads it, stable across repeated calls and identical on every
build back to §4.29, so it is the instrument rather than an upload defect (§4.27, §4.31). This
block said 3469 until §4.31 measured it on two builds.

The same on **level02**, which is the level an automated run should be loading, so this is the
block to check against after a change rather than the one above. The **ratio** is the invariant,
not the totals:

```
swapchain: 628x468, 3 images     rendering at: 640x480 offscreen, scaled at present (nearest)
draws: 274 this frame / 279 peak
draw calls seen: 128470   submitted: 128470   unaccounted for: 0   <- the reconciliation, §4.32
skipped: 0 topology, 0 arena slot, 0 no transform, 0 unconvertible, 0 scratch full, 0 no record
lit draws: 121951 (118532 with a light on, 0 want COLOR2, 0 before any material, 0 lights dropped)
depth format: 130 = D24_UNORM_S8_UINT (with stencil)   stencil draws: 2712 (0 with no buffer)
materials: 29 this frame / 30 peak (0 dropped)   2712 flat-shaded draws (all of them the shadow)
viewport depth-slice changes: 5165                 <- the six slices, §4.32; 0 would be wrong
backbuffer: 640x480   distinct viewport rects ever set: 1 (0,0 640x480)   <- §4.38 assumes both
pipelines: 9 (0 failures)
stages: 0 unimplemented ops, 0 needing more than two, 0 bound textures unresolved
render.verify_buffers() 2952-2953/2953     render.verify_textures() 292/292
validation errors: 0
```

**`verify_buffers` reads 2952/2953 on level02, and since §4.42 it says why.** The odd buffer is the
shared pre-transformed one (fvf 0x1c4, refilled about five times a frame) whose arena slot is
*deliberately* frozen for the rest of the frame once a draw reads it — so the verifier is comparing
a frozen slot against a buffer that has moved on, and a match would be the surprise. It prints
`(EXPECTED: the slot is frozen for this frame and the newer version is in the scratch)` and skips
its re-upload experiment for that case. Earlier revisions of this block called it an instrument
artefact and read the alternation as the evidence for that, which was the right shape and the wrong
reason.

The cumulative figures scale with how long the run sat there; the **ratios and the zeros** are the
invariant. `seen == submitted` and `unaccounted for: 0` are the two that say the frame is whole —
they are what §4.32 added, and no other counter here can see a draw that was never offered.

## Four things that shape everything downstream

Read these before changing the draw path; each cost real time to establish.

- **The seam is `Direct3DCreate8`, not the AWAPI render queue** (notes §1, `rendering_notes.md`
  §4.1). The queue looked total and is not.
- **A buffer's arena slot holds one version, and the game uses more than one per frame**
  (§4.23, §4.42). One shared 64 KB dynamic buffer is refilled about **five** times a frame with
  plain `NOSYSLOCK` locks over the same bytes, so the later versions are parked in the frame's
  scratch and a draw names its vertex and index source separately. **Once any draw this frame has
  read the slot, the slot belongs to it for the rest of the frame** — the test is `drawn_frame_ ==
  frames` alone, and gating it on "draws since the last rewrite" instead let two consecutive
  refills put the second straight back into the live slot, which is §4.42's whole defect.
  `unversioned_rewrites` must stay 0, and note that it stayed 0 throughout that defect.
- **The scratch belongs to the SCENE, not to a frame in flight** (§4.22). The game writes it
  before Present, so it rotates at the *bottom* of `DrawFrame` and there is one more slice than
  there are frames. Resetting it at the top — where the fence wait is — made every user-pointer
  draw read the previous scene's slice, and a static scene hides that completely.
- **One canonical 48-byte vertex** (§4.10), which is why there is one arena, one vertex shader
  and no vertex input state at all — a draw binds nothing and pulls by device address. The
  arena therefore aligns slots to `sizeof(CanonicalVertex)`, **not** 16; getting that wrong
  silently pulls the wrong vertices and is invisible to every counter (§4.16).
- **Texture pixels arrive by `CopyRects`, and the mirror reads the destination afterwards**
  (§4.12, §4.13) — *not* by replaying the source rectangles, which was the earlier conclusion
  and was wrong.
- **`TextureImage::index` is the bindless descriptor index** (§4.15). There is no second
  mapping to keep in step, and a descriptor is written once at image creation.

## Next

In order, most visible first.

1. **The material *override*, now that there is a table to override** (§5). `GpuMaterial` is
   interned per frame from the D3D state (§4.30), so a mod hook is a rewrite of one entry rather
   than a per-draw interception — and the key is the `.rim` name, which is the identity a mod can
   write down. This is the first thing the bindless shape was for, and the first item here that
   adds something the game never had.

   **This is now the top item because nothing visible is known to be wrong.** §4.42 closed the
   last one; if a new one is reported, the repro below is the shape to reach for.

2. **Re-audit what else a deferred readback has been vouching for** (§4.42). `verify_buffers` and
   `verify_textures` both read *now* and compare against what the game holds *now*, which is a
   weaker claim than either has been read as. Textures are safe — they do not move — but the
   buffer half cannot distinguish "the arena is wrong" from "the buffer moved on", and that gap
   hid the plate quad through three sections. The at-draw columns in `render.draw_geometry` are
   the pattern; nothing else has been converted to it.

3. **A frames-in-flight question the fix does not answer.** The slot is now frozen for the rest of
   the frame once a draw reads it, and released at the next frame's first refill — but frame N's
   command buffer is submitted at Present N and executes asynchronously, so frame N+1's first
   upload into that slot races frame N's GPU read of it. Not observed (a paused frame writes the
   same bytes every time, so a race there is invisible by construction) and not measured. The
   reading that would settle it is an at-draw arena read on a frame that is *moving* —
   `fx.snow(true)` is the generator the plan already recommends for exactly this class.

**Closed by §4.38 and not worth reopening**: the 2.59 residual, the HUD's remaining difference
(§4.28, now 0.000), the "edge fringes" on silhouettes (what is left of them is the game's own
animation phase, shared with two d3d8 launches), and the 0.60 floor the DXT1 probe quad showed
(§4.36, now 0.0000). Already ruled out before that: mip selection (§4.34), sub-pixel alignment
(§4.35 — the resample sweep minimises at exactly (0,0)), the A4R4G4B4 expansion (`r | (r << 4)`
is correct, and §4.38's bit-exact probe settles it), and alpha (§4.36, bit-identical). **Mapping
A4R4G4B4 natively is now a memory question only**, not a correctness one.

Two things deliberately deferred and still deferred: the `RenderQueue_Submit` seam as
*enrichment* (bounding spheres and LOD for culling — notes §1, "it demotes"), and widening
`GpuMaterial` past two stages, which §4.7 measured is enough for level01 and level02 and which
`truncated_stages` will announce if a level needs more. Neither blocks anything.

## Working on this

```bash
cmake --build build && cmake --build build --target copy
```

The copy **fails silently while an instance holds the DLL** — check the deployed file's
timestamp, not the build's. That invalidated a whole bisect once (§4.8).

Shaders are **Slang**, compiled offline; the generated header is checked in so `d3d8.dll` needs
no shader toolchain. After editing `src/shaders/*.slang`:

```bash
python3 src/gen-shaders.py
```

Testing is the REPL, driven from PowerShell. `utils/rendertest/launch-gunlok.ps1`
handles the three things that otherwise waste a run — `-skipfmv`, the modal "Run in a window?"
dialog that blocks *before* the REPL listener opens, and the foreground-lock dance needed to
focus the window. If it is gone, notes §4.4 and §4.6 have the details to rebuild it.
`shot-gunlok.ps1` beside it captures the window with `PrintWindow`, which is what makes a
change judgeable at all — see the warning below. `shoot-settled.ps1` builds on both and is what a
comparison should actually use; `find-draw.ps1` bisects for the draw behind a pixel.

**Kill `WerFault.exe` before the copy, not just `gl.exe`.** WER holds the crashed process's
handle to `d3d8.dll`, so `--target copy` fails with "Permission denied" long after the game is
gone — and the plan's older advice about the copy failing silently applies to the *game* holding
it, which is a different case with a different symptom.

**Load `level02`, not `level01`, for anything automated.** `level01.gcs` ends in
`PLAY CUTSCENE first contact`, so a scripted run is shooting a camera sequence rather than the
level, and which frame of it depends on how quickly the machine got there — the one thing a
screenshot comparison cannot tolerate. `level02.gcs` plays no cutscene:

```
levels.start({script: "level02.gls", console: "level02.gcs"})
```

Every "measured on level01" figure below stays as it is — those record what was run, and a
level01 measurement is only reproducible against level01. Use level01 when reproducing one,
level02 when starting fresh.

| variable | effect |
|---|---|
| `GKPLUS_RENDERER=vulkan` | Vulkan owns the window |
| `GKPLUS_RENDERER=d3d8` | **the ground truth** — forwards to Windows' own 32-bit `d3d8.dll` in SysWOW64, with the capture layer and the whole REPL harness still in place (§4.33). No ImGui overlay: there is no D3D9 device behind it. This is what a difference should be judged against |
| `GKPLUS_RENDERER=d3d9` | d3d8to9, the default and the second opinion — it says whether a difference is in the translation layer or in the game |
| `GKPLUS_VK_VALIDATION=1` | plus `VK_ADD_LAYER_PATH` at the self-built 32-bit layer (§4.6) |
| `GKPLUS_RENDER_UNFOCUSED=1` | keep rendering while the window is inactive (§4.2) |
| `GKPLUS_WRAP_BUFFERS` | `none`/`vb`/`ib`/`both` — bisecting the buffer wrappers. **Did nothing until §4.13**: `ReadWrapMode()` was defined and never called, so any earlier bisect that used it proved nothing |
| `GKPLUS_TEXTURE_UPLOAD` | `both`/`seed`/`blits` — the same bisect for the two halves of the texture upload |
| `GKPLUS_RENDERDOC` | load RenderDoc so `render.capture()` works (§4.17). Must be set at launch — the layer cannot be added after the instance exists |
| `GKPLUS_RENDERDOC_DLL` | override the path; the default is the **x86** build, not the one beside the UI |
| `GKPLUS_VK_HEAPS=small` | arenas and rings cut to just above level01's peaks (82 MB → 24 MB). **Set this for any in-level RenderDoc capture** — at full size 15 resources are captured uninitialised because RenderDoc's readback allocations do not fit in gl.exe's 2 GB (§4.17). It changes staging behaviour, so it is not implied by `GKPLUS_RENDERDOC` |
| `GKPLUS_NO_LIGHTING=1` | force `D3DRS_LIGHTING` off in the **forwarded** call only (§4.19) |
| `GKPLUS_NO_STAGE1=1` | force `D3DTSS_COLOROP` to `DISABLE` on every stage past the first, likewise forwarded only (§4.19) |
| `GKPLUS_NO_SPECULAR=1` | the same for `D3DRS_SPECULARENABLE`. Changes nothing on level01, which is how vertex specular was ruled out (§4.20) |
| `GKPLUS_NO_MIPMAP=1` | forwarded only: `D3DTSS_MIPFILTER` to `D3DTEXF_NONE`, so the reference samples level 0 whatever the footprint (§4.29) |
| `GKPLUS_NO_CULL=1` / `GKPLUS_NO_ZTEST=1` / `GKPLUS_NO_ATEST=1` / `GKPLUS_NO_BLEND=1` | forwarded only: `D3DRS_CULLMODE` to NONE, `D3DRS_ZENABLE` off, `D3DRS_ALPHATESTENABLE` off, `D3DRS_ALPHABLENDENABLE` off — the four states that make a draw vanish outright, or stop vanishing, rather than come out wrong (§4.29, §4.39, §4.40). A draw whose fragments are all discarded is indistinguishable from one that was never issued until you switch the test off in the **reference** and watch it appear. `NO_BLEND` is the fourth and it questions this layer's own mirror: forcing blending off in the original collapses its fire into hard orange blobs and dark rectangles — the same artefact class this renderer produces — which is what proved the layer covering the backdrop is the game's blended fire (§4.40) |
| `GKPLUS_VK_TOPOLOGIES` | **On by default since §4.27.** The variable now selects a *subset*: `none`/`0` for none, `strip`/`line` to bisect the two, `all`/`1` (or unset) for both. Also settable at run time as `render.topologies`, which is what makes them A/B-able on one paused frame |
| `render.lighting` | run-time only, on by default: the real light sum, or the §4.20 material collapse the build before it used. **The way to measure lighting** — toggle it on a paused frame and the difference image is exactly what it paints, at a 0.00 noise floor (§4.26) |
| `render.half_pixel` | run-time only, on by default: the D3D9 pixel-centre convention as a half-pixel viewport origin (§4.28). Off is the pre-§4.28 behaviour, and worth 1.34/255 over the whole frame |
| `GKPLUS_VK_OFFSCREEN=0` / `render.offscreen` | on by default: rasterise the world at the **game's** backbuffer size into an offscreen target and blit it onto the swapchain, rather than drawing straight into the swapchain and letting the viewport scale every 2D draw (§4.37, §4.38). Off is the pre-§4.38 behaviour exactly, and worth **2.55/255 over 65% of the frame**. `render.vulkan_report` says which is running |
| `render.present_linear` | run-time only, **off** by default: the filter for that final scale. NEAREST is a deduction, not a default — the original's own stretch preserves a 4-bit texture's sixteen distinct values, which a filtered downscale could not (§4.37) — and this is the A/B for it |
| `render.probe(name, scale, mipmap, offset, alpha)` | draw one textured quad through the capture device, so d3d8, d3d9 and vulkan all get the same draw with the scene's lighting, stages, blending and depth removed (§4.35). `name` is a substring of a live texture's `.rim` path, `scale` is screen pixels per texel; `render.probe(null)` disarms |
| `render.ref_range` / `render.ref_hide` | **`draw_range`/`draw_hide` for the REFERENCE** (§4.42): a draw outside the range, or inside the hide window, is simply not forwarded. Works in `d3d8` and `d3d9` mode, which is the whole point — for three sections "this renderer draws a quad the original does not" could be established and not followed, because every follow-up question is a `draw_range` question and `draw_range` only existed for us |
| `render.frame_draws([first, last])` | the capture layer's **own** draw list for the last complete frame — index, topology, primitives, FVF, buffered/user-pointer, blend, depth, cull, alpha test, depth slice, stage-0 `.rim` name. Mirror-side, so it works in every mode. It is what `ref_range` is aimed with: **an index does not carry between runs** (aiming at 222 because a `vulkan` session called the quad 222 landed on the HUD portraits), so find the draw by its signature in the mode you are in |
| `render.force_lod` | run-time only, `-1` off: force every texture fetch to an explicit mip level. The probe that ruled mip selection out of the residual (§4.34); pair it with `GKPLUS_NO_MIPMAP=1` on the reference, which pins the original to level 0 |
| `render.shade_mode` | run-time only, on by default: honour `D3DRS_SHADEMODE`, or interpolate everything the way every build before §4.31 did. Worth **0.000** on level01 and level02, because every flat-shaded draw there is the stencil shadow — kept because that is a fact about two levels, not about the game |
| `GKPLUS_VK_SKIP` | switch off this renderer's own features to bisect them: `t` topologies, `s` seeding a buffer from its own contents, `l` the material colour for unlit-vertex draws, `d` the API's initial state defaults |

**The three `GKPLUS_NO_*` switches are the sharpest tool here, and they are not features.** Each
makes the game's *own* renderer draw the scene without one thing the Vulkan path is missing,
which is the only way to tell "we are missing X" apart from "we are wrong in some other way that
happens to look like missing X". They are what proved fog was not the gap and the second texture
stage was, after the plan had said the opposite for three sections — and then that lighting *was*
one after all, on a frame the first A/B had not covered, and then that it was the *whole*
remaining gap (§4.25). Add one per hypothesis rather than reading more of the renderer. All three
modify only what is forwarded, so `render.state` keeps reporting the true state while one is set.

**Compare mean RGB per region, not whole-frame MAD, across two launches.** Two d3d9 runs of
identical code differ by 13.04 MAD on a small crop purely from sub-pixel misalignment, which
buries the effect being measured; the same crop's mean colour matched to 0.2 (§4.25).

**...and read the region MAD as well, against a floor, or a real difference cancels out.** Mean
RGB answers "is this region the right colour" and is blind to a per-texel difference with zero
bias. The junk pile reported from play matches d3d8 to **0.1 mean RGB** and differs by **2.95
MAD** against a d3d8-vs-d3d9 floor of **0.008** — 4,039 pixels brighter and 3,735 darker, which
averages to nothing (§4.33). Having the real reference is what supplies the floor that makes the
MAD readable; there was none against d3d9.

Read results with `render.report` (the D3D capture side), `render.vulkan_report` (device,
swapchain, arenas, images, scratch, bindless), `render.draws` (the draw list and what it
skipped), `render.textures` (every image with its `.rim` name), `render.state` (below),
`render.stats`, `render.vulkan` and `render.validation`.

**`render.draw_geometry`**, after setting `render.draw_state = <index>`, is what a **buffered**
draw actually pulled: the indices and vertices the shader reads out of the arena, beside the ones
D3D holds in the game's own buffer, plus each stage's bound `.rim` name against the name of the
bindless image it samples (§4.40). `verify_buffers` proves a slot holds what its buffer holds and
`draw_info` prints the offsets a draw was given; neither says the draw addressed the right place,
and the arena is one buffer every slot shares — so addressing it wrongly yields *other geometry*
rather than garbage. `render.draw_vertices` is the user-pointer half and cannot see these.

**Read the AT-DRAW columns, not the deferred ones** (§4.42). It prints four: the arena, the game's
buffer under a read-only lock *taken while the draw is being issued*, the arena read back with the
same synchronous `ReadArena` at that instant, and the game's buffer read back later. The last one
is the original and it is the weakest — the game refills a dynamic buffer several times a frame,
so it and the arena can agree perfectly on a version neither held at the draw. `<== STALE` on the
at-draw column is the reading that matters, and the buffer's own bookkeeping is printed above the
table (slot offset, unlocks, which frame drew from it, which frame its scratch version is from),
because that is what decides whether a draw reads the slot or a scratch version.

**`render.verify_state()` and `render.draw_state = <index>`** read the fixed-function state back
off the device and diff it against the shadow mirror — now, or at the moment one draw is issued
(§4.40). Every counter here is computed *from* the mirror, so this is the only reading that can
say the mirror itself is right; §4.39's state-block bug diverged it for a whole scene with every
counter clean. It compares the states and stage states the game has set, the bound textures, the
FVF, the transforms, the viewport, the material, the lights and **the stream bindings**.
`GKPLUS_NO_CULL=1` is its self-test: it must report a CULLMODE mismatch and nothing else.

**`render.draw_vertices = <index>`**, then read it back a frame later, for the converted vertices
and indices one draw was actually handed (§4.32). It is what distinguishes "this draw paints the
wrong shape" from "this draw is fine and something else covers it", and it is user-pointer draws
only — a buffered draw's vertices are in an arena that is never mapped, which is what
`render.verify_buffers()` is for.

**To attribute a pixel to a draw, use `render.draw_hide = [a, b]`** — record everything except
that window — then `render.draw_info(i)` for the culprit's full state. `render.draw_range =
[a, b]` renders only that window, which is how to see what one draw paints against an empty
frame. Two ways to get a wrong answer out of them, both of which did (§4.29): **bisect by hiding,
never by truncating a prefix**, because a prefix truncates the depth and stencil buffers too and a
draw that is merely unoccluded reads as the one that painted the pixel; and **allow ~900 ms
between setting the range and capturing**, or the shot lags one step behind and the bisect
converges neatly on the wrong draw.

**`render.state` is what to read before implementing any fixed-function behaviour.** Every state
the renderer has to reproduce — fog, lighting, blend, depth, stencil, colour write — as it
stands *and* every value it has ever been set to, and then six histograms of what was actually
drawn with, ordered by draw count:

- **texture-stage configurations** and **pipeline configurations**, keyed by FVF so the 2D draws
  can be told from the world's. A stage configuration carries `filt` (mag/min/mip) and `addr`
  (u/v) as well as the ops, which is the only way to attribute a filter to a group of draws —
  the separate per-stage sampler block above it reports every one of those seven states as
  "never set", because Gunlok configures its samplers inside **state blocks** and block replay
  writes the shadow state without going through the recorder (§4.28). Read the live values, not
  the history;
- **draws with no vertex diffuse**, with the material the fixed function colours them from —
  which is how the HUD's green was found (§4.20);
- **draws that are not triangle lists**, described individually down to the screen box their
  vertices cover, their colour, z, rhw and stencil state — four a frame, and how §4.21 was
  settled;
- bound textures that did not reach the shader, and vertex buffers drawn from with no arena slot;
- **the eight states D3D9 does not have**, which is to say the ones d3d8to9 invents a translation
  for and therefore the ones on which the reference can disagree with both this renderer and real
  D3D8 (§4.29). Gunlok leaves `ZBIAS`, `EDGEANTIALIAS`, `ZVISIBLE` and `LINEPATTERN` at 0, and
  **toggles `SOFTWAREVERTEXPROCESSING`, `CLIPPING` and `SHADEMODE`**. `SHADEMODE` is also a
  column of the pipeline histogram below, which is what settled it (§4.31);
- **indexed draws reaching past their bound buffer** — 0, and expected to stay 0. Not one of our
  invariants: D3D8 tolerated it and D3D9 rejects the call, so a non-zero reading would be a defect
  in the *reference*;
- **the backbuffer size and every distinct viewport *rectangle*** (§4.38). The world pass sets one
  viewport over the whole render target, which is right exactly while this reads **one** rectangle
  covering the whole backbuffer — level02 reads `0,0 640x480` against a 640x480 backbuffer. A
  sub-viewport would have to move onto the `DrawItem` beside `min_depth`/`max_depth`, and this
  prints a marker rather than leaving it to be noticed: a wrongly-scaled sub-viewport looks
  exactly like the defect §4.37 spent six sections on.

Twelve stage configurations and twenty pipeline configurations for a level01 session, which is
small enough to implement one by one rather than approximate. That is the whole reason these
print *values* and not, as `PipelineKey()` did for three sections, only how many there are — and
§4.31 is the case that shows why: a *count* of flat-shaded draws says 2% and stops, where the
histogram says all three of the configurations using it are the stencil shadow, which is the fact
that decided how to implement it.

### Comparing against the original

One run per renderer, then a numeric difference. Three-way — `d3d8`, `d3d9`, `vulkan` — is one
extra launch of the same script and is what tells a translation-layer defect from a real one.

**Wait for the camera to stop moving; do not settle for a fixed delay.** The renderers run at
different frame rates, so the same wall-clock delay lands at a different point in any intro
sequence: junkyard at 20 s gave a close-up under Vulkan and a wide shot under d3d8, with the
camera globals reading *identically* a minute later because both eventually settle to the same
place. Polling the camera until it repeats is renderer-independent by construction. Check the
actor count matches too — a level that got further along has different world state, which reads
exactly like a renderer defect (§4.32).

`utils/rendertest/shoot-settled.ps1` is that procedure: `Dismiss-Briefing`,
`Wait-CameraRest`, `Shoot-Settled`. Two things in it are load-bearing:

- **`levels.start` lands on the briefing screen**, and it renders plausibly — a character portrait
  over rock — so a run that misses it looks like a broken renderer rather than a game waiting for
  a keypress. `render.draws` sits at ~4 draws a frame. `Dismiss-Briefing` presses space *until
  `actors.count` is non-zero*, because when the briefing appears depends on how long the load
  took and a single press at a fixed delay lands before it exists about half the time.
- **`level02` is the level to shoot.** It has no cutscene, and all three renderers settle to
  bit-identical camera values and the same 178 actors, which is what makes the comparison mean
  anything. Use `level01` only to reproduce a level01 number.

`find-draw.ps1` beside it binary-searches `render.draw_hide` for the draw that painted a given
pixel.

**Stability is evidence about which defect you have, not whether you have one.** §4.22 needed a
moving scene and vanished on a still one; §4.23 was perfectly stable — 60 consecutive menu
captures were bit-identical with two draws rendering the wrong geometry throughout. Judge the
picture against d3d9, not against the previous frame.

**A paused frame cannot find a per-frame-data defect.** Pausing is what makes a comparison
reproducible, and it is also what hides anything whose failure needs the allocation pattern to
change between frames — §4.22 survived every counter and a "the text is in the right place"
screenshot for exactly that reason. `fx.snow(true)` is the cheap dynamic generator: the particle
count differs every frame, so nothing repeats. Judge the user-pointer path with it on, then pause
to measure.

**Pin the frame, and then two launches are fine.** The renderers run at different frame rates, so
at a fixed delay the *game* is in a different state — three Vulkan runs of identical code at the
same settle differ by up to **8.06/255** (§4.21). That is the game drifting, and all of it goes
away if you stop it drifting: on **level02** (no cutscene), **paused** with
`screen.toggle_pause()`, with the camera set explicitly from the REPL, **d3d9 against d3d9 across
two separate launches is 0.094 over the whole frame and 0.00 on every HUD region** (§4.28). So a
cross-renderer difference above ~0.1 is real and can be read as a whole-frame number.

Set `camera.position`, `yaw`, `pitch`, `roll` and `distance` explicitly rather than trusting the
settle — and read those values back out of the session you are comparing against, because the
same literals replayed later framed something else. `render.draws` collapsing to a couple of
dozen a frame is the tell that the camera went somewhere empty.

Where a feature can be toggled at run time — `render.topologies`, `render.lighting`,
`render.half_pixel`, `render.shade_mode` — that is still sharper: pause and shoot the same frame
twice, at a **0.000** floor, and the difference image is exactly the pixels the feature touched.
It is also the only comparison that can honestly return *zero*: §4.31's shade mode moves no pixel
at all, which no cross-launch measurement could have distinguished from noise.

Four things about the shot itself, each of which invalidated real work before it was found:

- **`PrintWindow` needs `PW_RENDERFULLCONTENT`** — flag 3, i.e. `PW_CLIENTONLY | PW_RENDERFULLCONTENT`.
  With flag 1 alone a Vulkan (or D3D) swapchain window prints **solid black**, because there is no
  WM_PRINT redraw for the compositor to ask for, and a bitmap full of zeros looks exactly like a
  renderer that is not drawing.
- **Call `SetProcessDPIAware()` in the capturing process.** gl.exe is not DPI aware, so
  `GetClientRect` reports 418x312 against a real 628x468 swapchain, and `PrintWindow` renders at
  the window's own resolution — a bitmap sized from that rect silently keeps the top-left two
  thirds. The HUD is in the upper right and was absent from an entire session's screenshots
  (§4.20). A DPI-aware caller asking about a non-aware window gets the physical rect.
- **Never settle for a fixed delay** — poll the camera until it stops. The renderers run at
  different frame rates, so the same wall-clock delay lands at a different point in any intro
  sequence, and the *game* is then in a different state. A ninety-second bisect gave five numbers
  with no pattern and a rock that appeared and vanished between runs of identical code; a
  twenty-second one gave a close-up under one renderer and a wide shot under another. Camera-rest
  is renderer-independent; `Wait-CameraRest` in `shoot-settled.ps1` is it. Check the actor counts
  match as well.
- **The HUD is only up after the intro**, so a HUD comparison needs the camera to have settled
  anyway — which the rest above already gives you. Compare the HUD *region* rather than the frame.

```bash
python3 -c "from PIL import Image, ImageChops; a=Image.open('d3d8.png').convert('RGB'); b=Image.open('vulkan.png').convert('RGB'); d=ImageChops.difference(a,b); px=list(d.getdata()); print(sum(sum(p) for p in px)/(3*len(px)))"
```

**When a textured surface looks wrong, count distinct values before differencing anything**
(§4.37). One line, and it settled what three sections of difference images could not:

```bash
python3 -c "from PIL import Image; im=Image.open('shot.png').convert('RGB').crop((40,80,600,440)); ch=[q[0] for q in im.getdata()]; print(len(set(ch)), sum(1 for v in ch if v%17==0)/len(ch))"
```

16 distinct values all divisible by 17 is a 4-bit channel replicated to 8 bits with **no filtering
at all** — one exact texel per pixel. 256 values is a resampled image. The original gives the
first and this renderer gave the second, which is the whole of §4.37.

Amplify the difference (`d.point(lambda v: min(255, v*4))`) and look at it before theorising:
it is what showed the residual is distance-dependent rather than a flat offset, and per-region
mean RGB is what turned that into the +8 / +13 / +23 numbers in §4.19.

Two checks that go beyond counters, and both exist because counters were not enough:

- **`render.verify_textures()`** reads each image back off the GPU and compares it against the
  D3D texture. Run it with `render.validation` in the same breath — the readback is itself
  Vulkan work, and a broken verifier reports mismatches that are its own (§4.13).
- **`render.capture()`** takes a RenderDoc capture of the next frame, needing
  `GKPLUS_RENDERDOC=1` at launch (§4.17). Two traps, both reported as
  `VK_ERROR_OUT_OF_DEVICE_MEMORY` and neither about VRAM:
  - **the REPLAY process must be 32-bit.** Launching the game from the x86 tooling does *not*
    help — the UI is x64 and replays in its own process. Tools → Manage Remote Servers →
    `localhost`, **set its Run Command** to
    `"C:\Program Files\RenderDoc\x86\renderdoccmd.exe" remoteserver` (empty by default, and
    without it Run Server silently does nothing), start it, then switch the status bar's
    **Replay Context** to `localhost`. Full steps in §4.17;
  - **an in-level capture needs `GKPLUS_VK_HEAPS=small`**, or fifteen resources come back
    uninitialised and show plausible rubbish.

  `renderdoccmd replay` loads a capture headlessly, which is enough to tell a good one from a
  bad one without leaving the loop — and `--remote-host localhost` is the same path the UI takes.

**Some defects only a picture can find, and there is a rule of thumb behind that.** A counter
proves the plumbing ran; a readback proves the right bytes arrived; neither proves a draw
*addressed* them correctly. The arena's slot alignment (§4.16) was invisible to both, and two
matrix-convention guesses were spent before the screenshot was consulted. When geometry looks
wrong, take the screenshot first.

**A counter that says "nothing I was given went wrong" cannot see work that never reached it.**
`DrawPrimitive` built no draw for the whole life of this renderer while every skip counter read 0,
because they all count *reasons a draw was rejected* and that one was never offered (§4.32). The
reading that catches this class is `draw calls seen` against `submitted` in `render.draws` — the
only number here that compares against what the **game** did rather than against what the renderer
chose. Check it before believing a frame is complete.

**Every "must be 0" counter is a real invariant**, not decoration: `foreign_buffers`,
`unconvertible_buffers`, `failed_uploads`, `opaque_block_applies`, `surface_texture_lock_rects`,
`texture_render_targets`, `copy_rects_untracked`, `descriptors_out_of_range`,
`scratch_exhausted`, `dropped_over_capacity`, `unversioned_rewrites`, `dropped_materials`,
`stencil_draws_without_buffer`, `draws_refused`. Each exists because
getting it wrong once was expensive.

**`unversioned_rewrites` is the cautionary one.** It is a real invariant, it read 0 for the whole
life of §4.42's defect, and it was right to: the rewrite that overwrote a live slot was never
*classified* as a rewrite-after-draw, so it never reached the counter that would have refused it.
That is §4.32's lesson one level down — a counter cannot see a case that is filtered out before
it. (`texture_surface_levels` is *not* one of them — it was never a defect counter, and
§4.12 explains what it actually counts.)

For a crash, use `cdb` — path and the `-cf` requirement are in §4.8. WER also leaves a husk of
the crashed process that makes the harness lie; kill `WerFault.exe` first.

## Known gaps

- **The staging ring stalls ~1080 times a session** (§4.13). Wrap safety is real now, but it is
  enforced with a `vkDeviceWaitIdle` rather than per-slot fences, because the renderer's fences
  belong to `VkRenderer`. This was affordable when the Vulkan path only cleared the screen;
  **real geometry is behind it now**, so it is the first performance item to revisit.
- **The vertex arena has no eviction.** Slots are freed when a buffer is released, which is
  correct, but a level with far more geometry than level01 could exhaust 32 MB. `arena_full`
  counts it; nothing recovers from it.
- **1–3 mip levels in 150 drift** (§4.13), always level ≥1 and always small; level 0 matches
  everywhere. `render.verify_textures()` detects it; the writing route is still unidentified.
- **112 MB of images on level01**, roughly half of it the `A4R4G4B4`→`R8G8B8A8` CPU expansion.
  Mapping that format natively would halve it, at the price of a per-device support matrix —
  worth revisiting only if memory ever matters.
- **The per-frame scratch is sized well above what is measured** (§4.18, §4.26, §4.30): 4 MB/1 MB
  against peaks of 1087 KB/84 KB for vertices and indices, 2304 KB/448 KB against 185 KB/18 KB
  for the draw records and lights, and 384 KB against **1 KB** for the materials. Deliberate
  headroom, and the peaks are the numbers to resize by. The record and material slices are sized
  to `kMaxDrawsPerFrame` on purpose, so the draw limit is the only one that can bite — which is
  also what makes `dropped_materials` mean "the scratch is unusable" rather than "a busy frame".
- **A device with no stencil-capable depth format falls back to depth only**, logs a line, and
  draws no shadow mask (§4.27). `render.draws` reports the chosen format and
  `stencil_draws_without_buffer`, so it announces itself rather than looking like a shading bug —
  but nothing has been tested on such a device, because none is known.
- **Specular is computed with the local-viewer form only.** `D3DRS_LOCALVIEWER` is on for every
  level01 draw and the shader assumes it; a level that switches it off would want the
  infinite-viewer eye direction instead. Nothing counts this yet.
- **`D3DRS_SHADEMODE` is honoured, and it has never yet mattered** (§4.31). Every flat-shaded
  draw on level01 and level02 is one of the stencil shadow's three passes — two of which write no
  colour at all and one of which is a single-colour full-screen quad — so toggling
  `render.shade_mode` on a paused frame moves **0 pixels** in either level. Two levels are not
  fifteen; `flat_shaded_draws` in `render.draws` and the `shade` column of `render.state`'s
  pipeline histogram are what would show a level using it for something real.
- **`render.verify_buffers()` reads one short — 2952/2953 on level02, 3468/3469 on level01 — and
  §4.42 is why.** The odd buffer is the shared pre-transformed one (`fvf 0x1c4`, ~5 refills a
  frame), and it **must** differ: its arena slot is deliberately frozen for the rest of the frame
  once a draw reads it, so a verifier comparing that slot against a buffer the game has refilled
  since is asking a question the design has already answered. The report now says so rather than
  leaving it to read as an upload defect, and it no longer runs its re-upload experiment on a
  frozen slot — which would have overwritten the version that frame's draws point at. "A
  pre-transformed buffer the game refills while the verifier reads it" (§4.31) was the right
  shape and the wrong reason.
- ~~The HUD's two bright columns are missing from the d3d9 reference, not from this renderer~~ —
  **closed by §4.42, and it was never about the reference.** The reference painted the columns
  where this renderer painted the fire's glow quad: one draw, one overwritten arena slot, and the
  two symptoms are the same event from either side. §4.28 and §4.29 both concluded "d3d8to9 drops
  them", which was an inference from the columns' absence rather than a measurement of it.
- **Only level01 and level02 have been measured**, plus a little of level03 and junkyard. The FVF
  set, material count, stage count, sampler count and scratch peaks are all "measured there", not
  "proven for every level".
  §4.7's max-2-active-stages in particular is worth re-checking on a level with richer materials
  before shrinking `GpuMaterial`. `level02` is the cheapest second data point — it is the level
  automated runs should be loading anyway (see "Working on this"), so widening these is a matter
  of reading `render.state` on a run that is happening regardless.
- **The overlay is tiny at 4K** (§4.5) — cosmetic, ImGui is drawing 1:1 into the swapchain.
