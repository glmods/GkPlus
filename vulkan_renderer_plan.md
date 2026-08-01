# Vulkan renderer: status and next steps

The working plan for the bindless Vulkan renderer. **`vulkan_renderer_notes.md` is the design
record and the evidence**; this file is what to do next and how to pick it up. Nothing here
restates a measurement — every claim of fact points at a section there instead, so there is
one place to correct when something turns out wrong.

## Where it stands

The renderer replaces Gunlok's D3D8 usage from *behind `Direct3DCreate8`*, which GkPlus
already owns. `src/D3D8Capture.cpp` is a **state recorder, not a translation layer**: it
mirrors the fixed-function state, replays state blocks into that mirror, reduces each draw to
a material and a pipeline key, and forwards every call to d3d8to9 unchanged. Alongside it,
`GKPLUS_RENDERER=vulkan` puts a real Vulkan swapchain on the game's own window.

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
| Textures wrapped, surfaces wrapped, pixel path settled | ✅ §4.11, §4.12 |
| Texture pixels uploaded into `VkImage`s, content-verified | ✅ §4.13 — 147/150 mip levels |
| Every image named by its `.rim` asset | ✅ §4.14 — 52/53 |
| Bindless descriptor array + samplers | ✅ §4.15 — 4096 slots, 5 samplers |
| Anything drawn from game geometry | ❌ Phase 3 |

**The game still renders through d3d8to9.** In Vulkan mode it shows a clear colour and the
ImGui overlay; the capture layer observes and uploads but nothing reads the arenas yet.

## The pixel path is settled — and the answer moved the upload

§4.12. `IDirect3DSurface8` is wrapped, and the measurement is unambiguous in both renderer
modes: **0 surface locks, 0 render-to-texture, and exactly one `CopyRects` per texture
`LockRect`**. The 2:1 `GetSurfaceLevel` ratio that looked alarming was two surfaces per blit,
not a second lock.

The engine uploads through the D3D8 staging idiom — lock a `SYSTEMMEM` texture, `CopyRects`
it into the `MANAGED` one it binds. So `IDirect3DTexture8::LockRect` really does see every
pixel, but on a texture that is never drawn with, and `CopyRects` is the only thing that says
where the bits end up.

**Therefore the upload hangs off `CopyRects`, not off `Unlock`.** Two measurements constrain
it, and both are in §4.12:

- **94% of blits carry an explicit sub-rect** (2,689 of 2,862), so the copy must honour the
  rectangle — at 4x4 block granularity for `DXT1`/`DXT3`. Restaging the whole surface would
  give textures that are right on the frame they load and stale ever after.
- **173 blits are whole-surface, in every run regardless of length.** That is the static
  texture set, uploaded once at load; everything above it is per-frame dynamic update.

## Done: the images (§4.13)

Every bound texture has a device-local `VkImage` whose contents track the game's, verified by
GPU readback rather than by counters — `render.verify_textures()` reports **17/17 mip levels at
the menu and 147/150 in level**, with validation clean. Read §4.13 before touching this: the
check found four real defects the counters could not see, including two in the staging ring
that predated the textures entirely, and it overturned §4.12's conclusion about *where* to read
the pixels from.

## Done: the bindless set (§4.15)

4096 image slots, 5 samplers for the whole game, `descriptors_out_of_range: 0`, validation
clean. `TextureImage::index` is the descriptor index directly, so a descriptor is written once
at image creation rather than per frame — 64 writes for a level session.

## Next: Phase 3, drawing

Everything the draw path needs is now on the GPU and addressable. Remaining before a triangle:

1. **Build the material key on the asset name, not the wrapper pointer** (§4.14). The pointer is
   what `MaterialKey()` hashes today, and it is meaningless across runs — which is exactly the
   identity a mod needs to write down. Doing it as part of defining `GpuMaterial` costs nothing;
   doing it after the shaders exist means changing the key, the table and the shader interface.
2. Per draw, emit a `GpuDraw` into a per-frame array — the shadow state already produces the
   material and pipeline keys that select its entries, and `StageSampler()` already resolves the
   sampler index per stage.
3. Übershader over the texture stages; one pipeline per state bucket (§4.7 measured 6).
4. Flip `CaptureDevice::Present` to draw the array instead of clearing.

## Then: Phase 3, drawing

Everything needed is now measured. `GpuMaterial` and `GpuDraw` are specified in notes §2, and
§4.7 says the tables are small: **6 pipeline states, 47 materials, at most 2 active texture
stages** for a whole level.

1. Per draw, emit a `GpuDraw` into a per-frame array — the shadow state already produces the
   material and pipeline keys that select its entries.
2. Übershader over the texture stages; one pipeline per state bucket.
3. Flip `CaptureDevice::Present` to draw the array instead of clearing. The D3D9 path stays as
   the A/B fallback (`GKPLUS_RENDERER=d3d9`), which is what makes "is this our bug or the
   game's?" answerable.

Two things deliberately deferred: the `RenderQueue_Submit` seam as *enrichment* (bounding
spheres and LOD for culling — notes §1, "it demotes"), and shrinking `stage_tex[8]` now that
§4.7 measured at most two active stages. Neither blocks drawing.

## Working on this

```
cmake --build build && cmake --build build --target copy
```

The copy **fails silently while an instance holds the DLL** — check the deployed file's
timestamp, not the build's. That invalidated a whole bisect once (§4.8).

Testing is the REPL, driven from PowerShell. `launch-gunlok.ps1` in the session scratchpad
handles the three things that otherwise waste a run — `-skipfmv`, the modal "Run in a window?"
dialog that blocks *before* the REPL listener opens, and the foreground-lock dance needed to
focus the window. If it is gone, notes §4.4 and §4.6 have the details to rebuild it.

| variable | effect |
|---|---|
| `GKPLUS_RENDERER=vulkan` | Vulkan owns the window; anything else keeps d3d8to9 |
| `GKPLUS_VK_VALIDATION=1` | plus `VK_ADD_LAYER_PATH` at the self-built 32-bit layer (§4.6) |
| `GKPLUS_RENDER_UNFOCUSED=1` | keep rendering while the window is inactive (§4.2) |
| `GKPLUS_WRAP_BUFFERS` | `none`/`vb`/`ib`/`both` — bisecting the buffer wrappers. **Did nothing until §4.13**: `ReadWrapMode()` was defined and never called, so any earlier bisect that used it proved nothing |
| `GKPLUS_TEXTURE_UPLOAD` | `both`/`seed`/`blits` — the same bisect for the two halves of the texture upload |

Read results with `render.report`, `render.vulkan_report`, `render.stats`, `render.vulkan`
and `render.validation`. **`render.verify_textures()` is the only check on texture *contents*
rather than on the plumbing** — it reads each image back off the GPU and compares. Run it with
`render.validation` in the same breath: the readback is itself Vulkan work, and a broken
verifier reports mismatches that are its own (§4.13).

**Every "must be 0" counter is a real invariant**, not decoration: `foreign_buffers`,
`unconvertible_buffers`, `failed_uploads`, `opaque_block_applies`,
`surface_texture_lock_rects`, `texture_render_targets`, `copy_rects_untracked`. Each exists
because getting it wrong once was expensive. (`texture_surface_levels` is *not* one of them —
it was never a defect counter, and §4.12 explains what it actually counts.)

For a crash, use `cdb` — path and the `-cf` requirement are in §4.8. WER also leaves a husk of
the crashed process that makes the harness lie; kill `WerFault.exe` first.

## Known gaps

- **The vertex arena has no eviction.** Slots are freed when a buffer is released, which is
  correct, but a level with far more geometry than level01 could exhaust 32 MB. `arena_full`
  counts it; nothing recovers from it.
- **The staging ring stalls ~1080 times a session** (§4.13). Wrap safety is now real, but it
  is enforced with a `vkDeviceWaitIdle` rather than per-slot fences, because the renderer's
  fences belong to `VkRenderer`. Affordable while the Vulkan path only clears and draws the
  overlay; it needs to become a proper fence wait before Phase 3 puts real work behind it.
- **1-3 mip levels in 150 drift** (§4.13), always level ≥1 and always small. Level 0 matches
  everywhere. `render.verify_textures()` detects it; the writing route is unidentified.
- **112 MB of images on level01**, roughly half of which is the `A4R4G4B4`→`R8G8B8A8` CPU
  expansion. Mapping that format natively would halve it, at the price of a per-device support
  matrix — worth revisiting only if memory ever matters.
- **Only level01 has been measured.** The FVF set, material count and stage count are all
  "measured on level01 and the menu", not "proven for every level". §4.7's max-2-active-stages
  in particular is worth re-checking on a level with richer materials before shrinking
  `GpuMaterial`.
- **The overlay is tiny at 4K** (§4.5) — cosmetic, ImGui is drawing 1:1 into the swapchain.
