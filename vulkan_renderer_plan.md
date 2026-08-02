# Vulkan renderer: status and next steps

The working plan for the bindless Vulkan renderer. **`vulkan_renderer_notes.md` is the design
record and the evidence**; this file is what to do next and how to pick it up. Nothing here
restates a measurement — every claim of fact points at a section there instead, so there is
one place to correct when something turns out wrong.

## Where it stands

**`GKPLUS_RENDERER=vulkan` draws the game.** Level01's world geometry, its textures and its
units all render through Vulkan, and the A/B against `GKPLUS_RENDERER=d3d9` is the same scene
from the same camera. What is missing is fog, lighting, blending and the non-triangle-list
topologies — listed under *Next* below, all deferred deliberately rather than unexplained.

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
| Texture pixels in `VkImage`s, content-verified by readback | ✅ §4.13 — 147/150 mip levels |
| Every image named by its `.rim` asset | ✅ §4.14 — 52/53 |
| Bindless descriptor array + samplers | ✅ §4.15 — 4096 slots, 5 samplers |
| **The world drawn by Vulkan**, textured | ✅ §4.16 — A/B matches d3d9 |
| RenderDoc capture from the REPL | ✅ §4.17 |
| **User-pointer draws** — text, particles, menus, units | ✅ §4.18 |
| Fog and lighting | ❌ next |
| Pipeline buckets (blend, depth, cull per draw) | ❌ next |
| Line lists and triangle strips | ❌ next |
| `GpuDraw`/`GpuMaterial` arrays replacing push constants | ❌ the §2 design |

Steady state on level01, in level, under validation:

```
draws: 363 this frame / 660 peak / 809133 total
skipped: 6158 topology, 3425 no arena slot, 0 no transform, 0 unconvertible, 0 scratch full
vertex: 10273 KB live / 32768 KB     index: 583 / 8192 KB     slots live: 3464
images: 53 live / 59 created, 112 MB      bindless: 4096 slots, 5 samplers
scratch: 800 KB vtx + 66 KB idx peak per frame
validation errors: 0
```

## Four things that shape everything downstream

Read these before changing the draw path; each cost real time to establish.

- **The seam is `Direct3DCreate8`, not the AWAPI render queue** (notes §1, `rendering_notes.md`
  §4.1). The queue looked total and is not.
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

1. **Fog and lighting.** The largest remaining gap against d3d9 — level01 is a fogged cavern,
   and the ceiling structure our shot shows is geometry the reference hides in fog rather than a
   defect. The shadow state already records the fog render states; `src/World.h` has the game's
   fog globals.
2. **Pipeline buckets.** One pipeline today: blending off, depth always on, cull fixed at
   back/clockwise. §4.7 measured **6 distinct pipeline states** for a whole level, and
   `PipelineKey()` already computes the key per draw and throws it away. This is what turns
   transparency on.
3. **Line lists and triangle strips.** `skipped_topology` is 6,158 and became non-zero only
   once the user-pointer draws arrived (§4.18). One more pipeline each, or convert strips to
   lists in the scratch.
4. **The material key on the asset name, not the wrapper pointer** (§4.14). Worth doing before
   `GpuMaterial` is defined: the pointer means nothing across runs, and the name is the identity
   a mod has to be able to write down. Afterwards it means changing the key, the table and the
   shader interface together.
5. **`GpuDraw`/`GpuMaterial` arrays** replacing the per-draw push constants — the design in §2.
   Changes nothing on screen; it is what makes a second pass over the frame cheap, which is the
   whole point of the bindless shape.

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

Read results with `render.report` (the D3D capture side), `render.vulkan_report` (device,
swapchain, arenas, images, scratch, bindless), `render.draws` (the draw list and what it
skipped), `render.textures` (every image with its `.rim` name), `render.stats`, `render.vulkan`
and `render.validation`.

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
`scratch_exhausted`, `dropped_over_capacity`. Each exists because getting it wrong once was
expensive. (`texture_surface_levels` is *not* one of them — it was never a defect counter, and
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
- **The per-frame scratch is sized ~5× larger than measured** (§4.18): 4 MB/1 MB against peaks
  of 800 KB/66 KB. Deliberate headroom, and the peaks are the numbers to resize by.
- **Only level01 has been measured.** The FVF set, material count, stage count, sampler count
  and scratch peaks are all "measured on level01 and the menu", not "proven for every level".
  §4.7's max-2-active-stages in particular is worth re-checking on a level with richer materials
  before shrinking `GpuMaterial`.
- **The overlay is tiny at 4K** (§4.5) — cosmetic, ImGui is drawing 1:1 into the swapchain.
