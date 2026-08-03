# Vulkan renderer: status and next steps

The working plan for the bindless Vulkan renderer. **`vulkan_renderer_notes.md` is the design
record and the evidence**; this file is what to do next and how to pick it up. Nothing here
restates a measurement — every claim of fact points at a section there instead, so there is
one place to correct when something turns out wrong.

## Where it stands

**`GKPLUS_RENDERER=vulkan` draws the game.** The world geometry, the textures, the lightmaps, the
units, the HUD, the **fixed-function lighting** and the **stencil shadows** all render through
Vulkan, and **every draw the game issues reaches the renderer**. Against the **real D3D8** on a
settled, paused level02 frame the whole frame is **2.59/255**; against d3d8to9 it is 2.59 as well,
and those two agree with each other to **0.017**.

**Compare against `GKPLUS_RENDERER=d3d8`, not d3d9** (§4.33). Windows 10 still ships a 32-bit
`d3d8.dll` in SysWOW64, so that mode runs the game on the original runtime with the capture layer
and the whole REPL harness intact — which is what makes the frame alignable, and a reference you
cannot drive is one you cannot align. `d3d9` is now the *second opinion*: it says whether a
difference lives in the translation layer or in the game. Thirty sections were measured against
d3d9 before anyone checked whether the real thing was on the machine.

No missing *feature* is known, and the residual has a cause:

- **The 2.59 is one defect, and it is a size mismatch** (§4.37). Gunlok renders into a **640x480**
  backbuffer; the window's client area, and so the swapchain, is **628x468**. A pre-transformed
  draw's pixels-to-clip matrix is built from the D3D viewport, so a Vulkan viewport covering the
  swapchain scales every 2D draw by 628/640 *during rasterisation* — resampling the texture, so no
  sample lands on a texel. The original rasterises 1:1 and lets its windowed `Present` stretch the
  finished frame. Item 1 under "Next"; sizing the viewport alone is **not** the fix.
- **Edge fringes** on every silhouette. Half of these were a real bug and are gone: D3D8/9 sample
  a pixel at its integer coordinate where Vulkan samples at the centre, so every interpolated
  value was **half a pixel out** in every frame this renderer had drawn (§4.28), worth 1.34/255.
  That fix is confirmed correct against the original — 2.90 with it, 5.07 without (§4.33). What
  looked like "genuine filtering" afterwards is the resampling above.
- **The HUD's two 3-pixel-wide bright columns** read 134 against d3d9's 35, and **they belong in
  the game: this renderer draws them and d3d8to9 does not** (§4.28). 0.75% of the frame. The
  `d3d8` mode is how that finally gets adjudicated, which was never possible before.
- **The effect layers are drawn but still dimmer than the original** (§4.32). Two defects fixed —
  `DrawPrimitive` was never wired up, and the six viewport depth slices were collapsed to one —
  and a glow now appears where there was nothing, with one of three blobs still missing.

Four measurement rules, each of which cost a wrong conclusion. Read them before trusting an older
number:

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
owns. `src/D3D8Capture.cpp` is a **state recorder, not a translation layer**: it mirrors the
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
| The effect layers still being dimmer than d3d9 | ❌ the depth slice was necessary, not sufficient — §4.32 |
| **Compare against the original**, not d3d8to9 — `GKPLUS_RENDERER=d3d8` | ✅ §4.33 — d3d8 vs d3d9 is 0.017 on a settled level02 frame, so the residual is 2.59 against the real thing |
| **The last residual against the original** — 2.9 on an oblique decal, and what a player sees as "the junk pile looks wrong" | ❌ **cause found**: the game renders 640x480 into a 628x468 swapchain, so every 2D draw is rasterised 2% small and resampled (§4.37). Not mips (§4.34), not alignment or alpha (§4.35), and A4R4G4B4 was the contrast agent rather than the cause (§4.36) |
| The last 2.59/255 against the original | ❌ one defect, §4.37 — 2D draws rasterised at 628/640 instead of stretched at present |

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
draws: 273 this frame / 279 peak
draw calls seen: 237109   submitted: 237109   unaccounted for: 0   <- the reconciliation, §4.32
skipped: 0 topology, 0 arena slot, 0 no transform, 0 unconvertible, 0 scratch full, 0 no record
lit draws: 227437 (221256 with a light on, 0 want COLOR2, 0 before any material, 0 lights dropped)
depth format: 130 = D24_UNORM_S8_UINT (with stencil)   stencil draws: 5076 (0 with no buffer)
materials: 28 this frame / 30 peak (0 dropped)   5082 flat-shaded draws (all of them the shadow)
viewport depth-slice changes: 7919                 <- the six slices, §4.32; 0 would be wrong
pipelines: 9 (0 failures)
stages: 0 unimplemented ops, 0 needing more than two, 0 bound textures unresolved
render.verify_buffers() 2953/2953     render.verify_textures() 292/292
validation errors: 0
```

The cumulative figures scale with how long the run sat there; the **ratios and the zeros** are the
invariant. `seen == submitted` and `unaccounted for: 0` are the two that say the frame is whole —
they are what §4.32 added, and no other counter here can see a draw that was never offered.

## Four things that shape everything downstream

Read these before changing the draw path; each cost real time to establish.

- **The seam is `Direct3DCreate8`, not the AWAPI render queue** (notes §1, `rendering_notes.md`
  §4.1). The queue looked total and is not.
- **A buffer's arena slot holds one version, and the game uses more than one per frame**
  (§4.23). It refills a vertex/index pair twice a frame with plain `NOSYSLOCK` locks over the
  same bytes, so the later versions are parked in the frame's scratch and a draw names its
  vertex and index source separately. `unversioned_rewrites` must stay 0.
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

1. **Render at the D3D backbuffer size, then scale to the swapchain** (§4.37). *Cause found.*
   Gunlok renders into a **640x480** backbuffer; the window's client area — and so the swapchain —
   is **628x468**. A pre-transformed draw's pixels-to-clip matrix comes from the D3D viewport, so
   the Vulkan viewport covering the swapchain **scaled every 2D draw by 628/640 during
   rasterisation**, resampling the texture so no sample landed on a texel. The original renders
   1:1 and lets its windowed `Present` stretch the finished frame.
   Proof: on a 1:1 probe quad the original produces **16 distinct values, 100% multiples of 17** —
   one exact 4-bit texel per pixel — and this renderer produces 256, 38.6%. Sizing the Vulkan
   viewport from the D3D viewport reproduces the 16 exactly, and costs the framing (2.59 → 13.07
   whole-frame) because a larger viewport on a smaller swapchain clips where D3D stretches. That
   experiment is reverted; the fix is an offscreen colour target at the backbuffer size, the depth
   buffer to match, ImGui on the right attachment, and a final scaled blit.
   **A4R4G4B4 was the contrast agent, not the cause** (§4.36): a 4-bit channel has 16 levels, so
   2% resampling visibly falls off the ladder, where an 8-bit one just blurs. The 0.60 floor on
   the DXT1 quad is the same defect not announcing itself.
2. **Re-measure everything once item 1 lands.** The 2.59 is one defect touching every
   pre-transformed pixel, so most of what is currently called residual is downstream of it and
   several open numbers should move at once — the HUD's remaining difference (§4.28), the "edge
   fringes" on silhouettes, and the 0.60 floor the DXT1 probe quad showed (§4.36). Ruled out and
   not worth revisiting: mip selection (§4.34), sub-pixel alignment (§4.35, the resample sweep
   minimises at exactly (0,0)), the A4R4G4B4 expansion (`r | (r << 4)` is correct), and alpha
   (§4.36, bit-identical). Take the three-way again before opening anything new.
3. **Finish the effect layers** (§4.32). Two defects found and fixed from a play report — a whole
   draw entry point that was never wired up, and the viewport depth range — and the glow sprites
   are drawn now but still dimmer than d3d9, with one of three blobs missing. The next reading is
   `render.draw_range` on that draw against `render.draw_hide` of everything after it, which
   separates "blended away" from "still partly occluded". This is the only item on this list a
   player can see.
4. **Why d3d8to9 drops the HUD columns.** A question about the *instrument* rather than about
   this renderer — nothing about the frame improves by answering it, and what does improve is
   knowing where else the d3d9 reference lies. Much cheaper now than when §4.29 tried: the
   `d3d8` mode adjudicates it directly, which was not possible then. §4.29 searched and **did not
   find it**, which makes the search itself the useful part — the draw is named exactly (draw 65,
   6 quads, `units\plates 2 1024.rim`, opaque `MODULATE`, 583 pixels at 125.9 here against 26.4
   there, meaning d3d9 rasterises nothing), and mip selection, culling, the depth test, an
   out-of-range draw call and d3d8to9's draw path are all eliminated. Two untried leads, both from
   §4.29's table of the states D3D9 does not have: the game **toggles
   `D3DRS_SOFTWAREVERTEXPROCESSING`**, which d3d8to9 honours only on a mixed-VP device, so what
   Gunlok created its device with is the next thing to look up; and `NO_CULL` is the weakest of
   the three switches (0.243 against a 0.094 floor) and deserves redoing on open geometry.
5. **The material *override*, now that there is a table to override** (§5). `GpuMaterial` is
   interned per frame from the D3D state (§4.30), so a mod hook is a rewrite of one entry rather
   than a per-draw interception — and the key is the `.rim` name, which is the identity a mod can
   write down. This is the first thing the bindless shape was for, and it is the first item on
   this list that a player would see.

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
| `GKPLUS_NO_CULL=1` / `GKPLUS_NO_ZTEST=1` | forwarded only: `D3DRS_CULLMODE` to NONE, `D3DRS_ZENABLE` off — the two states that make a draw vanish outright rather than come out wrong (§4.29) |
| `GKPLUS_VK_TOPOLOGIES` | **On by default since §4.27.** The variable now selects a *subset*: `none`/`0` for none, `strip`/`line` to bisect the two, `all`/`1` (or unset) for both. Also settable at run time as `render.topologies`, which is what makes them A/B-able on one paused frame |
| `render.lighting` | run-time only, on by default: the real light sum, or the §4.20 material collapse the build before it used. **The way to measure lighting** — toggle it on a paused frame and the difference image is exactly what it paints, at a 0.00 noise floor (§4.26) |
| `render.half_pixel` | run-time only, on by default: the D3D9 pixel-centre convention as a half-pixel viewport origin (§4.28). Off is the pre-§4.28 behaviour, and worth 1.34/255 over the whole frame |
| `render.probe(name, scale, mipmap, offset, alpha)` | draw one textured quad through the capture device, so d3d8, d3d9 and vulkan all get the same draw with the scene's lighting, stages, blending and depth removed (§4.35). `name` is a substring of a live texture's `.rim` path, `scale` is screen pixels per texel; `render.probe(null)` disarms |
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
  in the *reference*.

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
`stencil_draws_without_buffer`. Each exists because
getting it wrong once was expensive. (`texture_surface_levels` is *not* one of them — it was never a defect counter, and
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
- **`render.verify_buffers()` reads 3468/3469 on level01 in level**, stably, and that is
  **pre-existing** rather than a regression — the same run on the pre-§4.30 build reads the same
  (§4.31). The odd buffer is a pre-transformed one the game refills while the verifier reads it,
  the same shape already documented for `fx.snow(true)`. Level02 is unaffected at 2953/2953.
- **The HUD's two bright columns are missing from the d3d9 reference, not from this renderer**
  (§4.28). Nothing to fix here; what is unknown is why d3d8to9 drops them, and therefore where
  else the reference is wrong. Item 4 under "Next".
- **Only level01 and level02 have been measured**, plus a little of level03 and junkyard. The FVF
  set, material count, stage count, sampler count and scratch peaks are all "measured there", not
  "proven for every level".
  §4.7's max-2-active-stages in particular is worth re-checking on a level with richer materials
  before shrinking `GpuMaterial`. `level02` is the cheapest second data point — it is the level
  automated runs should be loading anyway (see "Working on this"), so widening these is a matter
  of reading `render.state` on a run that is happening regardless.
- **The overlay is tiny at 4K** (§4.5) — cosmetic, ImGui is drawing 1:1 into the swapchain.
