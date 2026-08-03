# Vulkan renderer: status and next steps

The working plan for the bindless Vulkan renderer. **`vulkan_renderer_notes.md` is the design
record and the evidence**; this file is what to do next and how to pick it up. Nothing here
restates a measurement — every claim of fact points at a section there instead, so there is
one place to correct when something turns out wrong.

## Where it stands

**`GKPLUS_RENDERER=vulkan` draws the game.** Level01's world geometry, its textures, its
lightmaps, its units, its HUD, its **fixed-function lighting** and its **stencil shadows** all
render through Vulkan. Against `GKPLUS_RENDERER=d3d9` from the same explicitly-set camera on a
paused frame the whole frame is **4.59/255**, and every region measured except the HUD matches to
within 0.5. **Every draw the game issues now reaches the renderer** — the non-triangle-list
topologies are on by default, because the stencil buffer was the only reason they were not.

No missing *feature* is known. What is left is residual:

- **The HUD is +2.6/+3.4/+0.8 against d3d9** and has been throughout, unchanged by the light sum
  and unchanged by the shadows, so it is neither a shading nor a masking question (§4.26, §4.27).
  Nothing has looked at it.
- **Edge fringes** on every silhouette, which are filtering and sub-pixel differences. They
  dominate any whole-frame MAD — which is the correction §4.27 makes to the previous entry here:
  the shadow was the largest missing *feature* and is worth 5.5/255 over the region it covers,
  yet only 0.12 over the frame. **Judge a feature on its own region.**

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
| **`GpuDraw`** — per-draw data in an array, push constants 120 → 72 bytes | ✅ §4.26 |
| **Stencil shadows** — depth format with a stencil aspect, stencil in `PipelineState` | ✅ §4.27 — the shadow region matches d3d9 to 0.5/255 |
| The last 4.59/255 against d3d9 | ❌ edge filtering and the HUD, §4.27 |
| `GpuMaterial` — the texture stages, keyed by asset name | ❌ the §2 design, and the room now exists |

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
render.verify_buffers() 3469/3469     render.verify_textures() 158/158
validation errors: 0
```

Two of those changed with §4.27 and are not regressions: **11 pipelines**, because the five
stencil fields are part of the key, and **0 topology skips**, because the strips and line lists
are drawn now. `render.verify_buffers()` reads 3468/3469 while `fx.snow(true)` is running — the
verifier reading a buffer the game is refilling, not an upload defect (§4.27).

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

1. **The HUD.** The one region that does not match, at +2.6/+3.4/+0.8, and the only residual left
   that is a *colour* rather than an edge. It is unchanged by the light sum (§4.26) and unchanged
   by the shadows (§4.27), which between them rule out the two things that looked likeliest — so
   this needs a fresh hypothesis and one of the `GKPLUS_NO_*` switches to test it, not more
   reading. Compare the HUD **region**, since it is a few thousand pixels in a frame whose MAD is
   dominated by silhouette fringes.
2. **The material key on the asset name, not the wrapper pointer** (§4.14). Worth doing before
   `GpuMaterial` is defined: the pointer means nothing across runs, and the name is the identity
   a mod has to be able to write down. Afterwards it means changing the key, the table and the
   shader interface together.
3. **`GpuMaterial`** — the other half of §2's design. `GpuDraw` landed with the light sum
   (§4.26), so the eight texture-stage words are all that is still per-draw push constants, and
   the block is down to 72 bytes of a guaranteed 128. Changes nothing on screen; it is what makes
   a second pass over the frame cheap, which is the whole point of the bindless shape.

Two things deliberately deferred and still deferred: the `RenderQueue_Submit` seam as
*enrichment* (bounding spheres and LOD for culling — notes §1, "it demotes"), and shrinking
`stage_tex[8]` now that §4.7 measured at most two active stages. Neither blocks anything.

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

Testing is the REPL, driven from PowerShell. `launch-gunlok.ps1` in the session scratchpad
handles the three things that otherwise waste a run — `-skipfmv`, the modal "Run in a window?"
dialog that blocks *before* the REPL listener opens, and the foreground-lock dance needed to
focus the window. If it is gone, notes §4.4 and §4.6 have the details to rebuild it.
`shot-gunlok.ps1` beside it captures the window with `PrintWindow`, which is what makes a
change judgeable at all — see the warning below.

| variable | effect |
|---|---|
| `GKPLUS_RENDERER=vulkan` | Vulkan owns the window; anything else keeps d3d8to9 |
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
| `GKPLUS_VK_TOPOLOGIES` | **On by default since §4.27.** The variable now selects a *subset*: `none`/`0` for none, `strip`/`line` to bisect the two, `all`/`1` (or unset) for both. Also settable at run time as `render.topologies`, which is what makes them A/B-able on one paused frame |
| `render.lighting` | run-time only, on by default: the real light sum, or the §4.20 material collapse the build before it used. **The way to measure lighting** — toggle it on a paused frame and the difference image is exactly what it paints, at a 0.00 noise floor (§4.26) |
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

Read results with `render.report` (the D3D capture side), `render.vulkan_report` (device,
swapchain, arenas, images, scratch, bindless), `render.draws` (the draw list and what it
skipped), `render.textures` (every image with its `.rim` name), `render.state` (below),
`render.stats`, `render.vulkan` and `render.validation`.

**`render.state` is what to read before implementing any fixed-function behaviour.** Every state
the renderer has to reproduce — fog, lighting, blend, depth, stencil, colour write — as it
stands *and* every value it has ever been set to, and then six histograms of what was actually
drawn with, ordered by draw count:

- **texture-stage configurations** and **pipeline configurations**, keyed by FVF so the 2D draws
  can be told from the world's;
- **draws with no vertex diffuse**, with the material the fixed function colours them from —
  which is how the HUD's green was found (§4.20);
- **draws that are not triangle lists**, described individually down to the screen box their
  vertices cover, their colour, z, rhw and stencil state — four a frame, and how §4.21 was
  settled;
- bound textures that did not reach the shader, and vertex buffers drawn from with no arena slot.

Twelve stage configurations and thirteen pipeline configurations for a level01 session, which is
small enough to implement one by one rather than approximate. That is the whole reason these
print *values* and not, as `PipelineKey()` did for three sections, only how many there are.

### Comparing against d3d9

One run per renderer, then a numeric difference. `levels.start` lands on the **briefing screen**
— a character portrait over rock, which renders plausibly and is not the scene — so dismiss it
with a space and give the intro camera twelve seconds to settle. `shoot-level.ps1` in the session
scratchpad is the whole procedure. Its **0.07/255** is the floor for the *capture*, measured
d3d9-against-d3d9; it is not the floor for the comparison, which is the next paragraph.

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

**Do not compare two launches if you can avoid it.** The renderers run at different frame rates,
so at a fixed delay the *game* is in a different state: three Vulkan runs of identical code at
the same settle differ by up to **8.06/255** (§4.21). Where a feature can be toggled at run time
— `render.topologies` can — pause the game with `screen.toggle_pause()` and shoot the same frame
twice. That floor is **0.03**, and the difference image is exactly the pixels the feature
touched. `controlled.ps1` does the cross-renderer version: pause, then set `camera.position`,
`yaw`, `pitch`, `roll` and `distance` explicitly so the framing does not depend on timing —
reading those values back from the session you are measuring, because the same literals replayed
later framed something else. `render.draws` collapsing to a couple of dozen a frame is the tell.

Three things about the shot itself, each of which invalidated real work before it was found:

- **Call `SetProcessDPIAware()` in the capturing process.** gl.exe is not DPI aware, so
  `GetClientRect` reports 418x312 against a real 628x468 swapchain, and `PrintWindow` renders at
  the window's own resolution — a bitmap sized from that rect silently keeps the top-left two
  thirds. The HUD is in the upper right and was absent from an entire session's screenshots
  (§4.20). A DPI-aware caller asking about a non-aware window gets the physical rect.
- **Twelve seconds, not ninety.** The two renderers run at different frame rates, so by ninety
  seconds the *game* is in a different state and the frames are not comparable — a bisect run
  there gave five numbers with no pattern and a rock that appeared and vanished between runs of
  identical code. Sample the d3d9-vs-d3d9 noise floor at whatever settle you use and do not
  trust a difference smaller than it.
- **The HUD is only up after the intro**, which is what makes those two rules pull against each
  other: a HUD comparison needs the long settle, so compare the HUD *region* rather than the
  frame, and take the whole-frame number at twelve seconds.

```bash
python3 -c "from PIL import Image, ImageChops; a=Image.open('d3d9.png').convert('RGB'); b=Image.open('vulkan.png').convert('RGB'); d=ImageChops.difference(a,b); px=list(d.getdata()); print(sum(sum(p) for p in px)/(3*len(px)))"
```

Amplify that difference (`d.point(lambda v: min(255, v*4))`) and look at it before theorising:
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

**Every "must be 0" counter is a real invariant**, not decoration: `foreign_buffers`,
`unconvertible_buffers`, `failed_uploads`, `opaque_block_applies`, `surface_texture_lock_rects`,
`texture_render_targets`, `copy_rects_untracked`, `descriptors_out_of_range`,
`scratch_exhausted`, `dropped_over_capacity`, `unversioned_rewrites`,
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
- **The per-frame scratch is sized well above what is measured** (§4.18, §4.26): 4 MB/1 MB
  against peaks of 1087 KB/84 KB for vertices and indices, and 2304 KB/448 KB against 185 KB/18
  KB for the draw records and lights. Deliberate headroom, and the peaks are the numbers to
  resize by. The record slice is sized to `kMaxDrawsPerFrame` on purpose, so the two limits
  agree rather than one biting first.
- **A device with no stencil-capable depth format falls back to depth only**, logs a line, and
  draws no shadow mask (§4.27). `render.draws` reports the chosen format and
  `stencil_draws_without_buffer`, so it announces itself rather than looking like a shading bug —
  but nothing has been tested on such a device, because none is known.
- **Specular is computed with the local-viewer form only.** `D3DRS_LOCALVIEWER` is on for every
  level01 draw and the shader assumes it; a level that switches it off would want the
  infinite-viewer eye direction instead. Nothing counts this yet.
- **The HUD is +2.6/+3.4/+0.8 against d3d9** and has been throughout — identical with the light
  sum on and off (§4.26) and with the shadows on and off (§4.27), so it is neither a shading nor
  a masking question. Nothing has looked at it; it is item 1 under "Next".
- **Only level01 has been measured.** The FVF set, material count, stage count, sampler count
  and scratch peaks are all "measured on level01 and the menu", not "proven for every level".
  §4.7's max-2-active-stages in particular is worth re-checking on a level with richer materials
  before shrinking `GpuMaterial`.
- **The overlay is tiny at 4K** (§4.5) — cosmetic, ImGui is drawing 1:1 into the swapchain.
