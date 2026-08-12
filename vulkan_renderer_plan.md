# Vulkan renderer: status and next steps

The working plan for the bindless Vulkan renderer. **`vulkan_renderer_notes.md` is the design
record and the evidence**; this file is what to do next and how to pick it up. Nothing here
restates a measurement — every claim of fact points at a section there instead, so there is
one place to correct when something turns out wrong.

## Where it stands

**`GKPLUS_RENDERER=vulkan` draws the game.** The world geometry, the textures, the **fog of war**,
the units, the HUD, the **fixed-function lighting** and the **stencil shadows** all render through
Vulkan, and **every draw the game issues reaches the renderer**. Against the **real D3D8** on a
settled, paused level02 frame the whole frame is **0.13/255**, against a cross-launch d3d8-vs-d3d8
floor of **0.034** — and **93% of the frame is bit-identical** (§4.38).

**And it now draws something the game never could.** `render.material_override(name, spec)` names a
`.rim` asset and retextures, tints or hides every draw that samples it — a rewrite of one entry in
the per-frame material table rather than a per-draw interception, which is what §4.30's table was
built for (§4.44). Verified on level02 paused: each of the three confined to the player character's
bounding box, and **removing the override restores the frame bit-identically**.

**And it now loads an asset the game never had** (§4.48). A `<texture> lighting.dds` beside a
`.RIM` — in a mod under `gkplus/mods` or in the install — gives that one texture a bump/metallic/
roughness response: **R height, G highlight intensity, B highlight sharpness**, with the normal
derived at draw time from R's gradient against a tangent frame taken from the fragment's own
derivatives, so the canonical 48-byte vertex is unchanged. **The whole interface is the file
name**; nothing registers it. That makes it the first image this side creates, uploads and owns a
bindless slot for — §5's missing half, where the material override could only ever point at a
texture the game had already loaded. Worth **2.00/255 over 22% of the frame** on level02 with a
synthetic map, and `render.lighting_maps = false` restores it **bit-identically**.

Three defaults in it are measurements, not taste, and each would have shipped wrong: every light
reaching level02's ground authors `specular 0 0 0` (so the highlight takes the light's *diffuse*
colour by default), the key light is `diffuse 4.0` (so `specular_scale` defaults to 0.25, its
reciprocal), and a bump that only shapes highlights is invisible wherever metallic is 0 (so the
derived normal reaches the diffuse too, as a ratio).

**And it now has ambient occlusion, with no blur pass** (§4.86). `render.ao` runs the technique
from <https://www.youtube.com/watch?v=vJU1PgGdH3k>: the sample offsets are generated in **2D**, from
one fixed disc shared by every pixel, and what is reconstructed is the 3D position of the *tapped*
pixel — so the kernel needs no per-pixel randomisation, the output is not noise, and nothing has to
blur it afterwards. Worth **0.675 MAD over 21.14% of level02** against an off-vs-off floor of 0.011.
Off by default.

Three things in it are measurements and each would have shipped wrong. **The pattern is a lattice,
not blue noise** — the first revision used Mitchell's best-candidate, which is what a paper would
reach for and is measurably worse at the same count, because a stratified set has no clumps *by
construction* where blue noise only has few on average. **Under-sampling a fixed kernel does not look
like noise, it looks like structure**: each missing tap is a shifted copy of every occluder's
silhouette, visible as a fan of outlines at 32 taps and gone at 64. And **the physically-correct
application point is inert here** — scaling the ambient term reached 0.31% of the frame, because
Gunlok has no ambient to occlude; what the term has to scale is the `STDLIGHT` rig, which is this
game's environment lighting, while D3D's own lights already have shadow maps and are left alone.

**And it now adds geometry, not just shading** (§4.71). `render.tessellation` runs a PN-triangle
amplification pass over the level mesh: the generated points sit on a cubic Bézier patch fitted to
each triangle's corner positions and corner **normals**, whose edge control point collapses to the
linear one exactly when a corner's normal is its face normal — so **a flat wall reproduces itself
at any tessellation factor while a boulder rounds off**, with no threshold and no per-material
opt-in. Off by default, because it changes the level's silhouette rather than reproducing D3D.

The measurement that had to come first is the reusable part: `render.normal_census()` reports how
much of a frame carries smooth normals at all, and it **corrected the design** — only **6.4%** of
level02's map triangles are fully flat, so the construction's free hard-edge identity covers far
less of this game than its reputation suggests, and `render.pn_flat_threshold` exists because of
that number. It stays watertight because the term it thresholds is a function of `(Pi, Pj, Ni)`
alone.

**Compare against `GKPLUS_RENDERER=d3d8`, not d3d9** (§4.33). Windows 10 still ships a 32-bit
`d3d8.dll` in SysWOW64, so that mode runs the game on the original runtime with the capture layer
and the whole REPL harness intact — which is what makes the frame alignable, and a reference you
cannot drive is one you cannot align. `d3d9` is now the *second opinion*: it says whether a
difference lives in the translation layer or in the game. Thirty sections were measured against
d3d9 before anyone checked whether the real thing was on the machine.

**And the upgrade screen is in** (§4.47). A third play report — "the inventory screen fills the
whole window, and the selection rectangles and text are shifted" — and a third real defect:
`D3DVIEWPORT8` has a **rectangle** as well as a depth slice, this layer recorded only the slice,
and the upgrade screen is the one thing in the game that sets a sub-rectangle (`32,24 575x431`
against a 640x480 backbuffer). It goes from **17.23 against the real D3D8 to 0.089**, against a
same-session repeat floor of 0.043. In level it is worth exactly the noise floor, because there
is only ever one rectangle there — which is why it survived this long with every counter clean.
The plan had named the shape of it in advance, next to `distinct viewport rects ever set`, and the
marker fired the first time the screen was opened.

Two API rules came out of `render.viewport_probe`, both measured against the real runtime because
— as in §4.45 — every rectangle Gunlok had ever set was at 0,0, where the readings coincide:
**D3D does not add the rectangle's origin to a pre-transformed vertex** (so `BuildMvp` subtracts
it, cancelling what Vulkan's viewport adds), and **D3D clips to the rectangle** (so it is the
scissor too).

**Level02's fire camera is at the cross-launch floor.** Two play reports in a row — flames that
came and went with distance (§4.45) and a concrete ledge "much redder in Vulkan" (§4.46) — were
two real defects, and with both fixed the whole frame reads **0.522 against a d3d8-vs-d3d8 floor
of 0.521**. The specular term is bit-identical to D3D's over all 104,693 lit pixels.

**And the light sum was wrong in one condition for twenty sections** (§4.46): D3D's specular sum
runs only over lights with `N·L > 0`, and `max(0, N·H)` is not that condition — so every face
turned *away* from a light carried a highlight it should not have. What found it was reading the
three channels rather than a MAD (the excess had **exactly zero blue**, and only the specular
colour does), plus a map showing **zero pixels where d3d8 had specular and we did not** — our set
strictly contained D3D's, which is a missing condition and not a wrong scale.

**One API rule was wrong from the first frame this renderer drew, and play found it** (§4.45):
D3D does not run the viewport's depth range over a pre-transformed vertex — it clamps `z` into
it — so every screen-space draw sat `MinZ * (1 - z)` too far away, and level02's flames came and
went with camera distance. `render.depth_probe` is what settled it, and the lesson is the reason
it exists: **both readings of that rule fit every draw Gunlok issues**, so the answer had to be
asked of D3D directly rather than inferred from the game. Read §4.45 before trusting any
whole-frame number below against a scene with effect layers in it.

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
| **The second texture stage** — which was the whole flat-and-bright gap. §4.19 called it a lightmap; it is the **fog of war** | ✅ §4.19, identified in §4.51 |
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
| **The material override** — retexture, tint or hide by `.rim` name | ✅ §4.44 — the first feature the game never had; a key survives a level reload by re-resolving image 34 → 35, and clearing one restores the frame bit-identically |
| **The specular sum runs only over lights with `N·L > 0`** | ✅ §4.46 — the ledge that was "much redder in Vulkan". Bit-identical specular over all 104,693 lit pixels, and the whole frame at the fire camera drops **2.093 → 0.522 against a 0.521 cross-launch floor** |
| **The viewport's RECTANGLE** — `D3DVIEWPORT8::X/Y/Width/Height`, per draw, as the Vulkan viewport *and* scissor | ✅ §4.47 — the upgrade screen that filled the window. 17.23 → **0.089** against the real D3D8 there, and the noise floor in level |
| **A pre-transformed vertex's depth** — `clamp(z, MinZ, MaxZ)`, not the viewport transform | ✅ §4.45 — the flames that came and went with camera distance. Every screen-space draw sat `MinZ * (1 - z)` too far away; the flame's retention across a distance sweep goes from a 79% → 0.9% collapse to a flat ~92% |
| **Ambient occlusion, with no blur pass** — a fixed lattice disc in screen space, world positions in a prepass | ✅ §4.86 — the first thing here with **no matrix in it at all**: the prepass writes world position per pixel, so the resolve inverts nothing and needs no camera transform, which is the very quantity `BuildSunCascades` records as unavailable on this side. **0.675 MAD over 21.14% of level02** against a 0.011 off-vs-off floor. The disc is the technique author's own **lattice** rather than blue noise, and the tap count is not a quality dial with a cheap end — 32 taps leaves a fan of every silhouette, 64 is smooth. **Off by default** |
| **Geometry amplification** — PN triangles over the level mesh, and over the shadow passes with it | ✅ §4.71 — the first thing here that adds *geometry*. A corner whose normal is its face normal contributes nothing to the patch, so **hard edges stay hard by arithmetic rather than by a heuristic** while a boulder or a pipe rounds off; and the patch is watertight across a shared edge by construction, which is what ruled out height-map displacement. **Off by default.** 1.667 MAD over 51.5% of level02, `pn_strength = 0` at the floor, and the off-state at the cross-launch floor against the pre-change build |
| **Runtime map lighting** — the level's own `STDLIGHT` rig, per pixel, replacing the bake | ✅ §4.53 (loader), §4.54 (the fitted model), §4.55 (the substitution), **on by default in §4.60** — 1.83 ms on the level with 686 of them and nothing measurable on three others |
| **The game's stencil, checked on every level** | ✅ §4.60 — 22 pipeline configurations over **sixteen** levels, **3** with stencil, all three flat-shaded, all three first seen on level01. §4.31's marker is now measured on the whole game rather than on two levels. `railway` is the one that could not be: `levels.start` on it takes gl.exe down inside its own `ConvertParsedObjects` |
| **Frame-uniform data in a buffer**, not in push constants | ✅ §4.57 — the block went 128 → **56** bytes, bit-identical on every static region. The prerequisite phase 4 could not start without |
| **A world-space light grid**, built by the renderer's first compute pass | ✅ §4.56 — culled against brute force is **0.00000 MAD over 0 pixels**, and the grid is built once per level rather than once per frame. §4.60 priced it: **28 ms a frame** on level01 |
| **The light sum, per PIXEL** — the same equation, evaluated per fragment | ✅ §4.52 — 0.48/255 over 26.9% of level02, and `false` is bit-identical to the build before it on every static region |
| **Lighting maps** — `<texture> lighting.dds` beside a `.RIM`, bump/metallic/roughness, loaded from a mod or the install | ✅ §4.48 — the first image this side creates and owns a bindless slot for, which is what §5's "a replacement texture the capture layer never saw" needed. 2.00/255 over 22% of level02 with a synthetic map, and off is bit-identical |
| **The sun casts a shadow** — a depth-only pass over the same draw list, 3x3 PCF | ✅ §4.58 — the first shadow in Gunlok that is not a blob under a unit |
| **Cascades** — four halving boxes in a 2x2 atlas, on the camera's orbit **pivot** | ✅ §4.59 — 0.0085 world units per texel near the pivot against the single map's 0.068, for 1.7 ms. It also found the box had been centred on `CameraFocus`, which is only latched during a `SET CAMERA FOCUS` and is stale in ordinary play |
| **The shadow knobs, swept** — bias in *texels*, and a strength that two levels disagree about | ✅ §4.59 — 2.5 / 0.7 / 70, each with the frame it came from |
| **The map lights' static shadow atlas** — a cube per `STDLIGHT`, baked once per level | ✅ §4.61 — 682 light slots in 32 MB, and it costs **0.50 ms** to sample on the level with 686 of them. Worth **6.9 MAD over 73% of level02**, whose 51 long-range lights reach through every wall, and 0.036 on level04's open terrain. **Off by default**, and that is a fidelity call rather than a cost one |
| **`vkCmdDrawIndexedIndirect` for that bake** — one command a face, `SV_DrawIndex` for the record | ✅ §4.62 — **804,924 draw calls → 4,092**, and level01's whole bake goes from 1.9 s of hitch to *one frame at the steady-state frame time*. The two paths' atlases differ by **0.006 MAD against a same-path floor of 0.010** |
| **The staging ring asks before it blocks** — `vkGetFenceStatus` per live slot, not `vkDeviceWaitIdle` | ✅ §4.63 — **511 stalls → 38** and **778 ms → 122** over thirty seconds of level01 play. It was filed as a level-load problem and is not: a load costs 70 ms and presents nothing |
| **The map light rim** — the fitted falloff's tail, windowed to zero | ✅ §4.64 — play reported "an unnatural disk" around every light, and a scan across it measured a first-derivative jump rather than a step. Refitting the windowed tail over four levels costs **r 0.957 → 0.949** at worst, so the fix is measured against the same bake the model came from |
| **Shadows from the game's OWN point and spot lights** — sixteen slots of §4.61's atlas | ✅ §4.65 — the last light system with no shadow at all. **The measurements dissolved the hard problem**: "a light has no identity across frames" is true, and `render.frame_lights` says a *level's* lights never move, so their contents are the identity. Worth **1.92% of the frame at level02's fire camera** against a 2.34% ceiling, and the bias sweep is flat — real occlusion, not acne. Nothing measurable in frame time. **A light that moves gets no shadow rather than a wrong one** and costs nothing — which is the right behaviour and covers more than it first looked like: an *effect's* light rides a particle, so every explosion, flare and projectile light is in that class. Item 0 of "Next" is what would change it |

Steady state on level01, in level, under validation:

```
draws: 370 this frame / 662 peak
skipped: 0 topology, 0 arena slot, 0 no transform, 0 unconvertible, 0 scratch full, 0 no record
lit draws: 1405937 (1360542 with a light on, 0 want COLOR2, 0 before any material, 0 lights dropped)
buffers seeded from their own contents: 9 (0 refused by pool, 0 read failures)
depth format: 130 = D32_SFLOAT_S8_UINT (with stencil)
stencil draws: 22302 (0 with no stencil buffer)
pipelines: 11 (0 failures)                         <- pre-§4.45; expect more, see the level02 block
stages: 0 unimplemented ops, 0 needing more than two, 0 bound textures unresolved
vertex: 10239 KB live / 32768 KB     index: 583 / 8192 KB     slots live: 3469
images: 56 live / 62 created, 112 MB      bindless: 4096 slots, 5 samplers
scratch: 1087 KB vtx + 84 KB idx peak, plus 185 KB draw records + 18 KB lights
render.verify_buffers() 3468/3469     render.verify_textures() 158/158
validation errors: 0
```

Two of those changed with §4.27 and are not regressions: **11 pipelines**, because the five
stencil fields are part of the key, and **0 topology skips**, because the strips and line lists
are drawn now. That pipeline count has not been re-measured since §4.45 added `depth_clamp` to
the key — level02 is the block to check a change against, and it moved 9 → 12 there. **3468/3469 is the reading, not 3469** — one pre-transformed vertex buffer that
the game refills while the verifier reads it, stable across repeated calls and identical on every
build back to §4.29, so it is the instrument rather than an upload defect (§4.27, §4.31). This
block said 3469 until §4.31 measured it on two builds.

The same on **level02**, which is the level an automated run should be loading, so this is the
block to check against after a change rather than the one above. The **ratio** is the invariant,
not the totals:

```
swapchain: 628x468, 3 images     rendering at: 640x480 offscreen, scaled at present (nearest)
draws: 277 this frame / 286 peak
draw calls seen: 962099   submitted: 961061   unaccounted for: 0   <- the reconciliation, §4.32
skipped: 0 topology, 0 arena slot, 0 no transform, 0 unconvertible, 0 scratch full, 0 no record
lit draws: 920682 (889250 with a light on, 0 want COLOR2, 0 before any material, 0 lights dropped)
depth format: 130 = D32_SFLOAT_S8_UINT (with stencil)   stencil draws: 26772 (0 with no buffer)
materials: 30 this frame / 30 peak (0 dropped)
light sum: per PIXEL
map lighting: 51 lights, gain 1.35, culled by the world grid (1 grid build - one per level)
  static shadows: on, baked 51/51 lights (0 refused, offset 1.00 texels)
sun shadows: on (716 casters over 4 cascades, bias 2.50 texels, strength 0.70, extent 70)
  atlas 4096x4096 (66560 KB, format 126), tile 2048: near cascade +-8.75 units, 0.0085 per texel
local light shadows: on (5 of 5 keys hold a baked cube, 16 reserved slots, 0 still moving)
viewport depth-slice changes: 34333                <- the slices, §4.32; 0 would be wrong
backbuffer: 640x480   distinct viewport rects ever set: 1 (0,0 640x480)   <- one IN LEVEL, §4.47;
                                                          the upgrade screen makes it 2
depth buffer the game asked for: D3DFMT_D24S8 (75)  <- what to match, §4.45
pipelines: 13 (0 failures)
stages: 0 unimplemented ops, 0 needing more than two, 0 bound textures unresolved
lighting maps: 49 images from 49 files (0 refused)
render.verify_buffers() 2952-2953/2953     render.verify_textures() 292-316/same
validation errors: 0
```

Three lines in that block are §4.59/§4.60 and each is easy to misread. **`716 casters` is 179
draws x 4 cascades**, not 716 objects — the shadow pass walks the same list once per cascade.
**`stencil draws` is non-zero only because the sun's map was switched off earlier in that session**;
while it casts, the game's own stencil shadow is dropped and the counter stops moving, which is
also why `flat-shaded draws` no longer prints beside `materials`. And `format 126` is
`D32_SFLOAT` — the shadow atlas deliberately does *not* use the world pass's depth+stencil format,
because nothing in it reads a stencil aspect.

**`depth format: 130` is what `ChooseDepthFormat` picked, not what the game asked for**, and the
two are printed in different reports for that reason. 130 is `D32_SFLOAT_S8_UINT` — the first
candidate, `D24_UNORM_S8_UINT` (129), is not universally supported and is absent on the machine
these numbers come from. The game asks for `D3DFMT_D24S8`; §4.45 measured the two to agree to
`6e-8` near the far plane, which is where everything so far has been compared, and to diverge
near the near plane, which nothing has. **Pipeline count rose 9 → 12 with §4.45** and is not a
regression: `depth_clamp` is part of the key, so the pre-transformed configurations fork.

**`verify_buffers` reads 2952/2953 on level02, and since §4.42 it says why.** The odd buffer is the
shared pre-transformed one (fvf 0x1c4, refilled about five times a frame) whose arena slot is
*deliberately* frozen for the rest of the frame once a draw reads it — so the verifier is comparing
a frozen slot against a buffer that has moved on, and a match would be the surprise. It prints
`(EXPECTED: the slot is frozen for this frame and the newer version is in the scratch)` and skips
its re-upload experiment for that case. Earlier revisions of this block called it an instrument
artefact and read the alternation as the evidence for that, which was the right shape and the wrong
reason.

There is no `material overrides:` line in either block, and that is the reading: `render.draws`
prints it only once something is registered, so `0 registered, 0 matched, 0 overridden, 0 hidden`
on every report would read as an invariant rather than as "nobody asked for anything" (§4.44).

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

0. ~~**Find why the per-frame atlas hangs the GPU**~~ — **done** (§4.66). It is on by default, and
   the bake was never at fault: **it was §4.67's field permutation**, and §4.67's own fix closed it.
   `GpuFrameData` had `light_flags` in a different place in `src/VkDraw.h` and in `world.slang`, so
   the fragment shader's `dyn_shadow_sampler` read the word the CPU fills with `dyn_shadow_offset`
   — a *float*, `DynShadowBiasValue` 1.0f, which is `0x3f800000` = **1,065,353,216** — and indexed
   the bindless `samplers[]` array with it. That array holds five. An out-of-bounds descriptor read
   is a GPU page fault, which is a lost device.

   Two things about that are worth carrying, because they are why it took a section. **The control
   being clean did not exonerate the shader** — the lookup's gate is the per-frame slot, which is
   -1 whenever the flag is false, so the flag was *arming* the fault rather than merely enabling a
   bake. And `render.dynamic_shadow_sample`, the knob written specifically to split "the bake hangs"
   from "sampling hangs", **was itself broken by the same permutation** and never stopped the
   sampling at all — a bisect knob is code, and it can be disabled by the defect it is bisecting
   for. Also: "dies after about three bakes" is at or below the frames-in-flight depth, which means
   the *first* submission hung and the CPU merely ran ahead. That reading alone would have excluded
   all three rings before any of them was written.

   The bake was proved innocent by measurement, not argument, using the caps this item recommended
   — `render.dynamic_shadow_max_lights` / `_max_faces` / `_max_casters` and
   `render.dynamic_shadow_indirect`, all kept. All lights, all faces and **one** caster runs at the
   control's 16.60 ms/frame, which kills the pass-structure hypothesis on its own; the full
   configuration runs at 16.65 over 1,201 frames with `render.validation` `[]`. No RenderDoc capture
   was needed. The rings are all kept — each closed a real defect that would have bitten later, and
   §4.62 carries the map bake's measurement.

   **What it is worth** (§4.66, per-pixel medians over an interleaved sweep — level02 will not hold
   still, and the first single-frame A/B gave an off-vs-off floor larger than the off-vs-on signal).
   At the camera §4.65 named, 14,127 px against a 750 px floor, and **all of it the moving-light
   half**: level02's point lights are at y ≈ -1.75 and its units at y ≈ 4.5, so at rest no unit is
   near a light. Put one there with `light.add` and the other half appears — `map_only` vs all is
   **0.60222 MAD over 81,276 px**, against the map geometry's own 0.24438, i.e. **beside a light the
   mobile casters are worth more than the map is**. Frame cost is nothing measurable: 16.64 ms/frame
   on and 16.64 off, which is the FIFO cap rather than headroom.

   **The class of defect is closed too** (§4.68). `src/gen-shader-abi.py` parses the Slang structs
   and emits `src/ShaderAbi.gen.inc.h` — an `offsetof` assert per field and a `sizeof` per struct,
   for **all twelve** pairs, not just `GpuFrameData`. Generating Slang from the header, which is
   what this item used to propose, is the wrong direction: the shader declares
   `ConstBufferPointer<GpuMapLight>` where the header has `uint64_t`. Confirmed by reintroducing
   the permutation — five errors naming exactly the five fields §4.67 lists.

0a. **Two defects the map bake's ring turned up on its way past** (§4.62, last subsection). Both
   were confirmed against a build with that ring stashed out, so neither is new, and both are
   cheap:

   - ~~**`render.map_shadows` off vs on reads the FLOOR**~~ — **done** (§4.67). Not the flag and
     not the bake: §4.66 added its four `GpuFrameData` words *above* `light_flags` in `src/VkDraw.h`
     and *below* it in `world.slang`, so the shader read `dyn_shadow_texture` — `kNoTexture`, every
     bit set — as the flags, and `local_lights` and `local_shadows` were equally stuck on. A
     permutation preserves the size, so every `static_assert` still passed. 0.00733 MAD before,
     **7.06544 over 3,908,856 px** after, which is §4.62's number recovered.
   - ~~**A forced re-bake clears the atlas and never re-bakes the local half's cubes**~~ — **done**
     (§4.62, last subsection). The `MapShadowBuiltForGeneration != generation` branch wound the map
     half back with `MapShadowCursor = 0` and left the local half alone, so a `LocalShadowKey`
     carrying `baked = true` was never re-queued and its tiles kept the clear value for the rest of
     the level. `RequeueLocalShadows()` beside the cursor reset is the whole fix. Measured on
     level02, before and after in one session: **38,335 static pixels changed by the re-bake
     before, 0 after**, and `local_shadow_report` goes from "cubes baked for this level: 6" on both
     sides of the clear to 6 then 12.

0b. ~~**A transient light casts nothing, and nothing mobile is ever a caster**~~ — **done**
   (§4.66). The two halves §4.65 left, reported from play as "lights from flares, explosions and
   projectiles still don't cast shadows against units and things like barrels". Both were real;
   they are one design, and §4.66 is that design — now on by default and measured. Both halves are
   priced in §4.66's last subsection: at level02's fires the moving-light half is worth 14,127 px
   against a 750 px floor, and beside a light on a unit the mobile-caster half is **0.60222 MAD over
   81,276 px, more than the map geometry's own 0.24438**. The description below is kept because it
   is what the design answers.

   - **A light that moves gets no slot**, which is the stability gate working as designed and at
     zero cost — but §4.65's correction measured what that covers, and it is more than a lift on a
     track. **An explosion's light moves**: it rides a particle, so one `fx.explode` in view leaves
     ~30 distinct contents behind and the census's own metric falls from 2,341 frames a key to
     **9.7**. Every effect light in the game is in this class, and none of them will ever cast out
     of a static atlas.
   - **The caster set is the map object only.** §4.65's cubes come from `IsMapGeometry`, so a fire
     correctly stops at the wall it stands behind and a unit or a barrel beside it throws nothing.
     Props are excluded too, and a crate *is* a real occluder for a runtime light in a way it is
     not for the `STDLIGHT` rig §4.61 inherited that test from.

   **One design covers both, and it is the one §4.65 set aside**: a small **per-frame** atlas,
   rebuilt from the frame's own caster list — map geometry, units and props alike — with no key, no
   cache and no gate, multiplied in beside the static term (`visibility = static * dynamic`, since
   occlusion is a union) or replacing it outright. Sizing is the open question: the frame peak with
   effects running is ~9 point/spot against 5 static, and §4.62 is the reason to think the bake is
   affordable — 4,092 faces over 213 casters each fit in one frame at the steady-state frame time.

   **Every number in §4.65's ceiling table is the static lights' contribution**, because it was
   measured paused. §4.66's own A/Bs are the moving-frame reading for the *shadow* half, and they
   turned up the practical form of the same trap: **level02's fires survive a pause but its
   explosions do not**, and a paused frame at the fires still carries 6 registered lights. What is
   still unread is `render.local_lights` while something is exploding — the ceiling on the *lighting*
   half, which is a different measurement from the one §4.66 took.

   One more thing that run turned up and that a cone projection would need: **a spot light does
   exist** — an effect's, `range 67.81` over 165 draws for one frame. §4.65's six settled cameras
   said there were none anywhere, and they were describing the static set.

0c. **Two things §4.71 left, and one measurement it did not take.**

   - **Displacement from the lighting map's R channel.** §4.48 already documents R as a height
     field, `lightmap/` already generates one, and `GpuMaterial::lighting_texture` already carries
     the slot — so the domain shader can sample it with no new plumbing at all. It was set aside
     rather than rejected: it has a much worse crack problem than PN (two adjacent map triangles
     with different stage-0 textures disagree about the displaced position of their shared edge,
     and there is no adjacency data to fix it with), so it needs measuring on its own rather than
     riding in on a feature whose crack behaviour is provably fine.
   - ~~**The crease case has no measurement.**~~ — **done, and it is not the problem** (§4.74). Run
     after "the tube looks inflated, not rounded" was reported off a screenshot and reproduced on
     level02. A crease guard on the endpoint-normal disagreement and an inflection guard on the two
     tangent terms' signs were both built, both watertight, and both **removed**: at 60 degrees the
     first touches 276 of 9,666 map half-edges and moves **zero pixels** on the pipe, and the second
     fires on 4,764 of them and still moves nothing, because `pn_flat_threshold` has already zeroed
     one side of most of them. **91% of the frame's displacement is on edges whose normals agree
     within 30 degrees**, so no guard aimed at that tail can matter.

     What it is instead: the pipe's whole contribution sits at normalised terms of **0.2–0.35**, a
     23–41 degree turn per edge — a ten- to sixteen-sided tube, exactly the cylinder the normals
     describe. The construction is correct (`pn_strength = -1` pulls *inward*, confirming the sign),
     and it reads as inflation because the pipe is seen near-tangentially, where a displacement `δ`
     along the normal moves the silhouette by `δ / sin(grazing angle)`.

     `render.pn_max_offset` came out of it — a world-unit ceiling on the control point's distance
     from its chord, which `pn_flat_threshold` cannot express because it is normalised by edge
     length by design. It bounds the tail (the census's worst is **1.104 units** against a 1.952-unit
     mean edge) and **does not cure the symptom**: the picture only moves below 0.05, by which point
     most of the tessellation is gone everywhere. The rounding and the inflation are one
     displacement; `pn_strength` is the dial that trades them, and this is a taste setting rather
     than a bug.
   - ~~**The census has only been run on level02.**~~ — **done** (§4.71, "All sixteen levels").
     16/16 via `utils/rendertest/census-levels.ps1`, and it **reversed the claim it was run to
     check**: level02's props are 53.2% curved corners, but over sixteen levels the props are
     **86.7% fully-flat triangles and 7.6% curved**, with 77% of their corners carrying no normal
     at all. `"map"` stays the default, and the `.d.ts` said the opposite for one commit.

     Two more things came out of it. **Level02 is near the *curved* end of the game** — 6.5%
     fully-flat map triangles against a 36.3% aggregate — so it is a pessimistic place to tune
     `pn_flat_threshold` and a safe one to inherit a default from. And **the spread is eightfold**:
     4.3% fully-flat on `level04` against 75.6% on `level15`, mean tangent term 0.034 to 0.283.
     There is no single level mesh to tune against; `prison`, `level04` and `Training_Level` are
     where this feature does the most and `level15` is where it does almost nothing.

     What it did **not** settle: the census reads one frame, so `examined` runs 22 to 208 draws and
     `level15`'s row rests on 8 map draws. A cumulative census — §4.60's trick for the pipeline
     histogram — is what would remove that, and nothing needs it yet.

1. **Where a material override comes FROM** (§5). **Half of this is closed by §4.48** — an image
   this side creates, uploads and owns a bindless slot for now exists, loaded from `src/Vfs` or the
   install by file name, and the lighting maps prove the whole path. What is left is the *override*
   half: the only way to register one is `render.material_override` from a script or the REPL, so
   it stays a session-scoped experiment rather than something a mod ships. The shape that closes it
   is a manifest in `gkplus/mods/*.zip` read through the VFS, so an override arrives with the
   assets it refers to — and `texture:` naming a file rather than another live image, which is now
   a matter of pointing `SetMaterialOverride` at the same loader `VkLighting` uses. `utils/rimutil`
   already converts PNG → `.RIM` in both directions.

   The lighting maps also suggest the cheaper convention: **a file name is an interface**. Nothing
   registers a `<texture> lighting.dds`, which is why it needs no manifest at all, and a
   `<texture> replace.rim` would need none either.

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

Shaders are **Slang**, and the generated header is still checked in so `d3d8.dll` needs no shader
toolchain — but **`cmake --build` compiles them now**, so editing `src/shaders/*.slang` and
building is enough. `src/gen-shaders.py` remains runnable by hand and gained two modes:

```bash
python3 src/gen-shaders.py --check    # is the checked-in header stale? needs no slangc
python3 src/gen-shaders.py --deps     # the sources, for CMake; ENTRY_POINTS is the only list
```

**On a machine with no `slangc` the build refuses a stale header rather than using it** — that is
the half that matters, and it is why staleness is a content hash embedded in the header rather
than a timestamp (a git checkout shuffles mtimes). Both halves are verified by deliberately
breaking them.

This replaces the standing warning that nothing in CMake ran the generator (§4.46), and one thing
from it is worth keeping because it is what the CMake rule had to get right: **the header must be
a declared `OUTPUT`, not just a stamp.** With a stamp alone Ninja treats it as a plain source,
decides every TU including it is clean *before* the generator runs, and links a DLL built against
the previous SPIR-V — measured, and it reproduces §4.46's symptom exactly. The stamp is still
there beside it so an unchanged shader does not re-run `slangc` every build.

The old giveaway still applies to anything else that measures as having changed nothing: the
screenshots being *byte-identical* to the ones before a change is what a real no-effect change
almost never produces. Hash the shots when a change is supposed to move pixels and does not.

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
| `render.specular` | run-time only, on by default: the specular term of that sum. **The mirror image of `GKPLUS_NO_SPECULAR`**, which reaches only the forwarded call — with only one side switchable, "we add specular the original does not" and "we add three times as much" are the same measurement (§4.46). Turn it off in both and the bases can be diffed directly |
| `render.lighting` | run-time only, on by default: the real light sum, or the §4.20 material collapse the build before it used. **The way to measure lighting** — toggle it on a paused frame and the difference image is exactly what it paints, at a 0.00 noise floor (§4.26) |
| `render.sun_shadows` | **a feature**, on by default (§4.58, cascaded in §4.59): a real shadow map from the sun over a box around the camera's orbit pivot, 3x3 PCF, **four cascades in a 2x2 atlas of 2048² tiles**. The first shadow in Gunlok that is not a blob under a unit. Costs **2.1 ms** on level01 against `false`, of which 1.7 is the three extra cascades |
| `GKPLUS_VK_PRESENT_MODE` | `immediate` \| `mailbox` \| `fifo`, launch-time only (§4.79). **Read this before quoting any frame time.** This surface offers no MAILBOX, so the renderer falls back to FIFO and a 60 Hz display puts a 16.67 ms floor under the frame — a frame marginally over it beats between one interval and two and averages ~23 ms, which is the "23 ms" three sections of A/Bs were measured against. `immediate` presents without waiting, so the number is the frame's own cost. Not the default because it tears, and because unthrottling the loop changes the game's own timing rather than only what reaches the screen. `render.vulkan_report` now prints the mode by name and the modes the surface offered |
| `render.sun_shadow_cull` | **A feature, on by default** (§4.77): reject a caster the cascade's box does not contain. **Exact rather than approximate here**, which is unusual for shadow-caster culling — the cascades differ only in x/y half-extent and share a deliberately generous depth range, so there is no occluder outside the box along the light direction that still casts into it. On level02's settled start it rejects **5 of 684 caster-cascades**, because cascade 0's box is 17.5 world units across and the camera sits in the middle of a dense interior; the sweep is what says the machinery works — 90.2% drawn at `shadow_extent` 20, 66.6% at 5, 41.5% at 2. A cull is worth what the scene's spread makes it worth, and a cascade around the camera is the worst case for one |
| `render.sun_shadow_indirect` | **A feature, on wherever the device has `multiDrawIndirect`** (§4.77): one `vkCmdDrawIndexedIndirect` per (cascade, bucket) instead of a `vkCmdPushConstants` + `vkCmdDrawIndexed` pair per caster per cascade — **684 draw calls to 8** on level02's start. The shader needed nothing: `map_shadow_vertex` already exists for the two bakes. Reads back false where the pipeline could not be built, and the direct path then draws the same culled set. `render.sun_shadow_report` prints both counts |
| `render.shadow_cascades` | 1..4, **4** by default: how many cascades are live. **1 is §4.58's single map at the same texel density**, which is what makes cascading A/B-able on one paused frame. Four halving boxes put the near field at 0.0085 world units per texel against the single map's 0.068 |
| `render.shadow_bias` | **in shadow texels**, 2.5 by default, and both halves of that are §4.59. Texels because that is the unit acne is measured in — the depth error across one texel is its world size times the surface's slope — so one value holds on every cascade, level and `shadow_extent`; a value in *depth* units does not. 2.5 is the knee of a sweep: below it level04 shadows itself everywhere, above it the shadow shrinks at 0.06–0.15% of the frame per texel. **Do not look for acne with a ragged-mask metric** — Gunlok's terrain is large flat polygons, so acne shadows a whole facet at a time and reads as a *solid* mask. The shadowed fraction is the instrument |
| `render.shadow_strength` | 0.7, and **the one knob here that is not a fidelity question** — the game never had a real shadow, so there is no ground truth. 1.0 is the physically correct value (the shadow attenuates only the direct terms, so it is exactly "no sunlight arrives here"); 0.7 is where §4.59's two frames meet. §4.58's 0.55 leaves level04's unit shadows reading as a smudge, and 1.0 takes **level02's covered start to 36% of its authored brightness** — that level really is under a roof, the shadow map is right about it, and the level's own bake is what disagrees |
| `render.shadow_extent` | 70, now with a measurement behind it (§4.59): it is the **outermost** cascade's half-extent, and Gunlok's own `camera.max_distance` is 75, so 70 covers everything the camera can ever see. Raising it to 200 buys 0.2% of the frame; dropping it to 40 costs 0.25% |
| `render.map_shadows` | **a feature**, and **on by default since §4.64 — play settled it**: it shipped off because no measurement could say whether the picture with these shadows was right, and the first report from playing was that the map lights do not cast any. The level's own `STDLIGHT` rig casting real shadows, one 6-face cube per light in a 32 MB atlas, baked once per level. Sampling it costs **0.50 ms** on level01 (686 lights) and nothing measurable on level02. Worth **6.886 MAD over 73.3% of level02** — 51 lights with ranges up to 83 units that otherwise reach through every wall — 0.195 over 6.2% of level01, and 0.036 over 0.8% of level04, whose lights sit on open ground with nothing to occlude. **The bake is gated on the knob too**, so off costs nothing; turning it on starts it, and `render.map_shadow_report` is what says when it has finished. `off → on → off` is bit-identical |
| `render.dynamic_shadows` | **A feature, on** (§4.66). The per-frame atlas: 4096² D16 ringed x2, 42 slots of 256-texel faces, rebuilt every frame from the frame's own caster list — map, props and units alike — so a light that moves needs no identity and a unit casts. **0.4 ms** on a frame off the vsync cap. It was off for one section because enabling it lost the device; that was §4.67's field permutation indexing the bindless sampler array with a float's bit pattern, not the bake. Knobs: `render.dynamic_shadow_map_only` (**narrow the casters to §4.65's set exactly — the A/B that prices props and units**), `render.dynamic_shadow_arena_only` (arena-sourced casters only; *not* the mobile-caster test, a unit draws from the arena as often as not), `render.dynamic_shadow_sample`, `render.dynamic_shadow_bias`, and four bisect caps kept from the hunt — `render.dynamic_shadow_indirect` (a draw call per caster instead of the batch) and `_max_lights` / `_max_faces` / `_max_casters`, 0 for no cap. **`render.dynamic_shadow_report` range-checks the batch it just built and prints a sample of it**, and now also prints which submission path and which caps are live — a capped bake that survives looks exactly like a healthy one |
| `render.dynamic_shadow_cull` | **A feature, on by default** (§4.76): reject a caster the light's sphere cannot reach, then one the cube face's frustum does not contain, instead of drawing every caster into every face. A RenderDoc capture of a played frame is what found it — the bake was the second depth-only pass and almost all of the time, 304 casters x 54 faces = **16,416 pieces of geometry** against the world pass's 367. On level02's settled start it draws **689 of 5,130 caster-faces**, and later **416 of 5,130** once §4.77 tightened the bounds block. **No millisecond figure** — §4.79 found every frame time in §4.76–4.78 was taken against a 60 Hz FIFO vsync ceiling, and unthrottled this pass costs nothing measurable; the counts are what stand. **The atlas must not change and does not**: cull on against off is 0.035% of pixels at 0.0010 MAD, against a repeat floor of 0.055% and 0.0103 — so this is a cost knob with nothing to weigh, and `off` exists because a *bounds* defect would show up as one shadow missing from one face rather than as anything bounds-shaped. The boxes it tests had to be built from nothing: nothing in the renderer knew where a draw was, the arena is never mapped and the scratch is write-combined, so they are accumulated at every write (per 64-vertex block for the arena, per draw for a user-pointer one, per version for a parked refill) and **"unknown" always reads as "draw it"**. `dynamic_shadow_report` counts the unbounded casters per bucket, which is what caught the first build covering only the arena half and culling 67.2% instead of 86.6% |
| `render.local_shadow_taps` | The PCF radius for **D3D's point and spot lights** — 0 a single tap, 1 a 3x3 (the default), 2 a 5x5, clamped at 3 (§4.69). Reaches whichever atlas serves them. **The map lights deliberately keep their single tap**: a fragment is in range of a mean of 11.5 of them and the sum already filters, so a kernel there would be a hundred taps a fragment for no visible change. One or two D3D lights reach a fragment and nothing averages them, which is what play reported as jagged. Below the noise to change — 21.92 / 21.74 / 21.56 / 21.51 ms at radius 0 / 1 / 2 / 3 |
| `render.local_shadows` | **a feature**, on by default (§4.65): shadows from the game's **own D3D point and spot lights** — level02's fires, and anything a `.gcs` adds with `ADD LIGHT`. A different light system from `map_shadows`, sharing the same atlas: sixteen of its 682 slots are reserved for these, which only level01 notices (682 → 666 map slots, 4 → 20 refused). Worth **1.92% of the frame at 0.336 MAD** at level02's fire camera against a 0.005 repeat floor, 0.77% at its settled start, and **0.000 over 0 pixels on level04**, whose lights have nothing to occlude. Nothing measurable in frame time — the fetch is last, behind the range, `N·L` and cone rejections, and there are five of these lights where the map lights are 686 for 0.50 ms. **`GKPLUS_VK_LOCAL_SHADOWS=0` is the launch-time form**, and it is not a convenience: the run-time knob needs the REPL, the REPL needs a running game, and a GPU feature suspected of wedging the display cannot be switched off through the display |
| `render.local_shadow_report` | what the local half of the atlas holds. **Not optional reading**, and the pair is `waiting out the stability gate` against `held still but found no free slot` — the first is the feature working (a light that moves lives there permanently and costs nothing), only the second is a limit. A moving light, a light past the sixteen slots, and a cube not yet baked all look identical on screen |
| `render.local_lights` | run-time only, on by default: whether D3D's point and spot lights are in the light sum at all. **A diagnostic, and the one that priced §4.65** — off drops them and keeps the directionals, so a paused A/B paints exactly the pixels they reach, and since a shadow only ever *removes* light that set strictly contains anything shadowing them could change. The ceiling: **2.34% of the frame at level02's fire camera**, 0.75% at its settled start, 0.63% on level04, and the noise floor on level01, level05 and prison. Prison is the self-test — it has no point light, and the knob moves nothing there. **Every one of those is the STATIC lights only**, because pausing is what makes the comparison possible and also what removes the effect lights; the same knob on a moving frame is the reading item 0 needs and nobody has taken |
| `render.frame_lights` | the last complete frame's D3D lights **deduplicated by contents**, with the draws each reached and the frames it has survived. Mirror-side like `frame_draws`, so it reads the same in every renderer mode. It exists because a `GpuLight` is deduplicated by enable mask *within* a frame and carries no identity across one, so "how many distinct point lights does a frame have" was unanswerable. Read `distinct this frame` against `distinct over the session`: a rig that never moves converges the second on the first (level02: **13 keys over 5,525 frames**), and one the game re-authors leaves a new key behind every frame |
| `render.map_shadow_bias` | 1.0, **in atlas texels at the fragment's own distance**, and it is a *normal* offset rather than a depth one (§4.61): a 64-texel cube face is `distance / 32` world units across, so the depth error is dominated by the surface's slope and a depth offset big enough to cancel it would detach every shadow by metres. 1.0 is the larger of two knees — level02's acne is gone by 0.25, level04's needs about 1 — and above it the real occlusion goes with the acne. **`= 0` is the sharpest picture of what the atlas holds**: per-light acne with cube-face stair-stepping and coloured fringes, which is what said the projection was right |
| `render.map_shadow_rate` | lights baked a frame, defaulting to **256 with indirect drawing and 4 without** — taken from the path at atlas creation. §4.62 is why: the 1.9 s the spreading existed for turned out to be draw-call *submission*, not GPU work, so with one command a face level01's 682 lights bake in three frames at the steady-state frame time. Changing it re-bakes from the start |
| `render.map_shadow_indirect` | on wherever the device has `multiDrawIndirect` (§4.62): one `vkCmdDrawIndexedIndirect` per cube face instead of a draw call per caster per face — **804,924 → 4,092** on level01. **The two must produce the same atlas and this is the only thing that can say so**: it rebuilds the pipeline and re-bakes, and on a paused level02 frame the two differ by **0.006 MAD against a same-path floor of 0.010**. Setting it on a device without the feature does nothing and reads back false |
| `render.stencil_shadow` | off by default (§4.58): draw the game's **own** blob shadow as well. It is dropped while the sun casts, or a unit carries both. Its three passes are marked by `stencil_enable`, which §4.31 measured is exact on level01 and level02 — this knob is what checks that on a level those measurements never covered |
| `render.map_light_cull` | run-time only, **on** by default (§4.56): bin the map lights into a world-space grid instead of looping every one per fragment. **Worth 28 ms a frame on level01** — 61.90 ms off against 33.67 on, which is the number §4.56 never took and §4.60 did. **Off must be bit-identical** — a light's range is a hard cutoff, so the grid drops nothing that would have been added, and that A/B is the only thing that can catch a cell quietly missing a light. Measured at 0.00000 MAD over 0 pixels once the blinking "ACTIVE PAUSE" indicator is excluded — which is worth knowing about, because with it in frame the same comparison reads 0.017 against a 0.007 floor and looks like a real defect |
| `render.map_lighting` | **not a diagnostic — a feature**, and **on by default since §4.60**: replace the level's baked per-vertex colour with a per-pixel evaluation of its own `.rif` light rig. It was off on performance grounds and §4.60 took the reading — with §4.56's grid it costs **1.83 ms on level01** (686 lights, the most in the game) and nothing measurable on level02, level04 or level05; **without** the grid the same level costs **30 ms**, which is what it was off for. The model is **fitted against the bake itself** (§4.54, r 0.87-0.96 on three of four levels), and its one free parameter is validated on screen — level04's difference from the bake minimises at exactly the fitted gain of 1.35. Applies to the **map geometry only**; `render.map_lighting_all` lifts that, and measures worse, because a prop carries its own file's bake. `render.map_light_gain` is the lever (default **1.35**, the mean of the fitted values), `render.map_light_report` says what was loaded. **Judge it on level04 or level05, not level02**, whose 51 long-range lights fit at only r 0.37. The falloff's tail is **windowed** since §4.64: `1 - d/range` reaches zero with a non-zero slope, and per pixel that draws a visible rim around every light — which play reported and which the vertex fit could not have seen |
| `render.per_pixel_lighting` | **not a diagnostic — a feature**, and the first one that departs from the original on purpose (§4.52). On by default: D3D8's light sum evaluated per fragment rather than per vertex, same equation and same lights. `GKPLUS_VK_PER_PIXEL_LIGHTING=0` is the launch-time form, and `false` restores the fixed-function path **bit-identically**. Worth 0.48/255 over 26.9% of level02 — but judge it on the **difference image**, not that number: it concentrates on units and curved geometry and is zero on flat ground, which is exactly what Gouraud shading cannot represent |
| `render.half_pixel` | run-time only, on by default: the D3D9 pixel-centre convention as a half-pixel viewport origin (§4.28). Off is the pre-§4.28 behaviour, and worth 1.34/255 over the whole frame |
| `GKPLUS_VK_OFFSCREEN=0` / `render.offscreen` | on by default: rasterise the world at the **game's** backbuffer size into an offscreen target and blit it onto the swapchain, rather than drawing straight into the swapchain and letting the viewport scale every 2D draw (§4.37, §4.38). Off is the pre-§4.38 behaviour exactly, and worth **2.55/255 over 65% of the frame**. `render.vulkan_report` says which is running |
| `render.present_linear` | run-time only, **off** by default: the filter for that final scale. NEAREST is a deduction, not a default — the original's own stretch preserves a 4-bit texture's sixteen distinct values, which a filtered downscale could not (§4.37) — and this is the A/B for it |
| `render.lighting_maps` | **not a diagnostic — a mod-facing feature** (§4.48). On by default: load `graphics/<dir>/<stem> lighting.dds` (or `_lighting.dds`) beside every `.RIM` the renderer knows a name for, from a mod under `gkplus/mods` first and the install second, and give every material whose **stage 0** is that texture a bump/metallic/roughness response. Off interns every material exactly as the build before it did, so it A/Bs on one paused frame at a 0.000 floor — and setting it back to `true` **destroys every image and re-reads every file**, which is the authoring gesture: a map edited while the game runs is picked up by `false` then `true` and by nothing else. `render.lighting_map_report` is the readback, and it is not optional reading — a texture with no companion file is the *normal* case, so a misnamed file and a stock install look identical from the screen |
| `render.bump_scale` / `bump_diffuse` / `specular_scale` / `specular_from_diffuse` / `gloss_min` / `gloss_max` | the lighting maps' knobs, uniform per frame and therefore free to sweep on a paused frame. Three of the defaults are measurements (§4.48): `specular_scale` is **0.25** because level02's key light is `diffuse 4.0` and 1.0 saturates a floor to white; `specular_from_diffuse` is **1** because every light reaching that floor authors `specular 0 0 0`, so at 0 — the game's own answer, and what the fixed-function term uses — the metallic channel does nothing over most of a level; `bump_diffuse` is **1** because a bump that only shapes highlights is invisible wherever metallic is 0 |
| `render.material_override(name, spec)` | **not a diagnostic — the first mod-facing feature** (§4.44). `name` is a case-insensitive substring of a live texture's `.rim` path; `spec` is `{texture, tint: [r,g,b,a?], hide}` or null to remove. Returns the readback, because a substring key that matches nothing — or matches more than was meant — is not an error and cannot be seen from the call. `render.material_overrides` re-reads it, `render.clear_material_overrides()` empties it. **An override that resolves and paints nothing looks exactly like a broken one**: check `draws overridden` in `render.draws`, and pick a target off `render.frame_draws()` so it is one the camera can see |
| `render.rhw_depth_raw` | run-time only, on by default: a pre-transformed vertex's z is the depth value clamped to the viewport slice, which is what D3D does, rather than something to run the viewport's depth range over (§4.45). Off is the pre-§4.45 behaviour. Read at **record** time, so both halves — `BuildMvp`'s compensation and the pipeline's `depthClampEnable` — move together; the game re-issues the same draws while paused, so it still A/Bs on one frame |
| `render.depth_probe(armed, quad_z, clear_z, min_z, max_z)` | draw one opaque magenta quad through the capture device against a depth buffer cleared to a known value under a known viewport slice, `ZFUNC LESS`, no depth write. The quad is either there or it is not, which is what settled §4.45 when nothing about Gunlok's own draws could. **Read it in `d3d8` or `d3d9`**: the clear goes straight to the forwarded runtime, because recording it would have made it the whole Vulkan frame's depth clear. Two rules for using it: discriminate **in both directions** — a monotonic map preserves ordering, so one row is always explicable by the other model plus an offset — and keep `quad_z` **inside** the slice unless the range handling is what you are measuring, or you measure the clamp and read it as the mapping |
| `render.viewport_rect` | run-time only, on by default: honour `D3DVIEWPORT8`'s **rectangle** per draw, as the Vulkan viewport and scissor, rather than covering the whole render target (§4.47). Off is the pre-§4.47 behaviour. Read at **record** time for the same reason as `rhw_depth_raw` — the rectangle reaches both the `DrawItem` and `BuildMvp`'s origin term from there. **The one comparison that cannot use a paused frame**: the upgrade screen will not open while the game is paused, so this A/Bs on consecutive shots inside one session, against a 0.043 repeat floor |
| `render.viewport_probe(armed, x, y, w, h)` | `depth_probe`'s sibling, for the other half of what a viewport does to a pre-transformed vertex: one opaque magenta `XYZRHW` quad 20 px in from the rectangle's own origin. **Read it in `d3d8`.** The reading is a *differential* — arm it twice with different origins and compare how far the quad moved against how far its own coordinates moved — which is what makes it independent of the frame behind it. It settled both rules in §4.47: the origin is not added, and the rectangle clips |
| `render.probe(name, scale, mipmap, offset, alpha)` | draw one textured quad through the capture device, so d3d8, d3d9 and vulkan all get the same draw with the scene's lighting, stages, blending and depth removed (§4.35). `name` is a substring of a live texture's `.rim` path, `scale` is screen pixels per texel; `render.probe(null)` disarms |
| `render.ref_range` / `render.ref_hide` | **`draw_range`/`draw_hide` for the REFERENCE** (§4.42): a draw outside the range, or inside the hide window, is simply not forwarded. Works in `d3d8` and `d3d9` mode, which is the whole point — for three sections "this renderer draws a quad the original does not" could be established and not followed, because every follow-up question is a `draw_range` question and `draw_range` only existed for us |
| `render.draw_state = <index>` | the device state at the moment that draw was issued, diffed against the mirror — **and since §4.46 the lighting equation's inputs**: the material with its `POWER`, every enabled light's colour, range and attenuation, the four `D3DRS_*MATERIALSOURCE` states, the FVF, and the eye with the view matrix it was derived from. "Does the mirror agree" and "what was this draw actually lit with" are different questions, and only the first had an instrument. `render.state` answers neither per draw: it prints the *last* draw of the frame, which on level02 is the text |
| `render.frame_draws([first, last])` | the capture layer's **own** draw list for the last complete frame — index, topology, primitives, FVF, buffered/user-pointer, blend, depth, cull, alpha test, depth slice, stage-0 `.rim` name. Mirror-side, so it works in every mode. It is what `ref_range` is aimed with: **an index does not carry between runs** (aiming at 222 because a `vulkan` session called the quad 222 landed on the HUD portraits), so find the draw by its signature in the mode you are in |
| `render.force_lod` | run-time only, `-1` off: force every texture fetch to an explicit mip level. The probe that ruled mip selection out of the residual (§4.34); pair it with `GKPLUS_NO_MIPMAP=1` on the reference, which pins the original to level 0 |
| `render.shade_mode` | run-time only, on by default: honour `D3DRS_SHADEMODE`, or interpolate everything the way every build before §4.31 did. Worth **0.000** on level01 and level02, because every flat-shaded draw there is the stencil shadow — kept because that is a fact about two levels, not about the game |
| `GKPLUS_VK_LOCAL_SHADOWS=0` | the launch-time form of `render.local_shadows` (§4.65). It exists because the run-time knob is reachable only through the REPL, which is reachable only from a running game on a **usable display** |
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
- **the backbuffer size and every distinct viewport *rectangle*** (§4.38, §4.47). This is the
  reading that found §4.47, and it found it the way it was designed to: it says how many distinct
  rectangles the game has ever set and marks any that is not the whole backbuffer, so opening the
  upgrade screen once turned `1` into `2` with `32,24 575x431` flagged. **In level it still reads
  one**, and the marker on the second is now "honoured per draw" rather than "the world pass
  assumes it is not there" — the rectangle rides on the `DrawItem` beside `min_depth`/`max_depth`,
  which is exactly where this bullet used to predict it would have to go. The *size* assumption
  that remains is §4.38's: the offscreen target is the backbuffer's size, so a backbuffer this
  layer has not seen would still scale wrongly.

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

**`render.stats.frames` is not a liveness test for the renderer, and "the counter moved" is not
one either** (§4.66). It is the *capture layer's* `Present` count and it keeps climbing whether or
not Vulkan is alive, because every call is still forwarded to d3d8to9 — on a session whose device
was already lost it read 10,109 → 10,456 over four seconds while `render.vulkan.frames_presented`
sat frozen at 1,716. **That one is the liveness test — read as a RATE against a baseline.** Both
halves cost a wrong conclusion inside one section: first the wrong counter, then the right counter
read as "it advanced" when it had advanced five frames in eight seconds against a healthy 16.63
ms/frame. A health check is a measurement and needs a baseline like any other.

**Two builds cannot use the toggle, and on level02 a whole-frame cross-launch MAD has a floor of
order 1** (§4.52). Two units idle-animate and nothing pins their phase, so the *same binary*
compared against itself differs by **0.6712** when one shot went through `Shoot-Settled`'s
`-Before` (which sleeps three extra seconds) and the other did not. §4.28's 0.094 was measured with
the camera set explicitly and is not what `Wait-CameraRest` alone delivers. Two ways out, and use
both: **match the `-Before` between the shots**, and **restrict to regions with no animating
geometry** — where the same comparison reads 0.0000 over 0 pixels and answers the question
outright.

**Assert the draw count and the actor count, not just the camera.** A relaunch here landed the
camera somewhere empty and produced **31.77 MAD against everything**, which reads as a catastrophic
regression until you notice `draws: 20 this frame` in `render.draws`.

**Stability is evidence about which defect you have, not whether you have one.** §4.22 needed a
moving scene and vanished on a still one; §4.23 was perfectly stable — 60 consecutive menu
captures were bit-identical with two draws rendering the wrong geometry throughout. Judge the
picture against d3d9, not against the previous frame.

**A paused frame stays pinned for a while, not forever — re-shoot the baseline before believing a
difference.** After a long REPL session the level02 pause had drifted (camera z 2.48 → 38.99, the
scene black), and a shot taken with a feature switched on was very nearly read as that feature
blanking the world. Clearing it and re-shooting is one command and is what said otherwise (§4.44).
The rule is §4.21's "pin the frame", applied inside a single launch rather than across two.

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

- **`PrintWindow` needs flag 3** — `PW_CLIENTONLY | PW_RENDERFULLCONTENT`, and **both bits**.
  With flag 1 alone a Vulkan (or D3D) swapchain window prints **solid black**, because there is no
  WM_PRINT redraw for the compositor to ask for, and a bitmap full of zeros looks exactly like a
  renderer that is not drawing. With flag **2** alone — which is what `shot-gunlok.ps1` actually
  passed until §4.47, against what this line has said since §4.20 — the whole window is rendered,
  title bar and border included, into a bitmap sized from `GetClientRect`: the picture is pushed
  down and right and the bottom and right edges of the game's own frame fall off it. That one is
  silent, because the result still looks like a screenshot of a game in a window. Every shot taken
  before §4.47 is missing roughly 40 rows and 10 columns; a MAD between two of them is still
  valid, and anything about *where* something is is not.
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

- ~~The staging ring stalls ~1080 times a session on a `vkDeviceWaitIdle`~~ — **mostly closed by
  §4.63, and the framing was wrong twice.** It was filed as a level-load problem; pricing the two
  blocking paths says a load costs 70 ms and presents nothing, while **thirty seconds of level01
  play cost 778 ms**. And it was one level: level01 stages 13.3 MB a frame against a 32 MB ring,
  level02 6.2 MB and never stalled at all. The ring now asks each frame's fence with
  `vkGetFenceStatus` and takes retired bytes back for free, blocking only on what is left —
  **511 stalls → 38, 778 ms → 122**, and 0.79 → 0.12 ms a frame. What remains is mostly
  `FlushPendingNow`, which is capacity rather than synchronisation: a bigger ring would fix it and
  is deliberately not taken, because staging is permanently mapped and address space is the
  scarcest thing on a 32-bit host.
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

---

### The Vulkan renderer (`src/D3D8Capture`, `src/Vk*`, `src/VertexFormat`)

A bindless Vulkan replacement for Gunlok's renderer. **`GKPLUS_RENDERER=vulkan` draws the
game** — the world, its textures, its fog of war, its units, its HUD, its **fixed-function
lighting** and its **stencil shadows**. **`vulkan_renderer_plan.md` is where to start** — status
and next steps; `vulkan_renderer_notes.md` is the design record and every measurement behind it.
Read the plan before touching any of this.

**The ground truth is `GKPLUS_RENDERER=d3d8`** (§4.33). Windows 10 still ships a 32-bit `d3d8.dll`
in SysWOW64, so the game runs on the *original* runtime with the capture layer and the whole REPL
harness intact — which matters, because a reference you cannot drive is one you cannot align to
the same frame. Compare against that. `d3d9` (d3d8to9) is the second opinion that says whether a
difference lives in the translation layer or in the game: on a settled level02 frame the two agree
to **0.017/255**, but two HUD columns that belong in the game are drawn by the Vulkan path and
dropped by d3d8to9. Thirty sections of measurement were taken against d3d9 before anyone checked
whether the real thing was available.

**The whole-frame residual against the original is 0.13/255 and 93% of the frame is bit-identical**
(§4.38), against a **cross-launch d3d8-vs-d3d8 floor of 0.034**. It was 2.59 for six sections, and
it was one defect: Gunlok renders into a **640x480** backbuffer while the window's client area —
and so the swapchain — is **628x468**. A pre-transformed draw's pixels-to-clip matrix is built
from the D3D viewport, so a Vulkan viewport covering the swapchain scaled every 2D draw by 628/640
*during rasterisation*, resampling the texture (§4.37). The renderer now rasterises into an
offscreen colour target at the backbuffer size and blits onto the swapchain at the end — the order
the original does it in. Sizing the viewport alone is *not* the fix: it reproduces the original
texel for texel and costs the framing (2.59 → 13.07), because a larger viewport on a smaller
swapchain clips where D3D stretches.

Three things that follow, and are the reason to trust the number: `render.offscreen` toggles it on
one paused frame — worth **2.55/255 over 65% of the frame**, and on→off→on is bit-identical; the
A4R4G4B4 probe quad went from 4.00 MAD / 31.6% bit-exact to **0.0000 / 100%**, which takes the CPU
expansion off the correctness list for good and leaves it a memory question; and of the 6.8% of
pixels still differing, **93% also differ between two d3d8 launches** — the characters idle-animate,
so that part is the game, not the renderer. The blit filter is **NEAREST**, deduced rather than
chosen: the original's own stretch preserves a 4-bit texture's sixteen distinct values, which a
filtered downscale could not.

Four measurement rules, each of which cost a wrong conclusion:

- **A feature is judged on its region, never on a whole-frame MAD** (§4.27) — the stencil shadow
  is worth 5.5/255 over its own region and 0.12 over the frame.
- **...but mean RGB per region is blind to a real difference** (§4.33). It answers "is this region
  the right colour" and cancels a per-texel error with zero bias: the reported junk pile matched
  d3d8 to 0.1 mean RGB while differing by 2.95 MAD against a 0.008 floor. Read both, against a
  floor — which is what having the real reference supplies.
- **Count distinct values, not differences, when a texture looks wrong** (§4.37). Sixteen values
  all multiples of 17 says "one exact 4-bit texel per pixel, no filtering"; 256 says "resampled".
  That one line settled what three sections of difference images could not.
- **An amplified difference image cannot tell a sub-pixel offset from filtering** (§4.28) —
  resample one shot against the other and find where the difference minimises.

**Every draw the game issues now reaches the renderer**, and that is newly true twice over.
`DrawPrimitive` — one of D3D8's four draw entry points — built no draw at all for the whole life
of the renderer, with **every "must be 0" counter reading zero**, because they count *reasons a
draw was rejected* and a draw that is never offered has no reason to be rejected (§4.32). The
reading that catches that class is `draw calls seen` against `submitted` in `render.draws`, the
only number that compares against what the **game** did rather than what the renderer chose.
Separately, the non-triangle-list topologies are on by default now that a stencil buffer exists.

**Gunlok uses six viewport depth slices and none is the default** (§4.32): the world sits in
`0.1..1.0` and the effect layers in thin slices around `0.02..0.06`, so an overlay is in front of
the world by construction rather than by switching the depth test off, and `1.0..1.0` pins a
backdrop to the far plane. `D3DVIEWPORT8::MinZ`/`MaxZ` were recorded nowhere and the viewport was
hardcoded `0..1`, so the world occluded the layers meant to sit over it — fires rendered as bare
scenery. It is per-draw dynamic state now.

**...but it does not apply to a pre-transformed draw the way Vulkan applies it** (§4.45). D3D
skips the viewport transform for a `D3DFVF_XYZRHW` vertex and **clamps** instead —
`depth = clamp(z, MinZ, MaxZ)`, no scale and no bias — measured with `render.depth_probe` against
the real D3D8, because both readings fit everything Gunlok itself draws. Vulkan has no bypass, so
every screen-space draw sat `MinZ * (1 - z)` too far from the camera; an error that shrinks toward
the far plane, which is why level02's flames came and went with camera distance instead of being
uniformly wrong. `BuildMvp` now feeds Vulkan the inverse and `PipelineState::depth_clamp` carries
the clamp — **both halves, or the HUD regresses from bit-exact.**

**The frame is an array of indices** (§4.26, §4.30). A draw's per-draw data lives in a
`GpuDrawRecord` array and its texture stages in an interned `GpuMaterial` array — 274 draws on
level02 are 29 materials — so the push constants carry four device addresses and three indices, 48
bytes, and describe no draw at all. That is what makes a second pass over the frame a walk over
the draw array rather than a replay of the recording loop.

**That table is what the material override rewrites** (§4.44), and it is the first thing this
renderer does that the game never could: `render.material_override("<.rim substring>", {texture,
tint, hide})` retextures, tints or hides every draw sampling one asset, as one entry rather than a
per-draw interception. **The key is the `.rim` name because a bindless index is load-order
dependent** — the same key re-resolved image 34 → 35 across a level reload, which is the
measurement that argues for it — and resolution is name → index once, gated on
`TextureRegistryGeneration()`. Removing an override restores the frame **bit-identically**: an
un-overridden material carries `tint = 0xffffffff`, which multiplies by exactly 1.0. `hidden_draws`
counts into the `seen == submitted + skips` reconciliation, or the one feature that drops draws on
purpose would read as §4.32's regression. Two traps: an override that resolves and paints nothing
looks exactly like a broken one (its asset may be out of view — read `draws overridden`, and pick a
target off `render.frame_draws()`), and a paused frame drifts over a long session, so re-shoot the
baseline before believing a difference.

Fog is measured absent — 0 of 12M draws enable it. **`D3DRS_SHADEMODE` is honoured and has never
mattered** (§4.31): every flat-shaded draw on level01 and level02 is one of the stencil shadow's
three passes, two of which write no colour and one of which is a single-colour quad, so
`render.shade_mode` moves 0 pixels in either. It is also the section on *how* to decide a question
like that — the pipeline histogram says which draws use a state where a count only says how many,
and that chose an extra varying over doubling the pipeline table. **The light sum is D3D8's whole
per-vertex equation** — ambient, diffuse and specular over directional, point and spot lights,
with the material colours tracked from `D3DRS_*MATERIALSOURCE`. §4.19 ruled lighting out on an A/B
that does not reproduce; §4.20, §4.25 and §4.26 are the corrections, and two of this layer's own
counters were measuring nothing while reading plausibly — see §4.26 before trusting one.

Six instruments answer questions no counter can, and each exists because one was needed:

| | |
|---|---|
| `render.draw_range` / `draw_hide` / `draw_info(i)` | attribute a pixel to a draw. Bisect by **hiding a window, never truncating a prefix** — a prefix truncates the depth and stencil buffers too (§4.29) |
| `render.ref_range` / `ref_hide` / `frame_draws()` | the same, pointed at the runtime the capture layer **forwards** to, so the *original* can be bisected — and it works in `d3d8` mode, where there is no Vulkan draw list (§4.42). `frame_draws` is the mirror-side draw list it has to be aimed with: an index does not carry between runs |
| `render.draw_vertices = i` | the converted vertices and indices a draw was actually handed, following its own indices rather than the head of the slice. Answers "wrong shape" vs "something covers it" (§4.32) |
| `render.draw_geometry` | what a **buffered** draw pulled, in four columns — and only two of them are read **at the draw** (§4.42). A deferred readback proves consistency, not correctness: the game refills a dynamic buffer several times a frame, so the arena and the buffer can agree perfectly on a version neither held when the draw was issued |
| `render.force_lod` | force every texture fetch to an explicit mip level. Pair with `GKPLUS_NO_MIPMAP=1` on the reference to pin both sides to level 0 (§4.34) |
| `render.probe(name, scale, mipmap, offset, alpha)` | one textured quad drawn **through the capture device**, so d3d8, d3d9 and vulkan all get the same draw with the scene's lighting, stages, blending and depth removed (§4.35) |

The one fact that shapes everything: **the seam is `Direct3DCreate8`, not the AWAPI render
queue.** The queue looked obvious and is not total — `rendering_notes.md` §4.1 — so
`src/D3D8Capture.cpp` wraps the D3D8 device instead. It is a **state recorder, not a
translation layer**: it mirrors the fixed-function state, replays state blocks into that
mirror, and reduces each draw to a `DrawItem` the Vulkan side consumes. Every call is still
forwarded to d3d8to9 as well, which is what keeps `GKPLUS_RENDERER=d3d9` available as the A/B —
the thing that makes "is this our bug or the game's?" answerable.

Things worth knowing before editing:

- **The capture layer is two translation units, and the split is along a real line.** The
  recorder (`D3D8Capture.cpp`) is everything on the path a frame takes; the evidence
  (`D3D8CaptureReport.cpp`) only ever reads what the recorder wrote, plus the `Note*`/`Log*`
  collectors it exports for the draw path to call. Roughly a third of the original file was the
  second thing. Keep the direction one-way — a diagnostic that mutates state the renderer then
  reads is not a diagnostic — and put new counters, histograms and verifiers in the report TU.
- **`src/D3D8Device.gen.inc.h` is generated** by `src/gen-d3d8-forwarders.py` from d3d8to9's
  `d3d8.hpp`. Re-run it after changing which methods are intercepted. Its
  `check_wrapped_params()` **fails the build** if a method taking or returning a wrapped
  interface is left forwarding — that is not a nicety: forwarding a wrapper to d3d8to9 makes
  it `static_cast` to its own concrete class and read a garbage proxy pointer, which surfaces
  as an access violation inside `d3d9.dll` with nothing pointing back. `ProcessVertices` was
  missed twice by reading before the check existed, and adding `IDirect3DSurface8` to
  `INTERFACES` enumerated all ten surface-carrying device methods — `SetCursorProperties`
  included, which the hand-written prediction had missed. The check covers **every** wrapped
  interface including `IDirect3DDevice8`, because a forwarded `GetDevice` on a resource is the
  same failure without the crash: it hands the game the unwrapped d3d8to9 device, after which
  every call it makes is invisible.
- **Vulkan is reached through volk, never the loader's import library**, and the ImGui Vulkan
  backend is vendored in `third_party/imgui_backends/` for the same reason. GkPlus *is*
  `d3d8.dll`; a load-time dependency on `vulkan-1.dll` would stop the game launching.
- **Every "must be 0" counter in `render.report` is a real invariant.** Each one exists
  because getting it wrong once cost a debugging session.
- **Some defects only a picture can find.** The arena's 16-byte slot alignment against a 48-byte
  vertex was invisible to every counter *and* to the texture readback verification, because
  nothing was wrong with the uploads — only with how a draw addressed them. `render.capture()`
  (RenderDoc) and a window screenshot are the tools for that class; §4.16 also records two wrong
  guesses that cost a rebuild each before the picture was consulted.
- **A counter says the plumbing ran, not that it moved the right bytes.**
  `render.verify_textures()` and `render.verify_buffers()` read each image and each arena slot
  back off the GPU and compare them against the D3D resource they came from — between them they
  have found five defects with every counter reading clean (notes §4.13, §4.24). Both must read
  `158/158` and `3467/3467` on level01. The **ratio** is the invariant, not those two numbers —
  a run on `level02` (which is what automated testing should load, see "Debugging the running
  game") has its own totals, and a mismatch is any run where the two halves differ. Check
  `render.validation` in the same breath as any readback: a verifier that is itself invalid
  reports its own mismatches as the code's, which cost an afternoon.
- **...but a readback taken later proves consistency, not correctness** (§4.42). Both verifiers
  read *now* and compare against what the game holds *now*, and now is a frame or more after any
  draw they are being used to vouch for. Textures do not move, so that half is safe; a dynamic
  vertex buffer is refilled several times a frame, so the arena and the buffer can agree perfectly
  on a version neither held at the draw. That is how a HUD draw rendering the fire's glow quad
  survived "0 of 12 vertices differ" through three sections. `verify_buffers` reading one short on
  both levels is this, and expected: the slot is deliberately frozen mid-frame.
- **Two transfers in a command buffer are not ordered against each other**, and a level load
  hands one arena slot to two buffers inside a single staging batch. Unordered, the loser's
  bytes are what the draw reads — which presents as one object smeared into a black wedge across
  a third of the screen, not as anything synchronisation-shaped. `ordered_overlapping_copies`
  counts the barriers that now prevent it (166,375 a session, so this is the common case, not an
  edge). Notes §4.24, which is also the record of three diagnostics that were built to catch it
  and could not.
- **RenderDoc cannot see a level load.** A load presents nothing, so `CaptureStagingBatch(n)`
  captures a staging *batch* instead — reproducible across runs, so one run says which batch and
  the next captures it. But at full heaps the capture dies on RenderDoc's own readback window
  (§4.17's 32-bit limit again), and at `GKPLUS_VK_HEAPS=small` the upload defects mostly stop
  happening, because a smaller ring flushes 100x more often. `GKPLUS_VK_WATCH_DST` +
  `render.staging_watch` is what works: log every upload to one arena offset with its batch.
- **To find out what the renderer is missing, make the GAME render without it.** Three env vars
  force `D3DRS_LIGHTING`, `D3DRS_SPECULARENABLE` and the texture stages past the first off, in
  the *forwarded* call only. That is what proved D3D fog was not the gap and the second texture
  stage was — after three sections of the plan said the opposite. (The joke §4.51 supplies: that
  stage turned out to be the game's *own* fog of war, drawn as a texture rather than as
  `D3DRS_FOGENABLE`. Both readings of "fog" were half right.) A hypothesis costs one such switch
  and one screenshot diff; reading more of the renderer costs a session and settles nothing (§4.19).
- **An A/B is only evidence about the pixels that were on screen when it ran.** "Lighting
  contributes nothing, 0.08/255" was measured on a frame whose HUD had not appeared yet; with
  the HUD up the same switch turns the panel from green to grey. Check what is actually in the
  frame before generalising from it (§4.20).
- **Two launches are not a comparison.** The renderers run at different frame rates, so at a
  fixed delay the *game* is in a different state — three Vulkan runs of identical code differ by
  up to 8/255, which is bigger than most of what is worth measuring. Toggle the feature at run
  time with the game paused instead (`screen.toggle_pause()`, then `render.topologies`): same
  frame, noise floor 0.03, and the difference image is exactly what the feature painted. For a
  cross-renderer shot, pause *and* set the camera explicitly (§4.21).
- **A screenshot of gl.exe needs `SetProcessDPIAware()` in the capturing process**, or
  `GetClientRect` reports virtualized coordinates, `PrintWindow` renders at the window's real
  resolution, and the bitmap keeps only the top-left two thirds. The HUD is in the upper right
  and went missing from a whole session's shots, all of which looked complete (§4.20).
- **A stage whose texture fails to resolve samples white, and white is not neutral** — it is the
  identity for `MODULATE` and for nothing else. The same texture under `ADDSIGNED` brightens by
  0.5, which is how 83,176 draws a session went unnoticed while only stage 0 was drawn.

| File | Purpose |
|------|---------|
| `src/D3D8Capture.h` | The **public** surface: what `entry.cpp`, `JsRender.cpp` and the Vulkan side may use. `CaptureStats`, the `render.*` readings, the run-time knobs, `D3D8CaptureSystem` |
| `src/D3D8Capture.cpp` | The **recorder**: wraps `IDirect3D8`, `IDirect3DDevice8`, the two buffer types, `IDirect3DTexture8` and `IDirect3DSurface8`; shadow state, state-block replay, per-draw material/pipeline keys, residency, the texture pixel path — `LockRect` on a `SYSTEMMEM` staging texture then `CopyRects` into the `MANAGED` one, so the upload hangs off `CopyRects` (notes §4.12) — and the `AcquireRimTexture` hook that names every image by its `.rim` asset (§4.14) |
| `src/D3D8CaptureReport.cpp` | The **evidence**: the histograms, the verifiers, the frame draw log and every `render.*` reading built on them. Split out because it was a third of one 5,000-line file and none of it is on the path a frame takes — it only reads what the recorder wrote, through the `Note*`/`Log*` collectors. **That direction is the invariant**: a diagnostic that mutates state the renderer then reads is not a diagnostic, and this is the file where that mistake would be made |
| `src/D3D8CaptureInternal.h` | The seam between those two — the wrappers, the shadow state, the shared containers. Deliberately **not** `D3D8Capture.h`: nothing outside those two `.cpp` files should include it |
| `src/VkContext.h/cpp` | Instance, physical device, logical device, validation. Lazily initialized — **never from `DllMain`**, since volk calls `LoadLibrary` and that deadlocks under the loader lock |
| `src/VkRenderer.h/cpp` | Surface, swapchain, frames in flight, the ImGui backend, present — and the **offscreen colour target** the world is rasterised into at the game's own backbuffer size, blitted onto the swapchain at the end (§4.38). The extent comes from `d3d8::BackBufferExtent`, i.e. from the present parameters, not from the D3D viewport: the swapchain shows the whole backbuffer, so that is what the blit's source has to be. The overlay has its **own pass on the swapchain image**, after the blit, so it stays 1:1 with the window instead of going through the scale |
| `src/VkResources.h/cpp` | VMA arenas, the staging ring, the texture images (creation, format mapping, upload, readback verification) and the bindless descriptor set. Nothing device-local is ever mapped. **The vertex arena aligns slots to `sizeof(CanonicalVertex)`, not 16** — a draw addresses its buffer as a vertex index, and a 16-byte-aligned slot silently pulls the wrong vertices (notes §4.16) |
| `src/VkDraw.h/cpp` | The world pass: one `VkPipeline` per distinct blend/depth/cull state (five on level01, built on first sight — notes §4.19), the depth buffer, the per-frame draw list, and the **shader ABI** (`GpuLight`, `GpuDrawRecord`, `GpuMaterial` — the three arrays a draw is looked up in, §4.26 and §4.30). A draw binds nothing and its push constants describe nothing: four device addresses and three indices, 44 bytes of the 72 the block is (the rest is frame-uniform knobs — the LOD probe and §4.48's lighting-map parameters — which ride a push because it is the cheapest way to deliver a float that is the same for every draw). Vertices, its own record, its material and the lights are all pulled by address, from the arena for buffered draws and from a per-frame host-visible scratch for user-pointer ones (§4.18). **Materials are interned per frame** — 274 draws on level02 are 29 of them — which is what makes a second pass over the frame a walk over the draw array rather than a replay of the recording loop. **The list is never sorted**: the game's own order is what makes blending correct |
| `src/VkLighting.h/cpp` | **Lighting maps** (§4.48): the companion `<texture> lighting.dds` beside a `.RIM`, its two lookup roots and two suffix spellings, the per-name cache (**including the misses**, which is what keeps a texture with no companion from costing a file probe per frame), the images this side creates, and the base-slot → lighting-slot table. Keyed and resolved exactly like the material override — by `.rim` name, on `TextureRegistryGeneration()` — and it depends on nothing in the capture layer, because a lighting map is a texture D3D never sees. The decoder is `src/Dds`, unchanged and shared with the engine-facing codec |
| `src/VkCapture.h/cpp` | RenderDoc via its in-app API, so `render.capture()` grabs one frame from the REPL. Off unless `GKPLUS_RENDERDOC` is set, and loaded before the Vulkan instance because it captures by inserting a layer. **Opening a capture has two traps, both reported as `VK_ERROR_OUT_OF_DEVICE_MEMORY` and neither about VRAM** — the *replayer* must be 32-bit (launching from the x86 tooling does not help; the UI replays in its own x64 process, so it needs `x86\renderdoccmd.exe remoteserver`), and an in-level capture needs `GKPLUS_VK_HEAPS=small` (notes §4.17) |
| `src/shaders/*.slang`, `src/gen-shaders.py` | The shaders, in **Slang**, compiled to `src/Shaders.gen.inc.h`, which is checked in so `d3d8.dll` needs no shader toolchain. `cmake --build` runs the generator where `slangc` exists and **fails on a stale header where it does not**; `--check` and `--deps` are the two modes that serve it. `ENTRY_POINTS` is the single source of truth for the build's shader dependencies |
| `src/gen-shader-abi.py` | **The shader ABI, checked by the compiler** (§4.68). Parses the Slang structs and emits `src/ShaderAbi.gen.inc.h` — an `offsetof` per field and a `sizeof` per struct for all **twelve** C++/Slang pairs, so a struct that drifts from the shader it feeds is a compile error naming the field. Needs no slangc, hence its own CMake edge: a machine that cannot build shaders is where drift would otherwise be least likely to be caught. Also fails the build on a struct whose layout differs under scalar and std430 rules, which is the design rule `world.slang`'s `Vertex` comment states and relies on |
| `src/VertexFormat.h/cpp` | Every FVF the game uses → one canonical 48-byte vertex. Pure CPU, no Vulkan and no D3D headers |
| `src/JsRender.cpp` | The `render` namespace: all of the above, readable from the REPL — plus `material_override`, the one member of it that is a **feature** rather than a measurement. Only that part is in `types/gk.d.ts`; the diagnostics move with whatever is being investigated, so `Render` carries an index signature and says so |
