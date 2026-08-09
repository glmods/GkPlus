# Replacing the renderer with bindless Vulkan

The design record for the Vulkan backend. `rendering_notes.md` is the analysis of what Gunlok's
renderer *does*; this file is what replaces it and why. Read §1 before anything else — it is a
measurement that overturned the obvious plan, and every other decision here follows from it.

Status: **the renderer draws the game.** `GKPLUS_RENDERER=vulkan` renders the world, its
textures and lightmaps, the units, the HUD, the fixed-function light sum and the stencil shadows,
and every draw the game issues reaches it. The D3D8 capture layer (`src/D3D8Capture.cpp`) mirrors
the fixed-function state, replays state blocks into it, reduces every draw to a `DrawItem`, and
tracks buffer and texture residency — while still forwarding every call, which is what keeps the
A/B available.

**The reference is `GKPLUS_RENDERER=d3d8`** (§4.33), which runs the game on Windows' own 32-bit
D3D8 with the capture layer and the REPL harness intact. Against it, a settled and paused level02
frame is **2.59/255** whole-frame — and that residual has a known cause (§4.37): the game
rasterises into a 640x480 backbuffer while the swapchain is 628x468, so every pre-transformed draw
is scaled by 628/640 during rasterisation instead of being stretched once at present.

Sections are in the order they were measured, and several correct earlier ones. Where they
disagree the later number wins — §4.36 is superseded by §4.37, and §4.33 corrects the reading of
the junk pile in §4.30-§4.32. `vulkan_renderer_plan.md` carries the current state; this file
carries how each of it was arrived at, including the wrong turns, because most of them were wrong
in a way worth not repeating.

## 1. The seam, and the measurement that chose it

The obvious seam is `RenderQueue_Submit` @ 0x0059d760 — 102 call sites, and it hands over semantic
objects (`Renderable`, `AwMaterial`, `CameraData`, `LightSet`) that `src/Render.h` already mirrors.
It is the wrong one, and `rendering_notes.md` §4.1 is the measurement:

> Of the nine functions that call the four `Aw_Draw*` wrappers, only **two** are downstream of
> `RenderQueue_Flush`.

Text, particles, the in-game menus, the shadow renderer and the world-effect overlays all draw
immediately, through `RenderBatch_Draw` @ 0x005a3970 or their own direct calls. A hook on the queue
sees none of them. Two of the four wrappers are also **user-pointer** draws, so for those there is
no `IDirect3DVertexBuffer8` in existence at any moment — capturing geometry at
`CreateVertexBuffer`/`Lock` misses them too.

What *is* total, verified by scanning all 662,906 `.text` instructions: **the D3D8 device.** Exactly
four call sites reach an `IDirect3DDevice8::Draw*` slot, all four inside the `Aw_Draw*` wrappers,
and they are the only functions among the 24 displacement candidates that also reference
`direct3d_device` @ 0x007c121c.

So the seam is the one GkPlus already owns: **`Direct3DCreate8`.**

```
gl.exe ──► d3d8.dll!Direct3DCreate8   (ours, src/exports.def)
              │
              ├─ GKPLUS_RENDERER=d3d9 ──► d3d8to9 ──► D3D9      (today; kept as the A/B fallback)
              └─ GKPLUS_RENDERER=vulkan ─► capture device ──► bindless Vulkan
```

### It is a state *recorder*, not a translation layer

This is the distinction that keeps the job from being a mini-DXVK. The capture device does not
implement D3D8 semantics on Vulkan. It keeps one flat struct of fixed-function state —
`SetTransform`, `SetRenderState`, `SetTextureStageState`, `SetTexture`, `SetMaterial`, `SetLight`,
`SetStreamSource`, `SetIndices`, FVF — and on each `Draw*` it *snapshots the parts that matter* into
a `GpuDraw` and appends it to an array. Resource creation gives us the geometry for the buffered
path; the UP draws hand us the pointer directly. Both covered, no AWAPI knowledge required.

D3D8 fixed-function state sounds large and is not: the game's own material object
(`AwMaterial`, `src/Render.h`) carries **nine** render states and eight texture stages, and that is
the whole of what `ApplyMaterial` sets. Phase 0's remaining half measures the true set rather than
guessing it.

**The queue seam does not disappear — it demotes.** `RenderQueue_Submit` still tells us "the next N
draws are one `Renderable`, at this LOD, with this bounding sphere", which is exactly the enrichment
a shadow pass or a culling pass wants. It is optional, it is added after the backend works, and
nothing depends on it for correctness.

## 2. The bindless core

The point of bindless here is **not** framerate. Gunlok is a 2000-era game and the draw counts are
trivial. The point is that a frame becomes a plain array, so any additional pass — shadows,
G-buffer, an ID buffer for picking, wireframe, an 8K screenshot — is another walk over the same
array with a different pipeline. That is the flexibility being bought.

**One descriptor set, bound once per command buffer:**

| binding | contents |
|---|---|
| 0 | `VkSampler samplers[~16]`, immutable — the distinct filter/address combos from `AwTextureStage::{min,mag,mip}_filter` |
| 1 | `texture2D textures[]` — `VARIABLE_DESCRIPTOR_COUNT \| PARTIALLY_BOUND \| UPDATE_AFTER_BIND`, capacity 16k |

The game ships 513 textures, so 16k leaves the whole range to mods.

**No buffer descriptors at all.** `VK_KHR_buffer_device_address` (core in VK 1.2) plus push
constants:

```c
struct FrameAddrs {          // 128 bytes of push constant is guaranteed; this fits easily
  VkDeviceAddress draws;     // GpuDraw[]
  VkDeviceAddress materials; // GpuMaterial[]
  VkDeviceAddress lights;    // GpuLight[]
  VkDeviceAddress vertices;  // one global arena - vertex pulling, not vkCmdBindVertexBuffers
  VkDeviceAddress globals;   // view/proj/fog/time
};
```

Vertex pulling out of one arena is what makes a draw bind *nothing*: no `BindDescriptorSets`, no
`BindVertexBuffers`, no `SetTexture`, no `SetMaterial`.

```c
struct GpuMaterial {
  uint  stage_tex[8];        // bindless indices, ~0u = none
  uint  stage_ops[8];        // packed COLOROP/ARG1/ARG2/ALPHAOP/ARG1/ARG2
  uint  num_stages, flags;   // alpha test, lighting, fog
  float alpha_ref;
};
struct GpuDraw {
  float world[12];
  uint  material, mesh, light_range, flags;
};
```

Four decisions inside this, in decreasing order of how much they simplify everything downstream:

- **Canonicalize the vertex format at capture.** Convert every FVF the game uses to one
  `{pos, normal, color, uv0, uv1}` layout on the way into the arena. One layout means one arena,
  trivial pulling, and one vertex shader. This is the single highest-leverage simplification in the
  design, and it is only safe because Phase 0 enumerates the FVFs rather than assuming them.
- **Bucket by pipeline only** — `(blend, depth test/write, cull, alpha test)`, which is a handful of
  combinations. The two-level material→texture sort in `RenderQueue_Flush` existed purely to
  minimise `SetTexture`/`SetMaterial`, and bindless deletes its reason to exist. Keep the *third*
  list though: `needs_depth_sort` items still need back-to-front, because that is transparency and
  not state.
- **Interpret the texture stages in an übershader**, looping `num_stages` and switching on the op.
  It is ~10 D3D8 ops, correct for every material with no discovery pass, and costs one pipeline per
  state bucket instead of hundreds. Specialize with specialization constants only if profiling asks.
- **No multi-draw-indirect.** At these draw counts a plain `vkCmdDrawIndexed` per item with zero
  binds between is already nothing. MDI is a later optimization that does not change the data model.

**Lighting.** `LightSet` bucketing exists because D3D8 had 8 hardware lights. Replace it with a light
array plus a per-draw `light_range`. World geometry already carries baked lighting in the colour
attribute, so dynamic lights are sparse. Going clustered-forward later changes the shader and not
the data model.

## 3. What will bite

- **32-bit address space is the real constraint**, not GPU capability. 2 GB of user VA (check
  whether `gl.exe` is `/LARGEADDRESSAWARE`) shared with the game, the driver and QuickJS. Never
  persistently map a large heap: a modest staging ring (~32 MB) plus an unmapped device-local arena.
  Vulkan itself is fine in x86 — the 32-bit `vulkan-1.dll` ships with the loader and VK 1.2 features
  are not bitness-gated.
- **The focus gate.** `RenderSceneAndPresent` @ 0x00574c50 wraps its body in
  `if (DAT_007c1230 != 0)`, cleared on focus loss, so the game stops presenting entirely when
  unfocused. Once we own present that becomes our policy call; `SetFrameWakeupEnabled` (`src/GUI.h`)
  is the existing out-of-band heartbeat.
- **Teardown may never run.** `game_defects_notes.md` §4: the game faults on exit. Treat Vulkan
  shutdown the way `src/Vfs.cpp` treats its temp directories — assume it does not happen, and sweep
  on the next startup instead.
- **`ShadowRenderer_Quality23` has no call edge.** It is installed as a function pointer by
  `ApplyShadowQuality`, so reachability queries say nothing reaches it. It draws immediately. The
  shadow *quality setting changes which functions are producers at all* (`rendering_notes.md` §5).
- **Keep the d3d8to9 path selectable.** `GKPLUS_RENDERER=d3d9|vulkan`. The A/B pays for itself every
  time the question is "is this our bug or the game's?"

## 4. Phases

| # | Deliverable | Proves |
|---|---|---|
| 0a | ✅ Is the queue total? | **No.** §1, and `rendering_notes.md` §4.1 |
| 0b | Interposing capture device in *logging* mode: histogram FVFs, render states, texture-stage configs, draws/frame, texture count+size | The true state subset, arena sizes, pipeline count. Also builds Phase 2's skeleton |
| 1a | ✅ Vulkan instance + device inside gl.exe, capability report | The design's features exist on real hardware. §4.3 |
| 1b | ✅ Surface + swapchain on the game HWND, clear colour presented | Vulkan owns the window; rebuild path exercised. §4.4 |
| 1c | ✅ ImGui on the Vulkan backend | `main.mjs`'s `draw_gui` renders unchanged. §4.5 |
| 2a | ✅ Shadow state + state-block replay; each draw reduced to a material and pipeline key | 6 pipelines, 47 materials, 0 unwitnessed blocks. §4.7 |
| 2b | ✅ Wrap the buffer objects: live-vs-created residency and per-frame upload volume | 8 MB arena, 4.7 MB/frame staging. §4.8 |
| 2c-i | ✅ VMA arenas + staging ring; every buffer owns a slot and uploads on Unlock | 6.1 MB resident, 0 exhaustion, validation clean. §4.9 |
| 2c-ii | ✅ Canonical 48-byte vertex format, all six FVFs converted | 0 unconvertible, arena 1.65x. §4.10 |
| 2c-iii | ~ Textures wrapped; pixel path measured, not yet settled | LockRect vs GetSurfaceLevel is 1:2. §4.11 |
| 2c-iv | Wrap IDirect3DSurface8 to settle it, then the image upload + bindless array | Textures reach the GPU |
| 3 | Draws replayed through the bindless renderer | **The whole game renders on Vulkan** — not "world only", because the seam is total |
| 4 | Retire d3d8to9 as the default; queue-seam enrichment for culling/LOD | The semantic layer, once it is an optimization rather than a dependency |
| 5 | Actual new capability | The payoff |

Phase 3 rendering *everything* rather than a subset is the direct consequence of §1. The seam being
the device rather than the queue is what turns a long partial-blackness period into a single switch.

## 4.1 Phase 0b results

Measured on a running game through `src/D3D8Capture.cpp`, read back with `render.stats` over the
REPL. One session: front-end menu, a `level01` load, then play. **12,618,337 draws over 76,458
frames.**

- **Six FVFs, and every one fits the canonical layout.** `0x002` (XYZ), `0x112`
  (XYZ|NORMAL|1 uv), `0x152` (+DIFFUSE), `0x1C4` (XYZRHW|DIFFUSE|SPECULAR|1 uv), `0x212`
  (XYZ|NORMAL|2 uv), `0x252` (+DIFFUSE, 2 uv). `0x252` is 10.8M of the 12.6M draws - the
  lightmapped world geometry, whose second UV set is the lightmap. So the "one canonical
  `{pos, normal, color, uv0, uv1}` vertex format" simplification in section 2 is **safe**, with
  one caveat: `XYZRHW` is pre-transformed screen space and needs a shader path that skips the
  view/projection transform, not just a different stride.
- **39% of draws are user-pointer** (4,916,022 of 12,618,337). Section 1's argument is not
  theoretical - a capture built on `CreateVertexBuffer`/`Lock` would miss two draws in five.
- **Three primitive types**: `TRIANGLELIST`, `TRIANGLESTRIP`, `LINELIST`.
- **Texture formats**: `A4R4G4B4` (26), `A8` (28), `DXT1`, `DXT3`. No DXT5, matching what
  `rimutil` already refuses by name. 119 textures in the session.
- 165 draws per frame on average, peak **662**. Small enough that section 2's "no
  multi-draw-indirect" call holds comfortably.
- Transform states used: exactly three - `VIEW`, `PROJECTION`, `WORLD`.

**The state-block result is the one that changes Phase 2.** 87 state blocks recorded, carrying
2,114 states between them (max 37 in one block), applied **3,711,481 times** - about 48 material
changes per frame. And:

> **`blocks_opaque` is 0.** Not one `CreateStateBlock` or `CaptureStateBlock` in the whole
> session.

Every block is built by `BeginStateBlock` .. `EndStateBlock`, which issues its state through the
ordinary setters. So the recorder sees the complete contents of every block, and Phase 2 can
maintain a correct shadow state purely by recording blocks and replaying them on `ApplyStateBlock`
- no device-state introspection, no reading back from D3D. If even one block had been an opaque
snapshot of device state we never saw, that whole approach would have been unavailable.

It also explains an alarming intermediate reading. A steady-state sample (after `render.reset()`,
in level) reported **11 render states, 2 stage states and a max texture stage of 0** across 873,200
draws - impossible for geometry carrying two UV sets. The states are not absent: they are set once
at block-build time, during the level load, and replayed by `ApplyStateBlock` thereafter. Over the
whole session the totals are 55 render states and 112 stage states, with all 8 texture stages
touched.

**Two caveats on the numbers, both real:**

- `vertex_buffers`/`index_buffers` count **creations, not live objects** - the recorder does not
  track releases. The session shows 81,149 vertex buffers and 122,514 index buffers created, which
  is per-frame churn (the immediate-mode path allocates and frees around 4 index buffers a frame),
  not 672 MB resident. Peak *live* geometry, which is what sizes the Vulkan arena, is still
  unmeasured - it needs the resource objects wrapped, which Phase 2 does anyway.
- `frames` counts `Present` calls, so it measures rendered frames, not elapsed ones. See §4.2.

## 4.2 The focus gate, and switching it off

Gunlok renders and presents nothing while its window is inactive, which makes any measurement
driven from another window read zero. `RenderSceneAndPresent` @ 0x00574c50 wraps its whole body in
`if (DAT_007c1230 != 0)`, and `OnActivateApp` clears that gate on focus loss.

**The gate is a "D3D resources are valid" flag, not a "should I draw" flag** - it is cleared
*because* `ReleaseD3DResources` @ 0x00574960 has just released the textures, vertex buffers and
cached state. Forcing it back to 1 draws through released objects.

`GKPLUS_RENDER_UNFOCUSED=1` (`src/GUI.cpp`) skips `OnActivateApp` entirely instead. Measured, same
build, in level, with the app genuinely deactivated by another process taking the foreground:

| | frames over 5 s |
|---|---|
| flag off | **0** |
| flag on | **7,153** |

Three traps, each of which cost a run:

- **Suppressing only `ReleaseD3DResources` is not enough, and it kills the game.** The two branches
  of `OnActivateApp` are not symmetric around it: the focus-*gain* branch runs restore work with no
  counterpart on the loss side (`FUN_005a1d60(&TexturesObject)` against the loss branch's
  `FUN_005a1ca0`, plus three more). Skip the release and that restore still runs, against objects
  that were never released. The first version of the patch did exactly this and the process
  vanished during the next level load - with **no WER record at all**, so the usual
  `game_defects_notes.md` recipe finds nothing to read.
- **`ReleaseD3DResources` may not be no-oped globally.** It has six callers and only
  `OnActivateApp` is about focus; the others are resolution changes and multiplayer session setup,
  which genuinely need the teardown.
- **Minimizing the window is not a test.** `ShowWindow(SW_MINIMIZE)` from another process left
  `gl.exe` still reported as the foreground window, so `WM_ACTIVATEAPP(0)` never fired and the
  *control* rendered 4,668 frames while minimized. A valid negative control needs another window to
  actually take the foreground, which needs the ALT-tap trick to defeat the foreground lock -
  `SetForegroundWindow` alone is silently ignored.

Only sound in **windowed** mode: in exclusive fullscreen the device really is lost on Alt-Tab, and
keeping the gate set only means every `Present` fails instead of being skipped. Off by default, and
only the exact value `1` enables it.

One thing worth knowing that is not about rendering: **the game does not leave load state 18 for
play state 5 until the window is focused**, patch or no patch. So a scripted level load still has
to focus the window at least once.

## 4.3 Phase 1 results: the device

`src/VkContext.cpp`, read back with `render.vulkan_report`. Brought up **inside gl.exe**, a
32-bit process, while its D3D9 device was live:

```
status: ok
device: AMD Radeon RX 7600 XT (discrete)
api: 1.4.349   driver: 0x0080018b
device-local: 16368 MB   host-visible device-local: 16368 MB
bindless sampled images: 4294967295   push constants: 256 bytes   graphics family: 0
descriptor_indexing=1 runtime_array=1 partially_bound=1 variable_count=1
update_after_bind=1 non_uniform_indexing=1 buffer_device_address=1
```

All seven required features present, so section 2's design is supported as written. Two numbers
worth carrying:

- **`maxDescriptorSetUpdateAfterBindSampledImages` is 0xffffffff** - effectively unbounded. The
  16k bindless table is not a compromise with anything.
- **256 bytes of push constant**, double the guaranteed 128, so `FrameAddrs` is never tight.
- **The entire 16 GB device-local heap is also host-visible** (full ReBAR). That is a hazard
  rather than a gift here: it makes it easy to map far more than a 32-bit process can afford, so
  section 3's "never persistently map a large heap" rule is load-bearing on this machine
  specifically, not just in principle.

Three implementation constraints, all of which bite before anything renders:

- **`volkInitialize` calls `LoadLibrary`, so initialization can never live in `DllMain`** -
  that is the loader lock, and it deadlocks. `VkContext` initializes lazily on first use from
  the main thread instead.
- **A machine with no Vulkan must not be an error.** GkPlus *is* `d3d8.dll`; a hard link to the
  loader would stop the game launching. volk resolves it at run time and `Initialize()` returns
  a reason.
- **The legacy `HKLM\SOFTWARE\...\Khronos\Vulkan\Drivers` ICD keys are absent on a current
  driver.** Modern loaders discover the ICD through the display adapter key's
  `VulkanDriverName` / `VulkanDriverNameWow`. For a 32-bit host it is the `...Wow` one that
  matters; their absence from the Khronos key says nothing about support.

## 4.4 Phase 1b results: the swapchain

`src/VkRenderer.cpp`. `GKPLUS_RENDERER=vulkan` puts a Vulkan surface on the game's own window
and presents a clear colour instead of the game. Verified on screen: the game window filled
with RGB **(26, 41, 71)**, which is the clear value (0.10, 0.16, 0.28) to the byte.

```
requested: vulkan   ready: yes
swapchain: 3840x2160, 3 images, format 37 (B8G8R8A8_UNORM), present mode 2 (MAILBOX)
presented: 4072   rebuilds: 2   acquire failures: 0
```

**The switch is a branch, not a null device**, exactly as §4.3 predicted:
`CaptureDevice::Present` returns `D3D_OK` without forwarding and calls `DrawFrame()`. The game
keeps its D3D9 device and every resource; it simply stops reaching the screen. Nothing had to be
stubbed.

Six things worth keeping:

- **The swapchain extent is in PHYSICAL pixels and `GetClientRect` is not.** The surface reported
  3840x2160 where the client rect read 2560x1440 - the display is at 150% scaling and `gl.exe` is
  not per-monitor DPI aware, so Win32 hands out virtualized coordinates while Vulkan reports the
  real ones. 3840/2560 is exactly 1.5. Never size anything from `GetClientRect`;
  `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` is the authority.

  Those were **full screen** numbers, and that was not obvious at the time: the client area was
  the whole desktop. Answering the launch dialog with "run in a window" (see below) gives a
  swapchain of 628x468 instead, which is the game's own client area. If the swapchain comes up
  the size of the desktop, the game is full screen - which is also the mode where none of this
  is supported.
- **The intro FMV presents outside the D3D device.** For the first ~40 seconds of a launch,
  `Present` is never called at all and the renderer stays `ready: no` with no error, because Bink
  drives the screen itself (`file_io_notes.md` already notes Bink is off gl.exe's IAT). This reads
  exactly like a broken renderer. **Launch with `-skipfmv`**, which is the supported way past it;
  a posted `WM_KEYDOWN`/`VK_ESCAPE` also works. The first `Present` after the intro went straight
  to Vulkan.
- **`-skipfmv` raises a modal dialog of its own, and it blocks before the REPL exists.** A
  `#32770` titled "Development only mode requester", asking *"Run in a window? This may not work
  on some cards! (Default is full screen.)"* with Yes = `IDYES` (6) and No = `IDNO` (7). Nothing -
  not the REPL listener, not the D3D device - comes up until it is answered, so a wait-for-port
  loop hangs forever. Dismiss it with
  `PostMessage(dlg, WM_COMMAND, IDYES, GetDlgItem(dlg, IDYES))`, and pick **Yes**: windowed is the
  only mode in which the Vulkan path and `GKPLUS_RENDER_UNFOCUSED` are sound.
- **The FMV is not the only scripted footage in the way — `level01` opens with a cutscene too.**
  `level01.gcs` ends in `PLAY CUTSCENE first contact`, so the twelve-second settle after
  dismissing the briefing is partly spent watching a scripted camera, and where it has got to
  depends on how fast the machine reached that frame. That is a second source of the
  cross-launch variance §4.21 measures, on top of the frame-rate difference. **Load `level02`
  for anything automated** — `level02.gcs` issues no `PLAY CUTSCENE` (nor do `prison`,
  `level03`, `level04` or `level06`; every other campaign level does). The measurements
  recorded in this file were taken on level01 and stay stated that way: reproducing one means
  loading level01 deliberately, cutscene included.
- **`vkCreateSwapchainKHR` retires the old swapchain whether it succeeds or fails**, so the old
  handle and its views and semaphores are destroyed on both paths. Only the success path installs
  the new one.
- **`VK_ERROR_OUT_OF_DATE_KHR` from acquire must not reset the fence or advance the frame index.**
  Nothing was submitted, so the fence is still signalled from last time round and the acquire
  semaphore was never waited on; resetting either desynchronizes the frame ring in a way that
  only shows up under resize.
- **The render-finished semaphore is per swapchain IMAGE, not per frame in flight.** A present
  waits on it, and only the image index tracks when that is done. Two frames in flight against
  three images is exactly the case where a per-frame semaphore breaks.
- **Present mode is MAILBOX where available, not FIFO.** The engine runs at ~300 fps in level, and
  FIFO would throttle the whole game loop to the monitor - that changes game timing, not just
  presentation.

### One build trap, because it cost a link error

**vcpkg's prebuilt volk is compiled WITHOUT `VK_USE_PLATFORM_WIN32_KHR`.** `volk.h` still declares
`vkCreateWin32SurfaceKHR` (the header sees *our* define), so the mismatch surfaces as an undefined
symbol at link time and nowhere useful. The fix is to compile the shipped `volk.c` into GkPlus so
the platform defines apply to volk's own translation unit too - `find_path(VOLK_SOURCE_DIR NAMES
volk.c)` and link `volk::volk_headers` instead of `volk::volk`.

## 4.5 Phase 1c results: the overlay

`main.mjs`'s existing `draw_gui` renders over the Vulkan clear colour with **no script change**,
which is the property worth having: the ImGui *context*, the Win32 backend and the F11 toggle stay
with `GUISystem` whichever renderer is running, and only the rendering half is swapped. `GUI.h`
grew exactly two accessors for this - `IsOverlayVisible()` and `RunOverlayDrawCallback()`.

The frame also changed shape: the clear is now the colour attachment's **load op** inside
`vkCmdBeginRendering` rather than a separate `vkCmdClearColorImage`. One less barrier, one less
layout transition, and the swapchain no longer needs `TRANSFER_DST` usage.

### The dependency trap, which took three attempts

vcpkg builds imgui's `vulkan-binding` **without** `IMGUI_IMPL_VULKAN_NO_PROTOTYPES`, so its
prebuilt `imgui_impl_vulkan.o` calls `vkCreateFence` and friends directly and needs a Vulkan
import library. GkPlus deliberately has none - it reaches Vulkan through volk's runtime loading
precisely so that **`d3d8.dll` keeps loading, and the game keeps starting, on a machine with no
Vulkan at all**. That is not a nicety for a d3d8 proxy to a 2000 game.

1. Linking `Vulkan::Vulkan` works and silently destroys that property: a load-time import on
   `vulkan-1.dll`.
2. `/DELAYLOAD:vulkan-1.dll` would have kept it, and is provably safe here because nothing
   reaches an imgui Vulkan entry point except through `StartImGui`, which runs only after
   `volkInitialize` succeeded. **It is not available**: the installed Vulkan SDK has no `Lib32`,
   and gl.exe is x86, so there is no 32-bit import library to delay-load.
3. What works: vendor `imgui_impl_vulkan.cpp` (`third_party/imgui_backends/`) and compile it into
   GkPlus with `IMGUI_IMPL_VULKAN_NO_PROTOTYPES`, filling its pointers from volk through
   `ImGui_ImplVulkan_LoadFunctions`. Only the `.cpp` is vendored; the header still comes from
   vcpkg, so a version bump fails to compile rather than drifting silently.

**Verified**: `llvm-objdump -p build/Debug/d3d8.dll` lists no `vulkan-1.dll` among its imports.

Two smaller things:

- **`VK_KHR_dynamic_rendering` has to be enabled as an extension even though it is 1.3 core.**
  imgui's backend requires it explicitly and says so in its header; promoted-to-core is not
  enough for it.
- **The overlay is tiny at 4K.** ImGui is drawing at 1:1 into a 3840x2160 swapchain with no font
  scaling, so the window is physically small. Cosmetic, and a `FontGlobalScale` /
  `DisplayFramebufferScale` decision for whenever the overlay matters more than the renderer.

## 4.6 Validation, on a platform LunarG no longer ships

**LunarG dropped 32-bit Windows components from the SDK**, so `C:\VulkanSDK\1.4.341.0` has
`Bin\` and no `Bin32\`, and `HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\ExplicitLayers` — the key a
32-bit process reads — is **empty**. gl.exe is x86, so no layer is loadable. Reinstalling the SDK
does not fix it; the binaries do not exist. (Same gap that ruled out `/DELAYLOAD` in §4.5.)

Nothing about the layers is inherently 64-bit, though — they are an ordinary CMake project, and
**vcpkg carries them**:

```
cd <any directory with no vcpkg.json>          # else vcpkg is in manifest mode and refuses
vcpkg install vulkan-validationlayers:x86-windows-static-md --vcpkg-root=<repo>/vcpkg
```

The port forces `VCPKG_LIBRARY_LINKAGE dynamic`, so it produces a real layer DLL even under a
static triplet: `vcpkg/installed/x86-windows-static-md/bin/VkLayer_khronos_validation.{dll,json}`,
confirmed **i386** from its PE header. Point the loader at it with **`VK_ADD_LAYER_PATH`** — which
adds to the search path, where `VK_LAYER_PATH` would replace it and hide the driver's implicit
layers. No code change: `HasLayer` enumerates whatever the loader finds.

Two things make this usable rather than merely enabled:

- **A `VK_EXT_debug_utils` messenger, or the layer's findings are discarded.** With no messenger
  installed, "clean" and "not listening" are indistinguishable — which is why `ValidationEnabled()`
  is reported separately from the error count.
- **The messages are kept in a ring buffer and exposed as `render.validation`.** `DebugWrite` is
  `OutputDebugString` and nothing else, and attaching a debugger makes Gunlok crawl
  (`game_defects_notes.md`), so the REPL is the only practical reader. A count alone says something
  is wrong without saying what.

**Verified by deliberately breaking it**, per CLAUDE.md's rule that a harness which cannot fail
proves nothing: setting the colour attachment's `imageLayout` to `VK_IMAGE_LAYOUT_GENERAL` while
the barrier leaves it in `COLOR_ATTACHMENT_OPTIMAL` produced 10 errors naming the exact VUID, read
back through `render.validation`. Reverted, the same run reports **0 errors and 0 warnings** over
809 presented frames with the ImGui overlay active.

So Phase 1 is clean *under validation*, not merely clean to the eye — which is the standard Phase 2
needs from the first frame.

## 4.7 Phase 2a results: the state recorder

`src/D3D8Capture.cpp` now keeps a **shadow state** — a flat mirror of the D3D8 fixed-function
state, updated by the intercepted setters — and reduces each draw to two keys. Flat arrays
indexed by the D3D enum rather than maps: ~2 KB total, the API bounds the index space, and a map
would cost more than the memory it saved.

Measured on **level01, 7,406,508 draws over 19,819 frames**, with the menu for comparison:

| | menu | in level |
|---|---:|---:|
| distinct materials | 7 | **47** |
| peak materials per frame | 6 | **38** |
| distinct pipeline states | 4 | **6** |
| max *active* texture stages | 1 | **2** |
| applies of an unwitnessed block | 0 | **0** |

Four conclusions, in order of how much they change §2:

- **"Bucket by pipeline only" is safe with room to spare: six pipelines for a whole level.** That
  was the assumption most likely to be wrong, since it is what justifies deleting
  `RenderQueue_Flush`'s two-level sort.
- **The material table is 47 entries for a whole level**, and the key is a hash of exactly the
  fields `GpuMaterial` is specified to carry — so this predicts the real table's size rather than
  merely correlating with it. Nothing about it needs to be clever.
- **At most 2 texture stages are ever *active*, though all 8 are configured.** `max_texture_stage`
  is 7 because the engine sets every stage during init and reset; at draw time the first
  `D3DTSS_COLOROP == D3DTOP_DISABLE` comes at stage 2. So the übershader's stage loop runs at most
  twice, and `GpuMaterial`'s `stage_tex[8]` / `stage_ops[8]` are oversized. Measured on level01
  and the menu only — worth confirming on a level with more elaborate materials before shrinking
  the arrays, but 8 is clearly not the working figure.
- **`opaque_block_applies` is 0 across 970,451 applies.** The shadow state is provably complete:
  every `ApplyStateBlock` found a block this layer had watched being built. Phase 0b predicted
  this; Phase 2a is where it becomes load-bearing rather than encouraging.

One design detail that the numbers justify after the fact: **`ResetStats()` deliberately does not
clear the recorded blocks.** They belong to the device, not to the sample. The in-level figures
were taken after a reset, and only 4 blocks were *recorded* in that window while 970,451 applies
resolved — every one of them against a block built before the reset. Clearing the map would have
made all of those look opaque and quietly invalidated the headline result.

Still open, and unchanged from §4.1: `vertex_buffers`/`index_buffers` count **creations, not live
objects** — 25,123 VBs and 78,961 IBs during a level session, which is per-frame churn rather than
resident memory. Peak *live* geometry is what sizes the Vulkan arena, and it needs the resource
objects wrapped. That is Phase 2b.

## 4.8 Phase 2b results: what is actually resident

The buffer objects are wrapped, so `Release` reaching zero is observable and "live" can be
distinguished from "ever created". That distinction turns out to be a factor of a hundred.

Measured on level01, steady state (after `render.reset()`, 2,412 frames in 10 s):

| | cumulative | **live** |
|---|---:|---:|
| vertex buffers | 27,354 (711 MB) | **417 (6.2 MB)** |
| index buffers | 83,667 (148 MB) | **3,131 (586 KB)** |
| peak live | — | **3,551 buffers, 7.3 MB** |

So the Vulkan vertex/index arena needs about **8 MB**, not the 859 MB the cumulative figures
suggest. Everything else is per-frame churn: in steady state the game creates and destroys
exactly **1 vertex buffer and 4 index buffers per frame**, with the live count flat.

**Upload traffic: 4.7 MB and 75 locks per frame.** That sizes the staging ring, and confirms
§3's "~32 MB" guess as comfortable rather than lucky — two frames in flight need under 10 MB.

Two caveats on the numbers:

- **The whole-session peak locked figure (360 MB "per frame") is an artifact, not a
  measurement.** Frames only tick on `Present`, and the game does not present during a level
  load, so the entire load's upload traffic is attributed to one frame. The 4.7 MB steady-state
  figure is the real one; anything measured across a load is not per-frame.
- `foreign_buffers` is **0**, so every buffer reaching `SetStreamSource`/`SetIndices`/
  `ProcessVertices` is one this layer created and can safely unwrap.

### The crash, and why it took so long

Phase 2b crashed with an access violation *inside* `d3d9.dll`. The stack, once obtained, named
it immediately:

```
d3d9!CD3DHal::ProcessVertices+0x41          edx=00010000   <- garbage
d3d8!Direct3DDevice8::ProcessVertices+0x6f
d3d8!gk::d3d8::CaptureDevice::ProcessVertices+0x5b
```

**`ProcessVertices` takes an `IDirect3DVertexBuffer8` among five parameters** and looks nothing
like a resource call, so it sat in the forwarded list handing our wrapper to d3d8to9 — which
`static_cast`s it to its own concrete class and reads a proxy pointer out of the middle of our
object. I enumerated the methods taking a resource *by reading* and missed it twice.

The generator now enforces this: `check_wrapped_params()` fails the build if any method
mentioning a wrapped interface is not hand-written. It catches `ProcessVertices`,
`GetStreamSource` and `GetIndices` — all three of which I had missed or removed.

Three things about the *debugging* worth keeping, because they cost far more than the bug:

- **A bisect is worthless if the binary under test is stale.** `cmake --build --target copy`
  fails silently when an instance still holds the DLL, so four "wrapping modes" all ran the same
  older build in which wrapping was unconditional. The env-var switch existed only in the binary
  that never got deployed. **Check the deployed timestamp, not the build's.**
- **WER keeps a husk of the crashed process** — same name, 0 threads, no windows, and
  `taskkill` returns access-denied until `WerFault.exe` is killed. Anything selecting "the
  gl.exe process" then targets the husk, so focus never lands on the real game, the front-end
  menu never runs frames, and the REPL never answers. That reads exactly like a hang and is not
  one.
- **`cdb` is at `C:\Users\franc\AppData\Roaming\Binary Ninja\dbgeng\Windows Kits\10\Debuggers\x86\cdb.exe`.**
  Drive it with `-cf <script file>`, never `-c "a; b; c"` — PowerShell splits the argument on
  its semicolons and cdb ends up treating the tail as the program to launch. It resolves
  `d3d8.dll`'s own symbols from our PDB without help, contrary to what
  `game_defects_notes.md` says about needing `llvm-symbolizer`.

## 4.9 Phase 2c-i results: the geometry reaches the GPU

`src/VkResources.cpp` — a device-local vertex arena, a device-local index arena, and one
host-visible staging ring, all through VMA. Every `Unlock` stages the buffer's contents and
queues a copy; the copies are recorded at the top of the next frame with a single barrier.

Measured on level01, in level, under validation:

```
vertex: 6227 live / 6704 peak / 32768 KB    index: 583 / 587 / 8192 KB
slots live: 3464    arena full: 0    dropped: 0
uploads: 405674 (25.8 GB cumulative through a 32 MB ring, 789 wraps)
validation: on   errors: 0   warnings: 0
```

**The arena occupancy independently reproduces §4.8's residency measurement.** 6.1 MB across
3,464 slots here, against 6.2 MB across 3,548 buffers measured on the D3D side by a completely
different code path. Two ways of counting the same thing agreeing is the strongest evidence
either number is right.

Three decisions worth keeping:

- **A slot is per buffer, not per upload — and the first version got this wrong.** It
  bump-allocated on every `Unlock`, so re-locking a buffer consumed fresh arena; the game
  re-locks ~75 buffers a frame, 32 MB was gone within seconds, and `arena_exhausted` reached
  **391,173**. A D3D buffer is a fixed-size allocation whose *contents* change, so the slot is
  claimed once, written on every Unlock, and released in the destructor. That is exactly what
  makes the 7.3 MB residency figure the number that sizes the arena rather than the 859 MB of
  churn.
- **The free list coalesces, and that is not optional.** Five slots are created and five
  destroyed every frame; without merging adjacent free blocks the list grows without bound,
  which is the same leak wearing different clothes. First-fit over a sorted vector is plenty at
  3,500 live slots.
- **Nothing device-local is mapped.** The staging ring is the renderer's only mapping. On this
  machine the *entire* 16 GB device-local heap reports as host-visible (§4.3), so "it happened
  to be mappable" is a trap rather than an opportunity on a 32-bit host.

The slot is claimed lazily on first upload rather than in the constructor, because the arenas
do not exist until the renderer comes up on the first `Present` — which is after the menu has
already built its geometry.

**Still to do in 2c-ii:** the canonical vertex format (the arenas currently hold raw
per-FVF bytes) and the bindless texture table. Neither changes the memory design.

## 4.10 Phase 2c-ii (first half): one vertex format

`src/VertexFormat.cpp` converts every FVF the game uses into a single 48-byte layout on the
way into the arena — §2 calls this the highest-leverage simplification in the design, and it
is only safe because Phase 0 enumerated the FVFs instead of assuming them.

```c
struct CanonicalVertex {   // 48 bytes, every field naturally aligned
  float    pos[4];         // xyz + w: rhw for XYZRHW, 1.0 otherwise
  float    normal[3];
  uint32_t color;          // D3DCOLOR
  float    uv0[2], uv1[2];
};
```

Measured on level01, in level, under validation:

```
vertex: 10273 live / 10793 peak / 32768 KB   index: 583 / 587 / 8192 KB
slots live: 3464   arena full: 0   dropped: 0
unconvertible FVFs: 0   failed uploads: 0   foreign buffers: 0
FVFs seen at draw: 0x2, 0x112, 0x152, 0x1c4, 0x212, 0x252
validation: on   errors: 0   warnings: 0
```

**The arena grew 1.65×** — 6.08 MB of raw per-FVF bytes became 10.03 MB of canonical
vertices. That is the price of the simplification and it is worth stating plainly: a 0x252
vertex widens 44→48 bytes (1.09×), but a 0x002 position-only vertex widens 12→48 (4×), and
the weighted mix lands at 1.65. The arena is 32 MB, so there is ample room; had it not been,
the answer would be two layouts rather than a bigger arena.

**Zero unconvertible buffers**, so the six FVFs measured in §4.1 really are all the game
creates buffers with — the enumeration was complete, not merely representative.

Three decisions inside the converter:

- **`FvfStride` reproduces the arithmetic inlined in `Aw_DrawIndexedPrimitive`** rather than
  deriving it from the D3D docs, so our stride and the engine's agree by construction. The
  engine treats XYZ (+12) and XYZRHW (+16) as independent adders and ignores `PSIZE`
  entirely; a stride derived from the specification would differ on a layout it never emits,
  which is exactly the kind of divergence that only shows up as corrupt geometry.
- **`pos` is a float4 so XYZRHW survives.** Those vertices are already in screen space with a
  reciprocal w. *Which* of the two a draw uses is a per-draw property, not per-vertex, so the
  shader will learn it from the draw record.
- **Specular is dropped, deliberately.** The canonical vertex carries one colour, and D3D8
  fixed function only uses specular with lighting plus a specular material — which
  `AwMaterial`'s nine render states never enable. Recorded so the omission stays a decision.

Unsupported layouts are **refused, not guessed at**: the blend-weight FVFs, anything with
`PSIZE`, and more than two texture coordinate sets all return false and are counted, rather
than being mis-decoded into plausible-looking garbage.

**Still open in 2c-ii: the bindless texture table.** The textures are the last resource that
does not reach the GPU, and they need the `IDirect3DTexture8` chain wrapped
(`Resource8 → BaseTexture8 → Texture8`), `LockRect` captured, and the four formats measured
in §4.1 (`A4R4G4B4`, `A8`, `DXT1`, `DXT3`) mapped onto `VkFormat`. Nothing about it changes
the memory design.

## 4.11 Phase 2c-iii: the textures are wrapped, and the pixel path is not settled

`IDirect3DTexture8` is now wrapped (`Resource8 → BaseTexture8 → Texture8`), along with the
four device methods that take or return one. The game runs unchanged: 59 live textures, a
full level load, `foreign buffers: 0`, no crash.

Before building the image upload, the point was to establish **how the pixels actually
arrive**. D3D8 offers two routes into a texture's bits and only one of them is visible from
here:

- `IDirect3DTexture8::LockRect` — ours, wrapped.
- `GetSurfaceLevel` then `IDirect3DSurface8::LockRect` — the surface belongs to d3d8to9, so
  anything written through it is invisible to this layer.

Measured on level01:

```
textures live: 59   LockRect: 19722   GetSurfaceLevel: 39444
```

**`GetSurfaceLevel` runs exactly twice per `LockRect`.** That precise 2:1 ratio is the
interesting part, and it is genuinely ambiguous:

- it is consistent with the engine fetching a surface for a *query* (a `GetDesc`, a
  `CopyRects` source) around each lock, in which case `LockRect` already sees every pixel and
  the texture path is complete;
- it is equally consistent with a second write path this layer cannot see.

The counter cannot tell those apart, because it counts the call and not what is done with the
result. **What resolves it is wrapping `IDirect3DSurface8` and counting its own `LockRect`:**
zero would prove the texture path is total, non-zero would say exactly how much is missing.
That is a bigger job than it sounds - surfaces are handed out by `GetBackBuffer`,
`GetRenderTarget`, `GetDepthStencilSurface`, `CreateImageSurface`, `CreateRenderTarget`,
`CreateDepthStencilSurface` and `GetSurfaceLevel`, and consumed by `CopyRects`,
`SetRenderTarget` and `GetFrontBuffer`, so the wrapper has a much wider blast radius than the
buffer one did.

**This is the same shape as §4.1**, and it is worth naming as a pattern: the obvious capture
point looked total, and measuring the alternative route first was what stopped a renderer
being built on top of a half-complete texture path. It cost one counter.

Not yet done, therefore: the `VkImage` creation, the format mapping (`A4R4G4B4`, `A8`,
`DXT1`, `DXT3` from §4.1) and the bindless descriptor array. None of that is hard; it is
simply not worth building until the pixel source is known to be complete.

## 4.12 Phase 2c-iv: the pixel path, settled — and it was neither answer

`IDirect3DSurface8` is now wrapped, which is what §4.11 said would decide it. The answer is
**neither** of the two possibilities that section offered, and the third one changes the
upload design rather than merely confirming it.

Measured on level01, in level, under validation — and independently in `d3d9` mode, at 3×
the sample:

```
                      vulkan mode      d3d9 mode
textures live                 59             59
texture LockRect            2862           8582
GetSurfaceLevel             5724          17164     (exactly 2x)
surface LockRect               0              0
surface LockRect on a texture  0              0
CopyRects into a texture    2862           8582     (exactly 1x)
  ... untracked source         0              0
  ... whole surface          173            173
  ... sub-rect              2689           8409
SetRenderTarget on a texture   0              0
texture pools           65 MANAGED + 64 SYSTEMMEM
foreign buffers: 0   validation errors: 0
```

**The engine uploads through the classic D3D8 staging idiom**: lock a `SYSTEMMEM` texture,
write the decoded `.rim` bits into it, take a surface off each of the two textures, and
`CopyRects` the staging surface into the `MANAGED` texture it actually binds. The 1 : 2 : 1
ratio is exact in both modes and at both sample sizes, and the pool histogram is a matched
pair per texture (65 / 64, the odd one being a texture created before the first blit).

So the 2:1 that looked alarming was **two surfaces per blit, not a second lock**. Four
consequences, in decreasing order of how much they constrain the upload:

- **`IDirect3DTexture8::LockRect` does see every pixel** — `surface_texture_lock_rects` and
  `texture_render_targets` are both 0, so there is no second write path at all. But it sees
  them on the **staging** texture, which is never bound for drawing. The bits and the texture
  that will be sampled are two different objects, and `CopyRects` is the only thing that
  connects them.
- **`copy_rects_untracked` is 0**, which is the invariant the upload rests on: every blit's
  *source* is a level of a texture this layer wrapped, so the destination's new contents are
  always knowable here. A non-zero value would mean a texture whose pixels came from
  somewhere unwrapped.
- **Most blits are sub-rect, and that is the design change.** 2,689 of 2,862 carry an
  explicit rect list. The upload therefore cannot be "restage the whole surface on Unlock" —
  it has to honour the rectangle, which for `DXT1`/`DXT3` means the rects must be handled at
  4x4 block granularity. `vkCmdCopyBufferToImage` takes an offset and extent, so this costs
  nothing structurally; assuming whole-surface copies would have produced textures that were
  correct on the frame they loaded and stale afterwards.
- **173 whole-surface blits, in both runs, unchanged by sample size.** That is the static
  texture set uploaded once at level load; everything above it scales with frame count and is
  a per-frame dynamic update. Two numbers that should have differed and did not is what
  identifies the split.

The upload therefore hangs off `CopyRects`, not off `Unlock`: at the blit, read the source
level and stage exactly the rect into the destination texture's `VkImage`. Nothing needs to
remember which lock produced which bits.

### The generator found a hole the plan did not predict

`check_wrapped_params()` named all ten surface-carrying device methods as promised — including
`SetCursorProperties`, which the plan's list missed and which looks nothing like a resource
call. That is the third time the check has caught something reading the header did not.

It also found a **quieter** one, once the wrapped set was corrected to include `IDirect3D8`
and `IDirect3DDevice8`. The earlier version excluded those two on the grounds that they
"never appear as another method's parameter", which is false: every resource carries
`GetDevice(IDirect3DDevice8 **)`, and all four wrappers were forwarding it. Nothing had
called it (`resource_get_devices` is 0), so it had cost nothing yet — but a game that did
would have received the raw d3d8to9 device and every call it then made would have been
invisible here. **That is the `ProcessVertices` failure without the crash**, and it is the
worse shape of the two: capture that silently stops rather than an access violation.

Two smaller decisions inside the surface wrapper:

- **Surface wrappers are cached by inner pointer, not minted per call.** `GetSurfaceLevel`
  runs 5,724 times per level load and callers do compare surface pointers, so a fresh wrapper
  per call would make one surface look like many. The cache entry can never be stale because
  the wrapper holds a reference on `inner_` for exactly as long as it is in the map, so
  d3d8to9 cannot destroy a surface and reuse the address underneath it. On a cache hit the
  incoming reference is surplus and is released rather than leaked — `surfaces live: 0` after
  a full session is what says that balances.
- **A surface holds a reference on its owning texture wrapper.** The game releases textures
  and the surfaces taken off them independently, so without it a surface outliving its texture
  would read a destroyed object every time it checked whether it was a texture level. D3D8's
  own surfaces hold a container reference for the same reason.

## 4.13 Phase 2c-iv: the textures reach the GPU

`VkImage` per bound texture, fed through the existing staging ring, plus
`render.verify_textures()` — a GPU readback that compares each mip level against the D3D
texture it came from. Measured on level01, in level, under validation:

```
images: 53 live / 59 created   112 MB
image uploads: 3267 (407 MB cumulative)
unsupported formats: 0   unaligned rects: 0   dropped: 0
validation errors: 0
verify_textures: 17/17 mip levels at the menu, 147/150 in level
```

Format mapping, from the four in §4.1: `DXT1`→`BC1_RGBA_UNORM_BLOCK`, `DXT3`→`BC2_UNORM_BLOCK`,
`A8`→`R8_UNORM` with an `{ONE,ONE,ONE,R}` view swizzle, and `A4R4G4B4`→**`R8G8B8A8_UNORM` with
a CPU expansion**. That last is deliberate: `VK_FORMAT_A4R4G4B4_UNORM_PACK16` is optional even
in Vulkan 1.3, so mapping it natively buys a per-device support matrix and a fallback anyway.
It costs 2× memory on 58 of level01's textures, which is most of why the image set is 112 MB.

### The content check is the point, and it found four defects the counters could not

Every counter can read perfectly while an image holds the wrong bytes — which is exactly the
trap §4.11 was written to avoid, one layer down. So the upload shipped with a verifier, and it
paid for itself immediately. In the order they were found:

- **Staging offsets were unaligned.** `vkCmdCopyBufferToImage` requires a `bufferOffset` that
  is a multiple of the texel block size — 16 for BC2, 8 for BC1 — and the ring bump-allocated
  from wherever the last buffer copy left off. Validation rejected essentially every
  compressed copy. Every staging region is now 16-byte aligned.
- **The ring could wrap over un-recorded data.** Staged bytes are only read when
  `RecordUploads` puts them in a command buffer at the top of a frame. Steady state is ~11 MB
  between frames against a 32 MB ring, which is why this never showed — but **a level load
  stages 360 MB between two Presents**, because the game stops presenting while it loads.
  That is eleven wraps inside one batch. A batch that would exceed the ring is now submitted
  and waited for on its own command buffer; it fires ~29 times per level load.
- **`ReleaseFrameStaging` was never called**, so the ring also wrapped over regions a frame in
  flight was still reading. The `frame_start[]` bookkeeping existed and nothing consumed it.
  It is now a real check, and it stalls ~1080 times per session — which is both the proof the
  race was real and a cost to revisit when drawing lands (see Known gaps).
- **`ReadWrapMode()` was defined and never called**, so `GKPLUS_WRAP_BUFFERS` had never done
  anything. A bisect run with it set would have cleared the buffer wrappers of a fault they
  were still causing. Both bisect knobs are now read in the `D3D8CaptureSystem` constructor.

And one in the verifier itself: the images were created without
`VK_IMAGE_USAGE_TRANSFER_SRC_BIT`, so every `vkCmdCopyImageToBuffer` readback was invalid.
It reported mismatches that were its own, and two hours went into chasing them as if they were
real. **A verifier needs verifying**, and what caught it was reading `render.validation` while
the check ran rather than only after a plain frame.

### Where the pixels are read from, which is not where §4.12 predicted

§4.12 concluded the upload should replay each blit's source rectangle. That is what was built,
and it is wrong: it left one texture per session visibly stale, on a case where both blits were
whole-level copies from a same-sized source, so no rectangle arithmetic was even in play.

**Reading the destination after the blit is right where reading the source before it is not** —
17/17 against 16/17 on the same frame of the same scene. The mechanism was not pinned down, and
the destination read makes it moot: it reads the *result* of the copy rather than predicting it,
so it holds whatever d3d8to9 does in between — including any perturbation from our own read-only
lock of the source, which is the leading suspect. Two things were ruled out first, at a run
each: d3d8to9's copy **is** byte-exact (16 whole-level blits sampled, 0 differing), and the ring
race above is real but not this.

Then the same shape one level down: a texture with **both** its levels blitted and both mirrored
still came back with level 1 wrong. So something writes mip levels outside the blit that names
them. Re-reading the whole chain per blit converges regardless of which level moved, and took
level01 from 126/150 to 148/150.

Two variants were measured and rejected, which is why the simple version is the one in the tree:
deferring the read to the next `SetTexture` is far worse (**85/150**), because a texture blitted
after its last bind is then never re-read at all; doing both costs a second whole-chain read per
blit and measured the same as the blit-time read alone.

**The residual is 1-3 mip levels in 150, always level ≥1, always small, always fixed by a
re-upload.** Level 0 matches everywhere. It is a real open item rather than a rounding
artifact - `render.verify_textures()` is the detector, and Phase 3 will make any residue visible
at distance.

### Seeding, and why it is not optional

An image is created and populated from the texture's own contents the first time that texture
is **bound**, not the first time it is blitted into. Blits alone leave two holes, both measured:
a texture written before the renderer came up (it starts on the first `Present`, long after the
menu loads its art), and a texture bound and drawn but never blitted into — level01 creates 65
MANAGED textures and only 48 ever receive a blit. Seeding works only because every texture the
game binds is MANAGED (§4.12's pool histogram) and therefore lockable; a DEFAULT-pool texture
would not be, and there are none.

The 64 SYSTEMMEM staging textures never get an image, which is the point of tracking the pool.

## 4.14 Texture provenance: every image knows its `.rim`

The capture layer sees pixels with no idea what they are — a D3D texture carries no name, and
the engine decodes its `.rim` files itself. That is fine for reproducing the frame and useless
for modding: nobody can write "replace the water texture" against a wrapper pointer that differs
every run. So the name is recovered while the link still exists, **before** the bindless
descriptor array hardens a material key around pointer identity. Retrofitting it afterwards
would mean changing the key, hence the material table, hence the shader interface.

The link exists in exactly one place. `AcquireRimTexture` @ 0x005a15b0 is the engine's whole
texture-acquire path — 31 call sites, from `InitConsole` to `BuildShapeVertexBuffers` — and the
0x34-byte record it mints **is** `AwTexture` (§6): the strdup'd path at +0x2c, and at +0x00 the
`IDirect3DBaseTexture8 *` the loader stores once the texture exists. With the capture layer
installed that pointer is our own wrapper, so the join is a pointer compare.

```
53 images, 52 named   59 of 59 textures resolved
Ground\escape Ground vary 4.RIM   units\baddies3.RIM   bitmaps\lava.rim
bitmaps\scanner.rim   Units\Custom Screen bg 1k 01.RIM   units\alpha junk.rim
```

The one unnamed image is a 256×256 with no cache record, and there is always going to be a
residue like that — an engine-internal or procedural texture is not a moddable asset.

Two things worth keeping:

- **It is a per-frame sweep, not a lookup, and that is a timing result rather than a taste.**
  Resolving once when a texture is first bound named **5 of 53** — not because the join was
  wrong but because the record is minted with its D3D pointer null and the loader fills it in
  much later. Sweeping the ~130 records each `Present` converges within a frame of the store,
  and costs nothing measurable (60 fps unchanged).
- **`RimJoinHistogram()` is why that took one run instead of an afternoon, and it stays.** When
  5 of 53 resolved, the two candidate explanations were "the field is not +0x00" and "we looked
  too early", and no counter separated them. Scanning every offset of every record against the
  set of live wrappers answered it immediately: `+0x00=57`. A diagnostic that reports *where* a
  join actually lands is worth more than one that reports how often it failed.

`render.textures` is the surface: index, name, dimensions, levels, format and bytes per image.
That index is `TextureImage::index`, which is already the bindless slot.

## 4.15 The bindless descriptor set

One set for the whole renderer: samplers at binding 0, sampled images at binding 1, both
`UPDATE_AFTER_BIND | PARTIALLY_BOUND`, the images additionally `VARIABLE_DESCRIPTOR_COUNT`.
Measured on level01, under validation:

```
bindless: up   4096 image slots   5 samplers   64 writes   out of range: 0
validation errors: 0
```

**Five samplers for the entire game.** D3D8 has no sampler object — filtering and addressing are
texture-stage state — so the combination has to be collapsed, and the question was how many
survive. Interning them at draw time from the shadow state (the same discipline the FVF
enumeration used, rather than pre-creating one per conceivable state) answers it: five. The
array is sized 64 and will not be troubled.

`TextureImage::index` **is** the descriptor index. There is no second mapping to keep in step,
which is the whole reason the slot is assigned at image creation, and it is what makes a
descriptor a write-once affair rather than per-frame work — 64 writes for a whole level session.

Four things that are easy to get wrong here:

- **`VARIABLE_DESCRIPTOR_COUNT` is legal only on the last binding.** That is why the images are
  binding 1 and the samplers binding 0, and not the other way round.
- **A destroyed image's descriptor is deliberately left stale.** `PARTIALLY_BOUND` makes that
  legal to hold; what would be undefined is *reading* it, and nothing can — a draw only names a
  slot through the texture the shadow state has bound, which is live by definition, and the slot
  is rewritten when reused. This stops being true the moment a draw record outlives its texture,
  which is a Phase 3 concern and is noted at the site.
- **`UPDATE_AFTER_BIND` is not a nicety.** The engine creates textures mid-frame, after the set
  would already be bound; without it every such creation would have to defer to a frame boundary.
- **A non-dispatchable Vulkan handle is `uint64_t`, not a pointer**, so on 32-bit it is *wider*
  than one. `VkResources.h` returns the set and layout as `uint64_t` to keep mentioning no Vulkan
  type; typing them as `void *` the way `RecordUploads` takes its command buffer truncates the
  handle in half. `VkCommandBuffer` gets away with it only because it is dispatchable and really
  is a pointer.

## 4.16 Phase 3: the game's geometry, drawn by Vulkan

`src/VkDraw` plus `src/shaders/world.slang`. The capture layer reduces each `DrawIndexedPrimitive`
to a `DrawItem`; the renderer walks the list once per frame, binding nothing per draw. Measured
on level01:

```
world pipeline: up
draws: 205 this frame / 502 peak / 542021 total
skipped: 350853 user-pointer, 0 topology, 3785 no arena slot, 0 no transform
validation errors: 0
```

The A/B against `GKPLUS_RENDERER=d3d9` is the verification, and it is the same scene: same
camera, same rock faces, same walkways, same textures. **This is the first thing that reads what
Phase 2 built** — the arenas, the canonical vertex layout, the images, the samplers and the
bindless set are all now proven by a picture rather than by a counter.

Remaining differences from the D3D path, all deferred rather than unexplained: no fog (level01 is
a fogged cavern, which is most of what the two shots differ by), no D3D lighting, and the units
are missing because they are user-pointer draws.

### Slang, not GLSL

Khronos's own language now, it ships in the SDK, and it earns its place here for two reasons: one
file holds every entry point of a pass, so a push constant block shared across stages cannot
drift between two files; and its generics are what the übershader's stage ops want. SPIR-V is
compiled offline by `src/gen-shaders.py` and the header is checked in, so `d3d8.dll` depends on no
shader toolchain — the same argument that makes the renderer reach Vulkan through volk.

Two Slang specifics that cost a run each:

- **`SV_VertexID` carries D3D semantics** — the raw index, base vertex excluded — so it compiles
  to `gl_VertexIndex - gl_BaseVertex` and declares the `DrawParameters` capability whether or not
  a base vertex is ever non-zero. The device feature must be enabled or the module is rejected
  outright. It also means the base vertex is folded in on the CPU and `vkCmdDrawIndexed` gets a
  `vertexOffset` of 0; passing it to Vulkan as well would add it twice.
- **`mul(v, M)` with `-matrix-layout-row-major`** is D3D's row-vector convention, so the CPU
  uploads `world*view*projection` exactly as the game composes it, with no transpose anywhere.

### Three bugs, and what actually found each

The first frame drew the level smeared into long streaks converging on a point. Three defects
were stacked, and the order they came out in is the useful part:

- **The vertex arena aligned slots to 16 bytes while the canonical vertex is 48.** A draw
  addresses its buffer as `slot.offset / 48`, so any slot not starting on a whole vertex pulled
  the wrong ones — each triangle getting one right vertex and two from elsewhere, which is
  exactly what a streak is. The arena now takes a per-arena alignment and the vertex one is
  `sizeof(CanonicalVertex)`. **This bug was invisible to every counter**: the uploads were
  correct, the residency was correct, the readback verification passed. Only a picture showed it.
- **The shader's vertex struct was `{float4, float3, uint, float2, float2}`**, which is the
  obvious spelling of `CanonicalVertex` and a trap: a `float3` followed by a scalar lays out
  differently under std140, std430 and scalar rules, and nothing says which one Slang picked for
  a `ConstBufferPointer`. It is three `float4`s now — 48 bytes with fields at 0/16/32 under every
  rule there is — so the agreement with the C struct is structural rather than a bet. This was
  the one that mattered; fixing it turned noise into the level.
- **The cull winding is `FRONT_FACE_CLOCKWISE`**, which is the opposite of the intuition. The
  projection negates Y for Vulkan's clip space, so winding "obviously" reverses — but D3D's clip
  space is left-handed too, and the reasoning that accounts for only one of the two gets it
  backwards. `COUNTER_CLOCKWISE` culls the ground and most of the level.

**Two wrong guesses preceded the fix, and both were matrix conventions** — the streaks looked like
a transpose, and testing `mul(M, v)` against `mul(v, M)` cost two full rebuild-and-load cycles to
eliminate a hypothesis that was never the problem. That is the argument for §4.17.

## 4.17 RenderDoc, driven from the REPL

`src/VkCapture` loads `renderdoc.dll` and calls `StartFrameCapture`/`EndFrameCapture` around the
renderer's own frame; `render.capture()` arms one from the REPL and the `.rdc` lands beside the
game. Off unless `GKPLUS_RENDERDOC` is set.

**The in-app API rather than the hotkey, for two reasons specific to this process.** The game has
a live D3D9 device from d3d8to9 at the same time as our Vulkan one and RenderDoc captures one API
per frame, so the hotkey may grab the wrong one; and GkPlus is a DLL proxy, so there is no
"launch this exe" for the UI to hook. Calling it ourselves from inside the Vulkan frame is
unambiguous on both counts.

It must be loaded **before the Vulkan instance exists** — RenderDoc captures by inserting a layer,
and a layer cannot be added to an instance already created — so `vulkan::Initialize()` calls
`LoadRenderDoc()` as its first act. That is also why it cannot go in `DllMain`: it is a
`LoadLibrary`, which deadlocks under the loader lock.

Two practical notes: the x86 build is at `RenderDoc\x86\renderdoc.dll`, *not* the one beside the
UI, and loading the x64 one fails with a bare "not a valid Win32 application" that says nothing
about bitness. And `renderdoccmd` cannot dump a capture's contents without the UI, so a capture
is something to hand to a person — though `renderdoccmd replay` *does* load one headlessly,
which is enough to tell a good capture from a bad one without leaving the loop.

### Opening a capture: two separate failures, both reported as out of memory

Both present as `Failed allocating memory, VkResult: VK_ERROR_OUT_OF_DEVICE_MEMORY`, neither is
about running out of VRAM, and they are independent — the first stops a capture opening at all,
the second lets it open with silently wrong contents.

**1. The REPLAY process must be 32-bit — the launcher's bitness is irrelevant.** Our captures
come from `gl.exe`; replaying a buffer-device-address capture means reproducing the exact
`VkDeviceAddress` values the 32-bit driver handed out, which an x64 replay cannot place. Most
32-bit applications never touch BDA, which is why this is ours specifically.

**Launching the game from `x86\renderdoccmd.exe` does not fix it**, which is the obvious thing
to try and the thing that wastes the time: the RenderDoc UI is x64 and replays *in its own
process*, so where the capture came from changes nothing. Measured on one file, three ways:

| replay | result |
|---|---|
| `renderdoccmd.exe` (x64), local | `VK_ERROR_OUT_OF_DEVICE_MEMORY` |
| `x86\renderdoccmd.exe`, local | loads |
| `renderdoccmd.exe` (x64) → `--remote-host localhost` against a 32-bit `remoteserver` | loads |

So the fix is RenderDoc's remote-server mechanism, which is what the UI uses too. The full
procedure, because the middle step is not discoverable and stops the rest working:

1. **Tools → Manage Remote Servers…**, and select `localhost` (it is in the list by default).
2. **Set its Run Command**, which is empty by default:
   ```
   "C:\Program Files\RenderDoc\x86\renderdoccmd.exe" remoteserver
   ```
   Without it the dialog reports *"Remote server not running - no start command configured"*
   and the **Run Server** button does nothing. This is the step that looks optional and is not.
3. **Run Server** — or run that same command in a terminal, which is all the button does.
4. Close the dialog and set the status bar's **Replay Context** to `localhost`; it reads
   `Replay Context: localhost` rather than `Local`.
5. Open the capture.

Do not hand-edit `%APPDATA%\qrenderdoc\UI.config` while the UI is running — it rewrites the file
on exit. The host list lives there under `RemoteHostList`, with `runCommand` as the field above,
which is worth knowing when scripting a machine from scratch.

Headless, the same path is `renderdoccmd.exe replay --remote-host localhost` against a running
32-bit `remoteserver`, which is the third row of the table and is what was verified end to end.

**2. An in-level capture is incomplete at full heap sizes.** RenderDoc snapshots each resource's
initial state by allocating a mapped readback buffer **inside the game**, and `gl.exe` has 2 GB
of address space already shared with the game, the driver and QuickJS. Fifteen of those
allocations fail in level, and those resources are captured uninitialised — the capture still
opens, and shows plausible-looking rubbish. The evidence is in the *capture-side* log, which is
easy to mistake for a replay log because the error text is identical:

```
%TEMP%\RenderDoc\RenderDoc_app_*.log
  vk_initstate.cpp(350) - Error - Couldn't allocate readback memory
```

`GKPLUS_VK_HEAPS=small` cuts the arenas and rings to just above level01's measured peaks — 82 MB
of buffers down to 24 MB — and takes that count to **0** with `arena_full`, `scratch_exhausted`
and `dropped` all still 0. A menu capture is clean either way.

**It is deliberately not implied by `GKPLUS_RENDERDOC`.** Shrinking the heaps changes what the
renderer does: the same level goes from 29 mid-batch staging flushes to 2,540 and from 1,080
stalls to 2,244, because a smaller ring wraps more often. A debugging tool that quietly alters
the behaviour under investigation means the frame being captured is not the frame that
misbehaved. `StartResources` logs a warning instead when RenderDoc is loaded at full size, and
`render.vulkan_report` says which mode is in force.

This is §3's **"32-bit address space is the real constraint, not GPU capability"** arriving
exactly where it was predicted to, one layer further out than expected — not in the renderer's
own allocations but in a tool's.

## 4.18 User-pointer draws

`DrawPrimitiveUP` and `DrawIndexedPrimitiveUP` hand D3D their vertices inline — 350,000 of them a
session on level01, and they are the text, the particles, the in-game menus and **the units**.
There is no buffer, so there is nothing for a `BufferSlot` to attach to and nothing whose
`Release` says when the data dies.

```
draws: 363 this frame / 660 peak / 809133 total     (542021 before)
skipped: 6158 topology, 3425 no arena slot, 0 no transform, 0 unconvertible, 0 scratch full
scratch: 4096 KB vtx (peak 800 KB) + 1024 KB idx (peak 66 KB) per frame   exhausted: 0
index binds: 19930     validation errors: 0
```

The characters now appear where the d3d9 reference has them, which is the check that matters.

**The scratch is host-visible and mapped, and that is the one deliberate exception to "nothing
device-local is ever mapped" (§4.9).** The trade is the opposite of the arenas': this data is
produced on the CPU, is different every frame and is read once, so staging it would mean a copy
and a barrier to move bytes the GPU glances at. The arenas hold a level's worth of geometry read
every frame and written rarely, which is why they are device-local and unmapped. Converting
straight into the mapped pointer means a user-pointer draw costs one `ConvertVertices` and no
Vulkan work at all.

One slice per scene, rotated at the *end* of a frame. It was originally reset from the same place
`ReleaseFrameStaging` is called, on the reasoning that the frame's fence is the same proof for
both — and it is not, because the staging is written by the renderer and the scratch by the game.
**§4.22 is that defect**; read it before touching the allocator.

Three details that are easy to get wrong:

- **The stride comes from the call, not from the FVF.** A user-pointer draw states
  `VertexStreamZeroStride` explicitly and is entitled to pad; assuming the two agree is an
  assumption with nothing behind it. `ConvertVertices` now takes an optional source stride and
  refuses one *smaller* than the FVF implies, which would mean the promised fields do not fit.
- **`MinVertexIndex + NumVertexIndices` is the span to copy**, not `PrimitiveCount * 3`. The
  vertex pointer is biased so that index 0 is its first vertex, so copying the whole span means
  the indices need no rebasing.
- **The index binding is tracked, not reissued.** Arena and scratch indices are different
  buffers, so the bind changes wherever the list crosses between them — 19,930 binds over ~3,000
  frames, about 6 a frame, which is the alternation and not per-draw churn. Both sources make
  their first-index absolute from the start of their own buffer, which is what keeps it that low.

`skipped_topology` is 6,158 and non-zero for the first time: those are the line lists and
triangle strips among the user-pointer draws, which the single triangle-list pipeline cannot take.

## 4.19 The second texture stage, and the pipeline states

The scene was flat and bright against the d3d9 reference, and the plan said the gap was **fog and
lighting**. It was neither. It was the **second texture stage** and the **alpha test and blend**,
and getting that wrong for three sessions is the argument for the method below rather than for
reading more of the renderer.

### Ruling things out with the game's own renderer

The instrument is two environment variables that make gl.exe draw the scene *without* one thing
the Vulkan path is missing, and nothing else — `GKPLUS_NO_LIGHTING=1` forces `D3DRS_LIGHTING`
off in the forwarded call, `GKPLUS_NO_STAGE1=1` forces `D3DTSS_COLOROP` to `DISABLE` on every
stage past the first. Both modify only what is forwarded; the shadow state still records the
truth, so `render.state` does not start lying while one is set. A block records what is
forwarded, so a block built while `NO_STAGE1` is set carries the disable and `ApplyStateBlock`
cannot put the stage back.

Same camera, same frame, mean absolute difference against the unmodified d3d9 render:

| | mean/255 | pixels differing by >32 |
|---|---:|---:|
| d3d9 vs d3d9 with **lighting off** | **0.08** | **0.1%** |
| d3d9 vs d3d9 with **stage 1 off** | 6.48 | 11.6% |
| d3d9 vs Vulkan, stage 0 only (before) | 15.85 | 24.2% |
| d3d9 vs Vulkan, + stage 1 | 9.08 | 10.5% |
| d3d9 vs Vulkan, + pipeline states | **7.77** | **10.3%** |

The first row is the whole argument. It is also the *noise floor* of the comparison — two
separate launches, level loaded from the REPL, briefing dismissed, twelve seconds to let the
intro camera settle — so the procedure is reproducible to 0.08/255 and every other number in the
table is signal.

Three measurements, in the order they were taken:

- **Fog is never enabled.** `D3DRS_FOGENABLE` is 0 for every draw of a session and
  `D3DRS_FOGCOLOR`, `FOGTABLEMODE` and `FOGVERTEXMODE` are never set to anything else. The
  cavern's fog is *content*, not D3D fog: `world.fog` reports enabled with a black colour, and
  the engine renders it as a texture stage.
- **Fixed-function lighting is enabled for 97.6% of draws and changes nothing.** 432,331 of
  442,745, with `SetLight` 86,424 times and `LightEnable` 3.7 million — and the picture with it
  forced off is identical to 0.08/255. `DIFFUSEMATERIALSOURCE` is `D3DMCS_COLOR1`, the global
  ambient is 0 and the lights are disabled again by the time anything draws, so the result is
  the vertex diffuse either way. Implementing D3D lighting would have been a week of work for
  no pixels.

  **This is wrong, twice over — see §4.20 for the HUD and §4.25 for the rest.** The same A/B
  re-run measures 6.54/255, not 0.08. "The lights are disabled again by the time anything draws"
  is the error: `LightEnable` runs 118 million times a session, around individual draws, and
  `render.state` samples *between* frames. The snapshot is real and says nothing about the state
  at a draw.
- **Stage 1 is the whole gap**, and `NO_STAGE1` on the game's own renderer reproduces our
  picture: bright, washed out, with the ceiling structure that should be hidden clearly visible.

### What the stages actually are

`render.state` now prints every texture-stage configuration drawn with, and how often. Six for a
level01 session, and the two-stage ones are what matters:

```
53576 draws  2 stages | 0: c  4( 2, 0) a  4( 2, 0) uv0 tex | 1: c 13( 2, 1) a  3( 2, 1) uv1 tex
32461 draws  1 stage  | 0: c  4( 2, 0) a  4( 2, 0) uv0 tex
21904 draws  2 stages | 0: c  4( 2, 0) a  4( 2, 0) uv0 tex | 1: c  8( 2, 1) a  3( 2, 1) uv1 tex
  888 draws  0 stages
  296 draws  1 stage  | 0: c  3( 2, 0) a  3( 2, 0) uv0 ---
```

Stage 0 is always `MODULATE(TEXTURE, DIFFUSE)`. Stage 1 is `BLENDTEXTUREALPHA(TEXTURE, CURRENT)`
(op 13, 49% of draws) or `ADDSIGNED(TEXTURE, CURRENT)` (op 8, 20%), on the second UV set, with
its alpha `SELECTARG2` — i.e. the stage keeps the diffuse texture's alpha. `bitmaps\LEVEL01.rim`
is the lightmap; the second UV set §4.1 measured on FVF `0x252` is its coordinates.

**There is no lightmap, and `bitmaps\LEVEL01.rim` is the minimap — see §4.51.** The two ops are two
different subsystems: op 13 is the **fog of war** (a 256x256 `D3DFMT_A8` grid) and op 8 is the
**chrome** pass §4.49 later identified. The name here was a join artefact; that file is a 512x512
DXT1 top-down picture of the level, and it is not even resident while a level is up. The *shape*
of this section's finding stands — the second stage was the whole flat-and-bright gap — and only
the label was wrong.

The shader implements twelve `D3DTEXTUREOP`s rather than all twenty-six, and the CPU counts a
draw naming one it does not — `unsupported_stage_op`, 0 on level01. Two stages, not eight:
§4.7's "at most 2 active" is what sizes the push constants, and a third would not fit in 128
bytes.

**The stage count comes from `D3DTSS_COLOROP`, not from what is bound.** 2,160 draws a frame
have every stage disabled and a texture still bound at stage 0; the old code keyed off the
binding and sampled it. Fixed by construction here.

### Three defects the second stage exposed, each invisible before it

- **`D3DFMT_A8` was swizzled to `{ONE, ONE, ONE, R}` and must be `{ZERO, ZERO, ZERO, R}`.** D3D
  reads a channel a format does not carry as 0 — alpha is the exception, reading as 1 — so an
  alpha-only texture samples as `(0, 0, 0, a)`. White RGB is the *identity* under stage 0's
  `MODULATE`, which is why §4.16 could not see it; under `BLENDTEXTUREALPHA` it inverted the
  scene, fading the distance to white instead of to black. The texture readback (§4.13) cannot
  catch this either: it compares stored bytes, and the swizzle is in the view.
- **A texture bound only by `ApplyStateBlock` never got an image.** `EnsureTextureImage` hung
  off `SetTexture`, and a block replay sets the shadow state through `ApplyOp` while touching no
  `Set*` method at all — which is the whole reason blocks are replayed (§4.7). 83,176 draws a
  session bound a DXT1 lightmap at stage 1 that had no image and sampled white, which under
  `ADDSIGNED` *brightens* by 0.5. The fix is to give the texture its image in `ApplyOp`, so
  "bound, however it was bound" is one code path. `stage_texture_unresolved` is the counter, and
  it must be 0.
- **A stage whose texture does not resolve samples white, and white is not neutral.** It is the
  identity for `MODULATE` and nothing else. That is why the counter above is a must-be-0 rather
  than a diagnostic.

### The pipeline states, and the alpha test

`PipelineKey()` had counted six distinct pipeline states for a level since §4.7 and thrown the
values away. A count cannot be implemented against, so `render.state` now prints them:

```
       atest ref func  blend src dst   z zwrite zfunc cull
80709      1  31    7      1   5   6   1      1     4    3
27232      0  31    7      0   5   6   1      1     4    3
 1184      0  31    7      1   1   2   1      0     4    1
  592      0  31    7      1   5   6   0      0     4    1
  296      0  31    7      0   5   2   1      1     4    3
```

**73% of draws have both the alpha test and alpha blending on**, `SRCALPHA`/`INVSRCALPHA` with
`GREATEREQUAL 31`. A single always-opaque pipeline draws three draws in four with the wrong
coverage, which is what made the units white blobs with halos. Five `VkPipeline`s, built on
first sight of a state and cached; `pipeline_failures` must be 0.

Three decisions:

- **The alpha test is a `discard`, not pipeline state.** It varies per draw under the same blend
  and depth settings, so putting the reference in the pipeline key would multiply the pipeline
  count by the number of distinct references for nothing. It rides in `flags` — `D3DCMPFUNC` in
  bits 0..3, `D3DRS_ALPHAREF` in 8..15 — and compares in 0..255 units, because `>= 31/255` and
  `>= 31` differ by a rounding step and the test is exactly where that shows.
- **The draw list is not sorted by pipeline.** It is recorded in the order the game issued it,
  which is what makes blending come out right with no work here: `RenderQueue_Flush` has already
  state-sorted the opaque draws and put the back-to-front list last (`rendering_notes.md` §4).
  Sorting to save binds would undo that. 12,207 binds against 595,119 draws is what the game's
  own ordering already gives.
- **`D3DCMPFUNC` and `VkCompareOp` are the same eight comparisons in the same order, one apart.**
  Written as the subtraction it is, with the range checked, rather than an eight-case switch that
  would only restate it. `D3DCULL_CW` is the existing `D3DCULL_CCW` rule with the front face the
  other way round — the winding convention itself was measured in §4.16 and is not re-derived.

### What is left, precisely

7.77/255 against a noise floor of 0.08, and it is **not** uniform: the near rock matches to
within 1.5/255, the mid-distance walkway is uniformly ~+8 on every channel, and the far rock is
+8 red / +13 green / +23 blue. Amplifying the difference shows texture detail on every lit
surface rather than a flat offset. So the residual grows with distance and desaturates toward
grey — the shape of a fog or lightmap term that is not attenuating enough — and it is the next
thing to chase. Ruled out already: mip drift (`render.verify_textures()` is 155/158, all three
8x8 level-1 mips), the swapchain being sRGB (format 37 is `R8G8B8A8_UNORM`), and D3D lighting.

**It is D3D lighting** — §4.25. The description above is accurate and the attribution is not:
lit-against-unlit reproduces this residual's shape and magnitude region for region. It was ruled
out on the null A/B corrected in the bullet above.

Validation is clean with the five pipelines and the alpha test: 0 errors, 0 warnings.

## 4.20 The HUD is green, and it is fixed-function lighting after all

Reported from playing, not from a counter: **the character portraits render greyscale where the
game shows them monochrome green.** Chasing it corrected §4.19's headline claim, so read this
next to it rather than instead of it.

### The screenshot was missing a third of the frame

`GetClientRect` reported 418x312 while the swapchain was the real 628x468, because gl.exe is not
DPI aware and Windows hands *it* virtualized coordinates. `PrintWindow` renders at the window's
own resolution, so a bitmap sized from that rect keeps only the top-left two thirds — and the HUD
is in the upper right. **Every screenshot in §4.19 was cropped, and nothing about them looked
wrong.** One `SetProcessDPIAware()` in the capturing process fixes it: a DPI-aware caller asking
about a non-aware window gets the physical rect.

The §4.19 numbers stand as comparisons — both sides were cropped identically — but they were
never the whole frame, and the 7.77 there is 9.62 measured over all of it.

### §4.19 said lighting contributes nothing. That was true of the frame it measured.

`GKPLUS_NO_LIGHTING=1` changed the picture by 0.08/255 — on a frame **with no HUD up**. Re-run
with the HUD visible, the same switch turns the panel from green (2.1, 23.2, 0.2) to grey
(47.8, 46.3, 45.9), which is exactly what this renderer was drawing. So fixed-function lighting
does contribute, and the earlier conclusion was an over-generalisation from a sound measurement:
**an A/B is only evidence about the pixels that were on screen when it ran.**

What it does is narrower than "lighting", though, and the narrowness is the whole implementation:

- The HUD's geometry carries **no vertex colour at all** — FVF `0x112` and `0x212`, position,
  normal and texture coordinates. `ConvertVertices` gives those a neutral opaque white, which is
  the identity under `MODULATE` and therefore renders the source art in its own colours: a
  perfectly plausible picture that is not the game's.
- With lighting on, D3D does not use the vertex colour there. It computes
  `emissive + ambient_material * global_ambient + SUM over lights(...)`.
- **Gunlok enables no light for those draws** — measured per draw, 0 enabled on every one — and
  its global ambient is 0. The sum is empty and the whole expression collapses to the material's
  **emissive**, which for the HUD is `0x008000` and `0x00ff00`. Green.

So the implementation is one packed colour per draw in the push constants, not a lighting loop:
computing an empty sum would be elaborate machinery around a term that is always zero. Alpha
comes from the material's diffuse, which is where the panel's 0x80/0x99/0xcc translucency lives.

**It applies only where the vertex has no colour of its own**, and that boundary is measured
rather than cautious. Applying the same collapse to geometry that *does* carry a vertex colour
rendered the terrain very nearly black — the world is lit by real lights (94% of draws have one
enabled), so its sum is not empty. For that geometry §4.19's A/B already showed the lit result
and the vertex colour to be the same picture to 0.08/255, so keeping the vertex colour is a
measurement too, not a shortcut.

`lit_draws_with_lights` counts what falls through both tests: lighting on, no vertex colour, and
a light enabled after all — 845 in a session against ~2M draws. Note where it sits, because the
number means nothing without it: the vertex-colour test comes *first*, so this is not "94% of
draws are unlit by us". It is the narrow remainder where the collapse would have been wrong and
the vertex default is used instead, and a level on which it is large is a level that needs the
real light sum.

### Two things that were never drawn at all

Both found by asking why a draw was skipped rather than by looking:

- **A buffer whose only `Unlock` happened before the renderer existed never got an arena slot**,
  and is never unlocked again — 9 buffers, ~4.5 draws a frame. A slot was claimed on first
  Unlock, and `vulkan::ResourcesReady()` is false until the first `Present`. They now seed
  themselves from their own contents at draw time (`SeedFromContents`), the same shape and the
  same justification as `EnsureTextureImage`: being drawn is the definition of needing to be
  resident, and the buffer is the only thing that still knows what is in it. The read lock is
  safe for what it is used on and `pool_` is checked rather than assumed.
- **Non-triangle-list topologies** — 3 strips and 1 line list a frame.

`skipped_no_slot` is 0 now, and its breakdown (foreign stream / unslotted vertices / unslotted
indices / 32-bit index buffer) is what turned "14,644 draws skipped" into a single named cause.

### The topologies are OFF by default, and that is the interesting result

Drawing them **made the picture worse**. Measured against d3d9 on the same frame, mean absolute
difference over the whole frame:

| | |
|---|---:|
| d3d9 vs d3d9 (noise floor) | **0.07** |
| neither seeding nor the lit colour nor topologies | 9.62 |
| seeding + lit colour | **8.34** |
| + topologies | 10.64 |

So seeding and the lit colour are worth 1.28 together, and the topologies cost 2.30. Something
about how a strip or a line list is drawn is wrong — winding, or geometry that should not be
visible at all — and shipping them would have been a regression dressed up as completeness
("every primitive type now draws"). They are opt-in through `GKPLUS_VK_TOPOLOGIES=1`, and
finding out why is the next thing.

**That bisect only worked at the 12-second settle.** At 90 seconds the two renderers run at
different frame rates, so the game is in a *different state* by the time the shot is taken and
the frames are not comparable — the first attempt at this bisect produced five numbers with no
pattern, and a rock that appeared and vanished between runs of identical code. Check the
d3d9-vs-d3d9 noise floor at whatever settle you use before trusting any difference: 0.07 at
twelve seconds, and not reproducible at ninety.

### The shadow state now starts at the API's defaults

`D3DRS_COLORWRITEENABLE` is what forced it: its default is all four channels and its zero means
write *none* of them, so the moment it joined the pipeline key, every draw before the game first
set it would have rendered nothing. It never sets it, so that is every draw. Zero is a legal
value for most of these states rather than an obviously-missing one, which is what makes the
whole class dangerous — `InitialiseShadowState` sets the documented initial value for every
state the renderer reads, and adding one to a pipeline key is a reason to check its default.

### What it cost, and what it is now

| | whole frame | HUD panel |
|---|---:|---:|
| d3d9 vs d3d9 | 0.07 | 0.00 |
| before | 9.62 | 38.96 |
| after | **8.30** | **4.62** |

Ruled out along the way, each with a switch or a dump rather than an argument: vertex specular
(`GKPLUS_NO_SPECULAR` changes nothing), `D3DTA_TFACTOR` (set, but no stage names it),
`D3DRS_COLORWRITEENABLE` (never set), and a wrong texture (the shapes matched all along; only
the channels differed). The green channel matching d3d9 to within 2/255 while red and blue did
not was the measurement that made "a colour multiplies this" the only remaining explanation.

## 4.21 Why the topologies made it worse: Gunlok has stencil shadows

§4.20 shipped the line lists and triangle strips switched off because turning them on cost
2.30/255. The topologies are not the problem. **The game draws stencil shadows, and this
renderer has no stencil buffer**, so one of those draws lands over the whole screen instead of
over the shadows.

### Getting a comparison that means anything

The first attempt at this bisect was worthless, and finding out why is the more useful half.

**Two launches are not comparable.** The two renderers run at different frame rates, so at a
fixed wall-clock delay the *game* is in a different state. Measured: three Vulkan runs of
identical code at the same 12-second settle differ from each other by up to **8.06/255**, which
is larger than nearly everything worth measuring. Two of them agreed to 0.03 and a third did
not — so a single agreeing pair proves nothing either. The d3d9-vs-d3d9 floor of 0.07 says the
*capture* is reproducible; it says nothing about the game's state.

Three things together make a frame comparable, and all three are needed:

- **`screen.toggle_pause()`** (`PAUSE GAME`), so the simulation stops and nothing animates;
- **set the camera explicitly** — `camera.position`, `yaw`, `pitch`, `roll`, `distance` — so the
  framing does not depend on where the intro move happened to get to. Read the values back from
  a session and reuse them *within* it: the same literals replayed in a later session framed
  something else entirely, and `render.draws` dropping to 73 a frame is what said so. Check that
  count before trusting a controlled shot;
- **wait out the mission-objectives overlay**, which dims the entire screen while it is up and
  is itself one of these full-screen quads.

`controlled.ps1` in the session scratchpad is that procedure.

**Better still, do not compare two processes at all.** `render.topologies` is settable at run
time, so with the game paused the feature can be toggled between two shots of the *same frame*.
That reduces the noise floor to **0.03** and makes the difference image exactly the pixels the
feature touched. That is what turned this from an argument into a measurement, and it is the
technique to reach for whenever a renderer feature needs judging.

### What the three draws are

There are four non-triangle-list draws a frame, and `render.state` now prints what each one is,
including the screen box its vertices cover:

```
   type   fvf  from  prims  stages  texture  blend  ztest   screen box          colour      sten
      2 0x1c4   ptr      2       1       -1      0      1   512,384  640,480  0xff00c700   0
      5 0x1c4   ptr      1       1        7      1      1   576,418  639,440  0x8f3fff3f   0
      5 0x1c4   ptr      2       0       -1      1      0     0,0    628,468  0x7f000000   1
```

The first two are HUD corner decorations and cost nothing — toggling the line lists alone moves
the picture by 0.04/255. The third is a **full-screen quad, 50% black** (`0x7f000000` blended
`SRCALPHA`/`INVSRCALPHA`), drawn twice a frame, and it has `D3DRS_STENCILENABLE` on where
nothing else in the frame does.

### It is the classic shadow-volume algorithm

The pipeline histogram, now keyed by the stencil states too, shows all three passes:

```
 42 draws 0x112  blend ZERO/ONE  z 1 zwrite 0 cull NONE   sten 1 func ALWAYS    pass INCRSAT
 42 draws 0x112  blend ZERO/ONE  z 1 zwrite 0 cull NONE   sten 1 func ALWAYS    pass INCR
 42 draws 0x1c4  blend SRC/INV   z 0 zwrite 0 cull NONE   sten 1 func LESSEQUAL pass REPLACE  ref 1
```

Two passes over the shadow geometry with **colour writes neutralised by the blend itself** —
`SRCBLEND ZERO`, `DESTBLEND ONE` is `result = dst`, so they are invisible by construction rather
than by a colour mask — incrementing the stencil counter, and then the darkening quad tested
against it with `LESSEQUAL 1`.

So this renderer already draws the volume passes, correctly and invisibly. What it does not do
is *count* anything, because **`ChooseDepthFormat` puts `VK_FORMAT_D32_SFLOAT` first** and that
format has no stencil aspect. With no mask, the third pass darkens everything.

That also explains the shape of the earlier symptom exactly: the difference image showed the
terrain darkened and the **units left alone**, because the quad is submitted mid-frame, after
the world and before the characters.

### What the fix is

Not large, and now fully specified by the table above:

1. Pick a depth format **with a stencil aspect** — `D24_UNORM_S8_UINT` or `D32_SFLOAT_S8_UINT`,
   both already in the candidate list, just after the one that wins — and clear stencil with
   depth.
2. Put the stencil state in `PipelineState`: enable, func, ref, read/write masks and the three
   ops. `D3DSTENCILOP` and `VkStencilOp` do **not** correspond one-to-one — D3D's `INCRSAT`(5)
   saturates and `INCR`(7) wraps, which is `VK_STENCIL_OP_INCREMENT_AND_CLAMP` and
   `..._AND_WRAP` — so write them out rather than subtracting.
3. `D3DRS_STENCILREF` and the masks are dynamic state in Vulkan, so they need not multiply the
   pipeline count.

Until then the topologies stay off, which is the smaller error: shadows missing rather than the
whole screen darkened.

## 4.22 The scratch belonged to the wrong end of the frame

Actors and text glitched wildly and at random: units and HUD characters landing at arbitrary
positions, particles drawn as huge screen-filling quads, HUD numbers vanishing for a frame.
Only the user-pointer draws — §4.18's text, particles, menus and **units** — and never the
world, which is the shape that names the culprit before any measurement does. Buffered geometry
lives in the arena; only the UP path goes through the per-frame scratch.

### The defect

`DrawFrame` called `BeginFrameScratch(FrameIndex)` at the **top**, right after the fence wait,
on the reasoning that the fence proves the slice is free to reuse. The fence proof was right and
the placement was wrong, because **the scratch is not written by the renderer — it is written by
the game, during the scene, before Present is ever called**.

So for scene *k*:

- the game bump-allocates and converts into the slice whose base the *previous* `DrawFrame` had
  installed, and records `base_vertex` relative to it;
- `DrawFrame(k)` then rotates the base and `RecordDraws` reads `ScratchVertexAddress()` — the
  *new* base — for every `push.vertices`.

With two slices that is a full slice of skew: every user-pointer draw pulled its vertices from
the slice the **previous scene** had filled, at the **current scene's** offsets. It looks fine
whenever consecutive frames allocate the same way, which is exactly why it survived §4.18's
"the characters appear where d3d9 has them" check and every counter: a static scene re-allocates
identically frame after frame, so the stale slice happens to hold equivalent bytes. It falls
apart the moment the pattern moves — a particle appearing, a HUD number changing width, a unit
entering the frame — and then a draw addresses unrelated vertices.

Two independent bugs, one cause. The second is a plain data race: after the rotation, the slice
the CPU wrote next was the very one the frame just submitted was reading.

**The indices were unaffected and that asymmetry is diagnostic.** `AllocateScratchIndices`
already makes its offset absolute from the start of the buffer (the index buffer is bound at
offset 0), so an index offset carries its own slice and stayed correct. Only the vertex address
was recomputed at record time. Structure right, positions wrong — which is precisely what a
"vertices glitching" report describes.

### The fix, and why it needs a third slice

The slice belongs to the **scene**, not to a frame in flight. `RotateFrameScratch` is called at
the *bottom* of `DrawFrame`, after the submit that reads the outgoing slice, so the scene the
game is about to draw writes somewhere else and `RecordDraws` still sees what that scene wrote.

The count then has to grow, and the argument is worth keeping because "one per frame in flight"
is the reflex:

- scene *k+1*'s write window is `[end of DrawFrame(k), start of DrawFrame(k+1)]`;
- the only completion proven inside that window is the fence waited at the top of
  `DrawFrame(k)`, which retires frame *k−N*;
- with *N* slices the one coming round belongs to frame *k+1−N*, still in flight.

So `kScratchSlices = kFramesInFlight + 1`, and the incoming slice was last read by the frame that
fence retired. The buffers grow by half — 12 MB vertex, 3 MB index at full heaps, against
measured peaks of 800 KB and 66 KB, so this is untouched headroom either way.

A dropped frame (the `CreateSwapchain`-failed path) empties the current slice instead of
rotating: nothing was submitted from it, so it stays the one the next scene writes into, and
rotating there would be the unproven move all over again.

### What the A/B says

The check is a *dynamic* scene, and picking one is the whole difficulty — a paused frame cannot
show this. `fx.snow(true)` is the cheap generator: particle counts differ every frame, so the
allocation pattern never repeats.

| | pre-fix | fixed |
|---|---|---|
| snow on, level01 | screen-sized grey quads where flakes belong; both HUD health numbers gone | flakes correct, HUD complete |
| paused, camera set explicitly | — | **0.106/255** against pre-fix — the noise floor |

That second row is the one that keeps the first honest: with the simulation stopped the two
builds are the same picture, because a static scene is exactly the case the defect could not
reach. Both still sit 11.2/255 from d3d9 at that camera, unchanged by this — that residual is
§4.19's plus the §4.21 shadow wedge, and **both builds draw the wedge identically**, which is
what rules this change out as its cause. A screenshot showing a new artifact next to a fixed one
is worth nothing until the old build has been put at the same camera; that cost one extra
build cycle here and was the only way to tell the two apart.

## 4.23 One slot per buffer is wrong: the game refills within a frame

The main menu's text glitched occasionally after §4.22, and the first thing measured ruled the
scratch out entirely: **at the front end there are no user-pointer draws at all.** All 8,000-odd
draws of a menu session are buffered. Whatever was wrong lived on the arena side, and §4.22's
fix could not have touched it.

The number that named it was **6,171 locks against 8,117 draws over 395 frames** — about 15
locks a frame for 21 draws. A static menu does not re-upload its geometry fifteen times a frame
unless it is reusing the same buffer, which is what a batching text renderer does.

### The defect

`BufferSlot` is one arena region per D3D buffer, written on every `Unlock` (§4.8, and the
reasoning there is still right for what it was measured on). The draw list, meanwhile, is not
recorded until Present. So for a buffer filled twice in one frame:

```
lock/fill A -> unlock (A into the slot) -> draw1 -> lock/fill B -> unlock (B into the slot) -> draw2
                                                                                    ... Present
```

both draws read the slot, and the slot holds B. `draw1` renders `draw2`'s contents.

A counter was added rather than inferred — `buffer_rewritten_after_draw`, set when an `Unlock`
follows a draw off the same buffer in the same frame — and it reported **774 over 395 frames:
387 vertex and 387 index, one pair per frame, two draws affected.** Two of the menu's 21 draws
were rendering the wrong geometry every frame.

**The lock flags are what turn that from a suspicion into a defect**, and they are why the
counter records them. A `D3DLOCK_NOOVERWRITE` refill is the game *promising* it will not touch
bytes already drawn from, which would make all of this harmless; `D3DLOCK_DISCARD` is the
opposite. Neither is trusted on its own, so the byte ranges are compared too. The measurement:

```
lock flags 0x0800 overlapping 774
```

`0x0800` is `D3DLOCK_NOSYSLOCK` and nothing else — **no NOOVERWRITE, no DISCARD** — and all 774
overlap the range the earlier draw read. The game simply refills the same bytes of one
vertex/index pair, twice a frame, with no promise attached.

### The fix: version the refill into the frame's scratch

The slot must keep the version the already-recorded draws reference, so the *new* version is
what moves. It goes into the per-frame scratch — which is exactly the right home, because this
data has precisely the lifetime the scratch exists for: one frame, written by the CPU, read
once. It is a user-pointer draw arrived at from the other direction.

- On an `Unlock` that follows a draw off the same buffer this frame, the whole buffer is
  converted into the scratch and the wrapper records `(version_frame_, version_offset_)`; the
  slot is left alone.
- `EmitDraw` prefers a version whose frame is the current one. The version expires by itself
  when the frame number moves on — the scratch slice is recycled anyway, so there is nothing to
  clear and no way to reference a stale one.
- **The whole buffer is copied, not the locked range.** A draw may index anywhere in it, and
  copying all of it keeps the offsets the buffer's own. A *partial* refill therefore cannot be
  versioned at all — the untouched bytes are not kept anywhere on this side — so it falls
  through to the old behaviour and increments `unversioned_rewrites`, which is a must-be-0.
  Level01 and the menu produce none.

`DrawItem` grew a **separate source for each stream** for this. Vertices and indices are
refilled independently in general, so one `DrawSource` for both would force a draw with one
versioned buffer to fake the other.

### What it cost and what it fixed

```
menu:     774 rewritten, 774 versioned, 0 not versioned   scratch peak 96 KB vtx + 6 KB idx
level01:  598 rewritten, 598 versioned, 0 not versioned   scratch peak 991 KB (was 800 KB)
```

The picture is the proof, and it is legible rather than statistical: the console overlay at the
menu prints four lines, and before the fix the **third was absent and the fourth truncated to
its tail** — the two draws the counter had already identified. After it, all four render exactly
as d3d9 does. The console-text band goes from 9.95/255 against d3d9 to 6.64, and the whole frame
from 5.66 to 5.15; the rest is the pre-existing menu residual.

Two things worth carrying forward:

- **The static-frame trap from §4.22 has a mirror image here.** That defect needed a changing
  scene and hid on a still one; this one is perfectly stable — 60 consecutive menu captures were
  *bit-identical*, wrong text and all. A stability check is evidence about which defect you have,
  not about whether you have one.
- **A HUD element disappearing is not automatically a renderer bug.** The health bars and unit
  numbers vanished between two in-level shots and looked like a regression; the d3d9 reference
  shows them absent in the same states. They follow game state, and a toggle with a five-second
  wait either side spans one.

## 4.24 Two copies to one arena slot, and nothing ordering them

**Two transfers in a command buffer are not ordered against each other.** Vulkan orders the
stages of a pipeline; it does not order two `vkCmdCopyBuffer`s that write the same bytes. Without
a barrier between them the result is whichever the driver retires last, and on this AMD card that
is often the *earlier* one.

`RecordInto` emitted every pending copy back to back. That is correct for a batch of copies to
distinct destinations, which is what a frame's uploads are — and wrong for a **level load**, which
frees a buffer's arena slot and hands it straight to a new buffer inside the same batch. Both
uploads then sit in `Pending` naming the same arena offset, and the loser's contents are what the
draw reads.

The symptom is not subtle and does not look like a synchronisation bug: **one object smeared into
a hard-edged black wedge across a third of the screen**, because a mesh drawn through another
mesh's vertices reaches wherever those indices point. On level01 it was the fraggable boulder,
identical in every session, and it swept over the world as the camera scrolled — which reads as
the visibility mask glitching rather than as one object being wrong.

The same hazard applies to the image blits, which were also emitted back to back between one
barrier pair, and it was costing 1-3 mip levels a session — the residual §4.13 left open.

### What it took to find, which is the part worth keeping

Every cheap check said the upload path was correct, and each was true:

- `render.verify_buffers()` — added for this, the buffer half of §4.13's texture check — pinned it
  to **9 of 3,467 buffers**, deterministically, with `1 unlocks` each and a re-upload fixing every
  one. That is what turned "the picture is wrong" into "these nine slots hold the wrong bytes".
- The staged bytes were right when the copy was **recorded** and still right when its fence
  **retired**, so the ring was exonerated as a source.
- No two *live* slots overlapped, the buffer's own D3D contents matched what was uploaded byte for
  byte, and the uploads covered `[0, len)` of each slot.

Three of those measurements were built to catch this and could not, which is the lesson:

- **A per-block "who wrote this" table keyed on the copy's destination offset cannot see it.**
  Both copies name the same offset, so the table reported one writer and looked clean. It was
  blind to the very thing it was written for.
- **Forcing the frame path synchronous proved nothing**, because a level load never runs a frame —
  `RecordUploads` is not called, and every batch already goes through `FlushPendingNow`.
- **`GKPLUS_VK_HEAPS=small` "fixes" it** — 8 of 9 buffers come right — for the uninteresting
  reason that an 8 MB ring flushes 3,218 times instead of 30, so two copies to one slot rarely
  share a batch. A configuration that makes a bug go away is not thereby a diagnosis.

What actually named it was **logging every upload to one watched arena offset along with its batch
number** (`GKPLUS_VK_WATCH_DST`, `render.staging_watch`):

```
  batch 862: slot 6106272 + 0,  9408 bytes, staged at 15808176
  batch 862: slot 6106272 + 0, 10752 bytes, staged at 15991696
```

Two buffers, one slot, one batch. Nothing else needed to be measured.

### RenderDoc could not have shown this, and that is worth writing down

The capture route is closed for load-time uploads, in both directions at once:

- A load presents nothing, so there is no frame to capture. `CaptureStagingBatch(n)` exists for
  that — the unit is a staging batch, and it is reproducible because a load stages the same bytes
  in the same order every run, so one run says which batch and the next captures it.
- At full heaps the capture **dies**: `Allocating readback window of 67108864 bytes` then
  `common.cpp(214) - Fatal - Allocation for 67108992 bytes failed`. That is §4.17's 32-bit
  address-space limit again, hit by RenderDoc's persistent-map flush rather than by initial state.
- At the small heaps that would fit, the defect is 8/9 gone (above), so the capture would show a
  correct frame.

The instrumentation is kept anyway: it is the only way to look at a load, and the next upload bug
will want it.

### The fix, and the counter that keeps it honest

A copy whose destination overlaps one already queued in the batch gets a
`TRANSFER_WRITE -> TRANSFER_WRITE|TRANSFER_READ` barrier in front of it, and the range map is
cleared at that point — the barrier orders *every* earlier copy, so nothing before it can collide
again. Image blits use the same mechanism, conservatively keyed on image+level rather than on
rectangles. `ordered_overlapping_copies` reports it: **166,375** on a level01 session, which is
how much unordered overlap was there all along.

Both content checks come back clean afterwards, which neither had ever done:

```
  render.verify_buffers()   3467/3467 buffers match, 0 overlapping live slots
  render.verify_textures()  158/158 levels match
```

and the frames the wedge covered go from 11.23/255 against d3d9 to 7.41 (cross-session, so a
5.2/255 noise floor — the wedge is gone, and what is left is §4.19's residual).

### Two ring defects found on the way, neither of them this

Both were real and both are fixed; both are also a reminder that a plausible bug found while
hunting another one is not evidence you have found the one you are hunting.

- **`batch` counted payload bytes, not distance travelled.** Alignment padding and the tail
  abandoned at a wrap — 145 MB a session — let the head drift ahead of its own un-recorded batch
  and lap it.
- **Recorded bytes were released at record time.** A copy is only free once the GPU has *run* it,
  which is later than recording; `frame_bytes`/`in_flight` now hold them until the frame's fence.

## 4.25 The light sum is not optional, and it is most of the residual

Reported from playing, like §4.20: **crates, the boulder and some health bars are not fogged** —
they sit at full brightness against a scene that darkens with distance. Chasing it overturns
§4.19's second headline claim and reassigns the residual §4.19 left open, so this supersedes both
rather than sitting beside them.

**It is not fog, and that part of §4.19 was right.** `draws with fog on: 0` over a 12,045,221-draw
session, with `FOGENABLE`, `FOGCOLOR`, `FOGTABLEMODE` and `FOGVERTEXMODE` never set to anything
but zero. What reads as fog is fixed-function **lighting**, and this renderer implements none of
it for geometry that carries a vertex colour — which is nearly all of it.

### The measurement

`GKPLUS_NO_LIGHTING=1` on the game's own d3d9 renderer, level01, same camera set explicitly from
the REPL on a paused game (§4.21's procedure). Mean RGB per region, which is what to compare
across separate launches — a whole-frame MAD is dominated by inter-run misalignment and says
13.04 for two d3d9 runs of *identical* code:

| region | vulkan | d3d9 **lit** | d3d9 **lighting off** |
|---|---|---|---|
| boulder | 85, 59, 52 | 79, 49, 32 | **85, 59, 52** |
| ground mid | 81, 27, 17 | 75, 23, 11 | **81, 27, 17** |
| ground far | 68, 35, 28 | 59, 26, 15 | **68, 35, 28** |
| left rock wall | 24, 9, 7 | 26, 10, 7 | **24, 9, 7** |

Vulkan reproduces *lighting-disabled* d3d9 to within 0.5/255 on every static region. The renderer
draws the whole scene as though `D3DRS_LIGHTING` were off, because that is exactly what it does.

### Why it presents as "some objects are unfogged" rather than "everything is bright"

`DIFFUSEMATERIALSOURCE` is `D3DMCS_COLOR1`, so a vertex diffuse is the **material diffuse**, not
the final colour: D3D computes `emissive + ambient * global_ambient + SUM over lights(material
diffuse x light)`. The shader uses it *as* the final colour, which is D3D's lighting-off result.

On the lightmapped world — the two-stage draws on `uv1` — the lightmap supplies most of the
darkening, so it looks approximately right. The **single-stage** draws have nothing else
attenuating them and land at full unlit albedo: 428,414 at FVF `0x152` and 2,468,778 at `0x252`,
which is the crates, the boulder, the units and the health bars. That is the whole of the
reported symptom.

### The counter that should have caught this is blind by construction

`lit_draws_with_lights` is documented as the number that would say "this level needs the real
light sum". It cannot, because in `ResolveLighting` the FVF test precedes the light test:

```cpp
if ((State.fvf & D3DFVF_DIFFUSE) != 0) return;   // <- first
for (...) if (State.light_enabled[i]) { ++lit_draws_with_lights; return; }
```

so every draw with a vertex colour returns before the lights are ever looked at. It read exactly
**845 across 340,000 further draws** while the level rendered — a frozen counter, not a small
one, and the two are indistinguishable without watching it move. It only ever counts HUD
geometry.

### It is also the residual §4.19 could not name

§4.19 described the leftover as growing with distance and desaturating toward grey — near rock
within 1.5/255, walkway ~+8 on every channel, far rock +8/+13/+23 — and ruled out D3D lighting.
Measured here, lit against unlit: ground far +9/+9/+13, boulder +6/+10/+20, units +18/+16/+22.
Same shape, same magnitude. Whole-frame d3d9 lit vs d3d9 unlit is **6.54**, against the 0.08
§4.19 recorded for that same A/B.

**Why the original A/B read as null is worth keeping**, because it is a general trap:
`LightEnable` is called **118,077,962** times a session — lights are switched on and off around
individual draws — so `render.state`, which samples between frames, shows all five lights `off`
and the global ambient 0. From that snapshot the equation genuinely does collapse to the vertex
colour. **The state a per-draw quantity has between frames is not the state it has at any draw.**

Level01's lights, for whoever implements this: one directional (type 3, diffuse 1.29/1.00/0.69,
direction 0.382/0.644/-0.664) and four point (type 1, diffuse 1.00/0.39/0.09, range 10,
attenuation 0.9599/0.0199/0.0599).

### What the fix needs

The real per-vertex sum, in the vertex shader: directional, point and spot, with range and the
three attenuation coefficients, and the material tracked from `COLOR1`. The 128-byte push
constant block is full (§4.19), so the lights have to be a per-frame buffer with a per-draw
enable mask — i.e. this wants the `GpuDraw`/`GpuMaterial` design in §2 rather than another push
constant. Not implemented.

## 4.26 The light sum, and the per-draw record it needed first

§4.25 named the gap and measured it; this implements it. D3D8's per-vertex equation now runs in
the vertex shader - ambient, diffuse and specular over directional, point and spot lights, with
range, the three attenuation coefficients, the spot cone, and each of the four material colours
tracked from whichever source `D3DRS_*MATERIALSOURCE` names.

**It reproduces d3d9 to within 1/255 on four of the six regions §4.25 measured.** Same procedure -
level01, paused, camera set explicitly from the REPL (§4.21) - and mean RGB per region, which is
what survives comparing two launches:

| region | d3d9 | vulkan **lit** | vulkan **unlit** (the previous build) |
|---|---|---|---|
| boulder | 72, 48, 31 | **73, 48, 32** | 78, 58, 52 |
| ground mid | 80, 24, 11 | **80, 24, 11** | 85, 28, 17 |
| ground far | 68, 19, 9 | **68, 20, 9** | 74, 24, 15 |
| left rock wall | 35, 12, 8 | **35, 12, 8** | 35, 13, 10 |
| units | 71, 37, 25 | **76, 38, 26** | 87, 51, 45 |
| HUD | 0, 27, 0 | 6, 33, 2 | 6, 34, 2 |

Whole frame against d3d9: **4.66 lit, 6.97 unlit**. And because `render.lighting` toggles it at
run time, the sharper measurement is available - on one paused frame, two captures of identical
code are **bit-identical (0.00)** and lighting-on against lighting-off is **3.88**, so the
difference image is exactly what the light sum paints and nothing else.

### The counter said 845 and the truth was 796,297

`lit_draws_with_lights` is fixed and it is worth recording how far off it was. It read **845
across a whole session** because its FVF test preceded its light test (§4.25). Counting properly:
**819,653 lit draws, 796,297 of them with a light switched on** - so a light is enabled on 97% of
lit draws, not on a rounding error's worth. `render.state` shows all five lights `off` because it
samples between frames, and `LightEnable` is called 16.4 million times in this session alone.

### What the state turned out to be, which is not what the defaults suggest

Read off `render.state` rather than assumed, and two of these are the reason the equation cannot
be shortened:

| state | level01 | why it matters |
|---|---|---|
| `LIGHTING` | on for 1,650,311 of 1,688,802 draws | nearly everything is lit |
| `DIFFUSEMATERIALSOURCE` | `D3DMCS_COLOR1` | a vertex diffuse is the **material** diffuse - the whole of §4.25 |
| `AMBIENTMATERIALSOURCE` | never set, so `D3DMCS_MATERIAL` | the API default is load-bearing and had to be added to `InitialiseShadowState` |
| `SPECULARENABLE` | **on** | §4.20 ruled specular out by A/B, not by this state - see below |
| `SPECULARMATERIALSOURCE` | `D3DMCS_COLOR2` | on every draw, which broke the first version of the COLOR2 counter |
| `COLORVERTEX` / `LOCALVIEWER` | on | both are the API defaults, and both are read |
| `NORMALIZENORMALS` / `AMBIENT` | off / 0 | listed anyway: "the default is zero" is what stops being true later |

**The COLOR2 fallback is exact, and that is structural rather than luck.** The canonical vertex
drops the specular colour (§4.10), so a draw naming `D3DMCS_COLOR2` gets the material instead -
which is also what D3D itself does when the vertex has no such colour. The only FVF Gunlok emits
with `D3DFVF_SPECULAR` is **0x1c4, which is `D3DFVF_XYZRHW`** - pre-transformed, and therefore
never lit at all. So no lit draw can lose anything to it, and `lit_draws_wanting_colour2` reads 0.
Its first version tested the render state alone and counted all 1,351,426 lit draws while
reporting nothing; a counter that fires on every draw is as useless as one that fires on none.

### The push constant block was full, so this needed §2's GpuDraw first

A light array cannot go in 8 spare bytes, so the per-draw *data* moved out: `GpuDrawRecord` (288
bytes - both matrices, the normal transform, five material colours, the global ambient, the eye,
and a range into the frame's `GpuLight` array) lives in host-visible scratch reached by device
address, and the push constants carry its index. **The block went from 120 bytes to 72**, which
is also the room a third texture stage or a `GpuMaterial` pointer will need.

Both arrays are per-frame data written by the capture layer at draw time, which is exactly what
the user-pointer vertex scratch already is - so they are two more `Scratch` slices and rotate
with it. That inherits §4.22's correctness argument rather than restating it: the scene writes
them before the Present that reads them, so they rotate at the *bottom* of `DrawFrame`. Peaks on
level01 are **185 KB of 2304 KB** for the records and **18 KB of 448 KB** for the lights.

**The lights are deduplicated by enable mask within a frame**, keyed additionally on a generation
counter bumped by `SetLight` - because the mask alone stops identifying a run the moment a light's
contents change under it. Without the dedup, 16.4 million `LightEnable` calls a session would mean
appending up to eight records per draw.

### Two things that would have rendered black, and are guarded

- **A draw before the game's first `SetMaterial`.** D3D8 documents no default material and every
  term of the equation comes from one, so a zeroed material lights to black. Those draws keep
  their vertex colour - the previous build's behaviour, so it cannot be a regression - and
  `lit_draws_without_material` says whether it ever happens after a level is up. It reads 0.
- **A singular world matrix**, where the inverse transpose the normal needs does not exist. It
  falls back to the 3x3 itself rather than producing infinities, because a NaN normal takes the
  whole draw's colour with it and fails nowhere near where it was made.

### What is left, and it is now three named things

The amplified difference against d3d9 is no longer distance-shaped. What remains is:

- **the units' stencil shadow**, a dark blob under them that d3d9 draws and this renderer cannot
  (§4.21) - the largest single feature left in the frame;
- **edge fringes** on the walkway grating and the rock silhouettes, which are filtering and
  sub-pixel differences and dominate any whole-frame MAD;
- **the HUD, +6/+6/+2**, which is unchanged between lighting on and off - so it is neither caused
  nor fixed by this, and is a separate item that this measurement isolated for the first time.

## 4.27 The stencil buffer, and the shadows it lets through

§4.21 diagnosed this completely and left it: `ChooseDepthFormat` preferred `VK_FORMAT_D32_SFLOAT`,
which has no stencil aspect, so the game's two shadow-volume passes counted nothing and the
50%-black quad meant to be masked to the shadows covered the frame. That is the whole reason the
non-triangle-list topologies were opt-in.

Implementing the table in §4.21's "what the fix is" turned out to be exactly what it said, plus one
thing that section could not have known about.

### The masks, which the game never sets

`InitialiseShadowState` did not seed the stencil states, and D3D8's defaults for
`D3DRS_STENCILMASK` and `D3DRS_STENCILWRITEMASK` are **all ones**. The game sets the enable, the
func, the ref and the three ops for its shadow volumes and never touches either mask - so the
mirror would have handed Vulkan a compare mask and a write mask of **zero**.

That fails in the way that is hardest to attribute: a write mask of 0 means the two volume passes
increment nothing, and a compare mask of 0 means the `LESSEQUAL 1` test compares 0 against 0 and
**passes everywhere**. The stencil buffer would have been present, correct, enabled, validated -
and the quad would still have darkened the whole screen, which is the exact symptom the buffer was
added to remove. Seeding the eight stencil defaults is what makes the feature work, and it is a
state-mirror bug rather than a renderer one.

### What was implemented

- **The candidate order is the fix.** `D24_UNORM_S8_UINT` and `D32_SFLOAT_S8_UINT` come first now;
  the depth-only formats stay as a fallback, with a log line, because a renderer that will not
  start is worse than one without shadows. The chosen format reports in `render.draws` -
  `depth format: 130 (with stencil)` - so the failure mode is visible rather than inferred.
- **One image, one view, both aspects.** Dynamic rendering's depth and stencil attachments may
  name the same view, and a view restricted to one aspect could serve only one of them. The
  aspect mask, the layout (`DEPTH_STENCIL_ATTACHMENT_OPTIMAL` rather than `DEPTH_ATTACHMENT_OPTIMAL`)
  and the barrier all ask `DepthHasStencil()`.
- **The pipeline key grew five fields** - enable, func, and the fail/zfail/pass ops - and level01
  goes from 6 pipelines to 11. The reference and the two masks are **dynamic** state, set in
  `RecordDraws` on change, so a draw that differs only in its reference value does not build a
  second pipeline.
- **`D3DSTENCILOP` and `VkStencilOp` do not correspond**, and this is where §4.21's own table has a
  slip worth keeping: it calls `INCRSAT` 5, and `INCRSAT` is **4** (`DECRSAT` is 5). D3D orders the
  saturating pair before `INVERT` and the wrapping pair after it; Vulkan interleaves them
  differently. Subtracting a constant - which `ToCompareOp` legitimately does, because the compare
  functions really are the same eight in the same order - would have turned `INCRSAT` into
  `INVERT` here.
- **Both faces take the same state.** D3D8 has no two-sided stencil, and this is load-bearing
  rather than a formality: the volume passes draw with `D3DCULL_NONE`, so back faces are genuinely
  rasterised and must count the same way.
- **`StartImGui` now runs after `StartDraw`.** The overlay draws into the same pass, so its
  pipeline has to declare the same depth and stencil formats - and which those are is `StartDraw`'s
  decision. It used to be initialised first, when there was no format to declare.

### What it is worth, measured on one paused frame

The topologies toggle at run time, so this is the §4.21 technique rather than two launches: same
frame, `render.topologies` off and on, noise floor **0.000** across a repeat shot.

| region | d3d9 | vulkan, shadows | vulkan, no topologies |
|---|---|---|---|
| under the units | 70.7, 36.7, 24.9 | **70.7, 36.3, 24.4** | 76.2, 38.2, 25.5 |
| floor | 57.8, 17.4, 7.6 | 57.7, 17.5, 7.6 | 57.7, 17.2, 7.6 |
| pillar | 58.6, 35.1, 22.4 | 58.7, 35.1, 22.4 | 58.7, 35.1, 22.4 |
| HUD | 25.7, 32.3, 17.3 | 28.3, 35.7, 18.1 | 28.3, 35.7, 18.1 |

The shadow was worth **5.5/255 of red over the region it covers**, and that region now matches
d3d9 to within 0.5. Everything else moves by less than 0.3, which is what says the topologies were
never the problem.

**The whole-frame number barely moves: 4.71 without, 4.59 with.** That is the useful correction to
§4.26, which called the shadow "the largest single feature left in the difference image". It is the
largest *feature* - a thing that is drawn in one renderer and not the other - and it is nearly
invisible in a whole-frame MAD, because a few hundred pixels of shadow cannot compete with edge
fringes spread over every silhouette in the frame. Judge a feature on its own region; the
whole-frame number is dominated by filtering and always was.

The difference image amplified 4x is now edge outlines everywhere plus the HUD, with no blob.

### A validation error that belonged to the verifier

`render.verify_buffers()` was emitting `VUID-vkCmdCopyBuffer-srcBuffer-00118` on every run: the
arenas were created without `TRANSFER_SRC`, so the readback that exists to check the bytes was
itself an invalid call. It read 3469/3469 anyway, because the driver tolerates it. The texture
images already carried `TRANSFER_SRC` for exactly this reason and the arenas were simply missed.

This is the plan's own warning arriving from the other direction: it says to check
`render.validation` in the same breath as a readback because *a broken verifier reports its own
mismatches as the code's*. Here the verifier was reporting its own **errors** while its results
were fine, which is the same trap with the outcome reversed - and it went unnoticed because nobody
had run the two together on a frame where both had something to say.

### One thing that reads as a defect and is not

With `fx.snow(true)` running, `render.verify_buffers()` reports 3468/3469 with the odd one out a
buffer the game is refilling every frame. Paused, it reads 3469/3469. The verifier reads a buffer
while the game writes it; that is a race in the diagnostic, not in the upload path.

## 4.28 The edge fringes were half a pixel, and they were most of the residual

Every section since §4.19 has ended by calling the remaining difference against d3d9 "edge
fringes - filtering and sub-pixel differences" and moving on, because a fringe on every silhouette
looks like the price of a different rasteriser. It is not. **D3D8/9 put the centre of pixel
(i, j) at screen coordinate (i, j); Vulkan and D3D10+ put it at (i + 0.5, j + 0.5)**, so every
interpolated value - and therefore every texture fetch - was half a pixel out, everywhere, in
every frame this renderer has ever drawn.

Half a pixel of the whole frame, measured on level02:

| | whole frame vs d3d9 |
|---|---:|
| d3d9 against d3d9, two launches | **0.094** |
| vulkan, before | 4.017 |
| vulkan, after | **2.680** |

The fix is `VkViewport::x = y = 0.5f` on the world pass (`ViewportOrigin` in VkDraw.h, toggled at
run time with `render.half_pixel`). The overlay does not take it - ImGui's backend sets its own
viewport, and the overlay is drawn for the human rather than to match d3d9.

### How to find a sub-pixel offset, since staring at a difference image will not

Amplifying the difference shows outlines and says "filtering". What identifies it is **resampling
one shot against the other and finding where the difference minimises**: bilinearly sample the
d3d9 image at `(x + dx, y + dy)`, compare against the Vulkan one at `(x, y)`, and sweep dx and dy
over a grid. A rasterisation offset puts the minimum somewhere other than the origin, and says
exactly how far.

| region | before: MAD at (0,0) | before: best | after: MAD at (0,0) | after: best |
|---|---:|---|---:|---|
| scene | 4.876 | 3.663 at **(+0.50, +0.50)** | **3.515** | 3.481 at (0.00, -0.25) |
| HUD | 3.080 | 2.735 at (+1.00, 0.00) | **2.525** | 2.525 at **(0.00, 0.00)** |

The scene's minimum sitting at exactly (+0.50, +0.50) *before* is the whole diagnosis; that it
moves to the origin *after* is the whole verification. Neither depends on the absolute MAD, which
is what makes this measurable across two launches.

The HUD's +1.0 is the same offset seen through a small pixel-aligned sprite: shifting a quad by
half a pixel moves its edges across pixel boundaries, so the *content* appears to jump a whole
one. Sub-regions of one panel disagreed (+1.0 for the text, +0.5 for the icons) for that reason.
Sample a large region of ordinary geometry, not a sprite.

### Two launches are reproducible after all, and the old floor was measuring the wrong thing

**d3d9 against d3d9, two separate launches, is 0.094 over the whole frame and 0.00 on every HUD
region.** §4.21 measured up to 8.06 between two runs of identical Vulkan code and concluded that
cross-launch comparison was hopeless; §4.20 put the floor at 0.07 only at a twelve-second settle.
Both were measuring the *game* drifting, not the renderer. Pinning the frame removes it entirely:

- **`level02`**, which plays no cutscene (the reason it is the level to load, and this is what
  that rule buys);
- **`screen.toggle_pause()`** before the shot;
- **the camera set explicitly from the REPL**, from values read back out of the run being
  compared against.

With those three, level02 is deterministic to 0.094 across launches - so a cross-renderer
difference above ~0.1 is real, and the per-region-mean workaround is no longer the only thing
that survives. Keep it for regions anyway; a mean is still what tells a colour error from an edge
one.

### The five sampler defaults, which were all zero and none of which is zero

`InitialiseShadowState` seeded the render states and, since §4.27, the stencil ones - and never
the texture-stage sampler states. D3D8's defaults there are `D3DTADDRESS_WRAP` for both address
modes, `D3DTEXF_POINT` for MAG and MIN, and `D3DTEXF_NONE` for MIP. Zero is none of those: it
built a **LINEAR/LINEAR sampler with mipmapping enabled** for every stage the game had not
configured. This is the third instance of the same class of bug, after `D3DRS_COLORWRITEENABLE`
(§4.20) and the two stencil masks (§4.27), and the rule has earned restating: **adding a state to
the mirror is a reason to look up its default, because zero is a legal value for most of them.**

`D3DTEXF_NONE` also needed a translation that did not exist. It is not a filter this renderer gets
to approximate - it means *do not mipmap*, sample level 0 whatever the footprint - and Vulkan has
no `mipmapMode` that says so. It is a LOD clamp: `maxLod = 0.25` instead of `VK_LOD_CLAMP_NONE`.

Worth 0.04/255 on level02 and no visible change, because Gunlok configures its samplers inside
**state blocks** and the shadow state was already right nearly everywhere. Landed anyway: it is a
correctness fix whose absence is a blur, and a blur is invisible to every counter this renderer
has.

That state-block detail is also why the new `render.state` sampler block reports all seven states
as "never set" while showing live values that are anything but: `ApplyOp` writes the shadow state
directly and `TheStats.stage_states` only records direct `SetTextureStageState` calls. **The live
column is the one to read**, and the stage-configuration histogram now carries `filt` (mag/min/mip)
and `addr` (u/v) per stage per draw, which is the reading that actually attributes a filter to a
group of draws. Level02 uses `222` on the world's stage 0, `220` on its lightmap stage, and `111`
and `110` on parts of the HUD and the on-screen text.

### The HUD, and the first place the reference is the wrong one

The HUD has been "+2.6/+3.4/+0.8 against d3d9, cause unknown" since §4.26. Two things turned out
to be true of it, and the second is the one that matters.

**It is not a region that fails.** With the half-pixel offset in, the HUD panel differs by 2.478
where the *rest of the frame* differs by 2.693. It is slightly better than average. Every
statement since §4.20 calling it "the one region that does not match" was true only while every
pixel in the frame was half a pixel out, and the HUD - high-contrast art at roughly 1:1 texel to
pixel - showed that more than anything else did.

**What is left of it is two bright vertical bars, and they are d3d9's fault, not ours.** Scanlines
through the panels find the palettes identical and the text and portraits matching pixel for
pixel, and then a 3-pixel column at x=554..556 and another at x≈616..619 reading **134** in Vulkan
against **35** in d3d9, over a panel background of 26. Those columns **belong in the game**: the
Vulkan renderer draws them and the d3d9 path does not. d3d9 is not drawing nothing there - 35
against a 26 background is the panel edge underneath - it is the bright highlight over it that
goes missing.

So this is the first known case of **the A/B reference being the wrong one**, and it changes what
the reference is for. `GKPLUS_RENDERER=d3d9` is a second implementation with its own defects, not
ground truth: it answers "do these two agree", and a disagreement is a reason to find out which is
right rather than a defect report against this renderer. Everything measured before §4.28 was a
disagreement large enough that the question did not arise; at 2.68 it does.

What that is worth, so the headline number stays honest: the bars are 2,200 pixels, **0.75% of the
frame**, and excluding them moves the whole frame from 2.680 to 2.630 and the HUD panel from 2.478
to **1.497**. They are 40% of the HUD's difference and almost none of the frame's - §4.27's rule
about judging a feature on its own region, arriving from the other side.

Two things ruled out along the way, both cheap and both worth not repeating:

- **They are not a meter.** Setting `actors["gunlok"].health` to 3 leaves both columns exactly as
  they were, so they are static panel decoration rather than something whose length is state.
- **It is not a blur.** Our HUD has *more* gradient energy than d3d9's (|dx| 7.65 against 5.59) and
  a higher standard deviation, where the scene matches to 1% on both. Binning pixels by the d3d9
  value and averaging ours suggests a contrast compression and is **not diagnostic** - conditioning
  on one of two imperfectly-correlated images produces that slope whichever one is sharper. The
  gradient statistics are the test; the regression is not.

Why d3d8to9 drops them is unexamined. The draws are the green-emissive HUD material
(`0x80008000`, lighting on with no light enabled, so the sum collapses to the emissive) in a
pipeline group whose `D3DRS_ALPHABLENDENABLE` is 0 - an opaque green quad, which is exactly the
134 that appears here. Whatever the mechanism, it is a defect in the measuring instrument, and
the useful output of chasing it would be knowing where else the instrument lies.

## 4.29 Chasing the columns d3d8to9 drops: five causes ruled out, none found

§4.28 established that the two bright columns in the HUD panels belong in the game, that this
renderer draws them and the d3d9 path does not. This is the attempt to find out why. **It did not
find the cause.** What it did produce is the draw named exactly, five candidate causes eliminated
with instruments rather than argument, and the tooling that made both possible - so this section
is a record of a search, and the value in it is knowing where not to look again.

### The draw, exactly

Bisected out of the frame with `render.draw_hide` and described with `render.draw_info`:

```
draw 65 of 273
  topology 4  indexed  count 36  base_vertex 4407  first_index 298648  vertex_offset 0
  vertices from arena, indices from arena, index stride 2
  blend 0 (src 5 dst 2)  depth test 1 write 1 func 4  cull 3  colour write 0xf
  stencil 0   alpha test func 0 ref 0
  1 stage(s):  0: tex 50 sampler 3 colour 0x00000204 alpha 0x00000204
```

36 indices is 12 triangles is **6 quads**, and rendering that draw alone shows what they are: the
two vertical columns and four small icons, nothing else. Texture 50 is
`units\plates 2 1024.rim` - DXT1, 1024x1024, 3 mip levels. Stage 0 is
`MODULATE(D3DTA_TEXTURE, D3DTA_DIFFUSE)` on texture coordinate set 0. Blending is **off**, so the
result is written opaque.

Those 6 quads light **583 pixels**. Mean green over exactly those pixels:

| | |
|---|---:|
| vulkan | **125.92** |
| d3d9 | 26.44 |
| d3d9, `GKPLUS_NO_MIPMAP=1` | 26.75 |
| d3d9, `GKPLUS_NO_CULL=1` | 26.44 |
| d3d9, `GKPLUS_NO_ZTEST=1` | 26.44 |

26.4 is the panel *behind* them. **d3d9 writes nothing there at all**, and that is a stronger
statement than "it draws them wrong": an opaque `MODULATE` with a texture that failed to resolve
would write black, and nothing later covers those pixels - they survive to the final frame in
Vulkan. So the draw does not rasterise.

### What it is not

Each of these is a switch and a launch, not a reading of the source:

- **Not mip selection.** A quad three pixels wide sampling a 1024-texel texture is as minified as
  this game gets, so LOD was the first suspect. `GKPLUS_NO_MIPMAP=1` forces `D3DTSS_MIPFILTER` to
  `D3DTEXF_NONE` in the forwarded call; the columns stay absent (26.44 → 26.75).
- **Not culling.** `GKPLUS_NO_CULL=1`: 26.44, unchanged to two decimals.
- **Not the depth test.** `GKPLUS_NO_ZTEST=1`: 26.44, unchanged.
- **Not an out-of-range draw call.** D3D8's runtime tolerated a `MinIndex`/`NumVertices` reaching
  past the bound vertex buffer where D3D9's validates it and fails the call - and since
  d3d8to9 returns `D3D_OK` regardless, such a rejection would be silent on both sides. This
  renderer pulls by index and would not care, which is exactly the shape of the symptom. The new
  `draws_out_of_range` counter in `render.state` checks both buffers on every
  `DrawIndexedPrimitive` and reads **0**.
- **Not d3d8to9's draw path.** Read rather than measured, and it is a faithful pass-through:
  `SetIndices` stores the base vertex index that D3D9 moved to the draw call and
  `DrawIndexedPrimitive` passes it; `ApplyClipPlanes` re-applies only planes
  `D3DRS_CLIPPLANEENABLE` enables, and the game sets that state to exactly one value.

**Check that a switch did something before believing what it says.** These three were verified
live by their whole-frame effect against plain d3d9, on a floor of 0.094: `NO_MIPMAP` 1.711 over
46,204 pixels and `NO_ZTEST` 2.684 over 29,406 are unambiguous. `NO_CULL` is **0.243 over 2,192
pixels**, which is only just clear of the floor - honest reading of a closed-geometry scene where
back faces lose the depth test anyway, but it is the weakest of the three and worth redoing on a
scene with open geometry before treating culling as firmly excluded.

### Which D3D8 states d3d8to9 has to invent, and which Gunlok uses

`render.state` now prints the eight states D3D9 does not have or handles differently, because
"does the game set this at all" is the first question about each. Live values, and every value
ever set:

| state | Gunlok | what d3d8to9 does with it |
|---|---|---|
| `ZBIAS` | only 0 | scales to `D3DRS_DEPTHBIAS` by -0.000005 |
| `SOFTWAREVERTEXPROCESSING` | **toggles 0/1** | `SetSoftwareVertexProcessing`, but only on a mixed-VP device |
| `EDGEANTIALIAS` | only 0 | becomes `D3DRS_ANTIALIASEDLINEENABLE` |
| `ZVISIBLE` / `LINEPATTERN` | only 0 | dropped on the floor |
| `CLIPPING` | toggles 0/1 | forwarded |
| `SHADEMODE` | **1 and 2** | forwarded |
| `FILLMODE` | only 3 | forwarded |

Two of those are the next things to try, and neither has been. `SOFTWAREVERTEXPROCESSING`
toggling means the game asks for software vertex processing somewhere and d3d8to9 honours it only
on a mixed-VP device - so the question is what Gunlok created its device with. And **`SHADEMODE`
takes `D3DSHADE_FLAT`**, which this renderer ignores entirely: nothing in the shader is
`nointerpolation`. That is a Vulkan-side gap rather than an explanation for the columns, but it is
the one state in this table that is definitely unimplemented here.

### The tooling, and the two ways of using it that give wrong answers

`render.draw_range = [a, b]`, `render.draw_hide = [a, b]` and `render.draw_info(i)` were built for
this and are the first way to attribute a *pixel* to a *draw* - `render.draws` counts what was
skipped and `render.state` histograms what was configured, and neither could answer it.

- **Bisect by hiding a window, not by truncating a prefix.** A prefix truncates the depth and
  stencil buffers along with the draw list, so a draw that only becomes visible because the
  geometry in front of it was never drawn reads as the draw that painted the pixel. The first
  prefix bisect confidently returned draw 65 of 274 - and then hiding draw 65 alone changed
  nothing, because the prefix result was an artefact. Hiding a window leaves the rest of the frame
  intact, so "the pixel went away" means what it says.
- **Let the frame settle before shooting it.** At 300 ms between setting the range and capturing,
  the bisect converged neatly on a wrong answer; at 900 ms it is reproducible, and a repeat shot
  of the same range is bit-identical. A screenshot that lags one change behind produces a
  *monotone* sequence, which is what makes it convincing.

One more trap, from the same session: **the sampler-state history in `render.state` reads "never
set" for all seven while the live values are anything but.** Gunlok configures its samplers inside
state blocks, and `ApplyOp` writes the shadow state without going through the recorder. Read the
live column, or the per-draw `filt`/`addr` in the stage-configuration histogram.

## 4.30 `GpuMaterial`: the frame is now an array of indices, and 274 draws are 29 surfaces

The other half of §2's design. `GpuDrawRecord` moved the matrices and the lighting inputs into a
per-frame array (§4.26); this moves the texture stages and the alpha test into a second one, and
what is left in the push constants describes no draw at all.

**Nothing changes on screen, and that is the claim being tested rather than an aside.** It is
also the reason this section has a measurement in it: a refactor that "obviously" renders the
same is exactly the kind that quietly does not.

### What it is

```c
struct GpuMaterial {            // 48 bytes
  uint stage0_texture, stage0_sampler, stage0_color, stage0_alpha;
  uint stage1_texture, stage1_sampler, stage1_color, stage1_alpha;
  uint stage_count, flags;      // flags: the alpha test, D3DCMPFUNC | ref << 8
  uint pad0, pad1;
};
```

```c
struct Push {                   // 44 bytes of content, 48 of struct
  ConstBufferPointer<Vertex>        vertices;
  ConstBufferPointer<GpuDrawRecord> draws;
  ConstBufferPointer<GpuLight>      lights;
  ConstBufferPointer<GpuMaterial>   materials;
  uint record, material, base_vertex;
};
```

120 bytes before the draw record, 72 after it, **48 now**. Four addresses and three indices.

Four decisions, in decreasing order of how much they constrain the rest:

- **Interned in `SubmitDraw`, not in the capture layer**, which is where the `GpuDrawRecord` is
  written. A record is per draw and only the capture layer knows the matrices; a material is
  *shared*, and the table it is shared through belongs to the frame's draw list. `SubmitDraw` is
  also the one place that sees every draw exactly once — the capture layer has three entry points
  into a `DrawItem` and would have to remember to intern in all of them.
- **The table lives in the frame's scratch and the intern map dies with it.** A material index is
  only meaningful against the slice it was allocated from, so carrying one over from the previous
  scene would name an entry the shader can no longer see. `ClearDraws` clears both, and it is now
  called everywhere `Items.clear()` used to be — including the two early returns in `RecordDraws`,
  which is the kind of place a second cleanup step gets forgotten.
- **The slice holds `kMaxDrawsPerFrame` materials**, so the draw limit is the only one that can
  bite — the same reasoning that sized the record slice. It makes `dropped_materials`
  unreachable by capacity, which is what lets it mean "the scratch is unusable" instead of
  "a busy frame". Measured peak on level02: **1 KB of 384**.
- **Stages past `stage_count` are zeroed before the key is taken.** The shader never reads them,
  so two draws differing only there are the same surface — and would not be if the dead words
  went into the comparison. Zeroed rather than masked out of the comparator, because the table is
  uploaded exactly as it is compared, which is also what makes `memcmp` a sound `operator<`.

### The number that says it was worth building

Level02, in level, under validation:

```
draws: 274 this frame / 279 peak
materials: 29 this frame / 30 peak (0 dropped - must be 0)
  ... plus 2304 KB draw records (peak 78 KB) + 448 KB lights (peak 18 KB) + 384 KB materials (peak 1 KB)
```

**274 draws are 29 distinct surfaces** — a 9.5× collapse, which is the whole argument for a table
rather than a per-draw copy. It is not about bandwidth at these draw counts: it is that a second
pass over the frame is a walk over the draw array with a different pipeline, and that only works
if a draw is an *index* into shared state rather than a bundle of push constants only the
recording loop knows how to rebuild.

`render.draw_info` now prints the index, so §4.29's draw 65 reads `material 15` and is otherwise
character for character what it was.

### The material key is on the asset name now, and it predicts the table to within 2

`MaterialKey` in the capture layer is the *statistic* that predicted this table's size, and it
hashed the texture's **wrapper pointer** — an address the allocator happened to return, so the
same material hashed differently on every launch and identically to a different material that
reused a freed wrapper. It hashes the `.rim` path instead (§4.14), which is the identity a mod
has to be able to write down; a texture with no cache record behind it still falls back to the
pointer rather than collapsing every such texture into one material.

The two counts do not agree exactly and should not: `render.report` peaks at **28 a frame** and
`render.draws` at **30**, because `GpuMaterial` carries two things the key does not — the sampler
index and `D3DTSS_TEXCOORDINDEX`, both packed into the stage words. The key is strictly coarser,
so it is a lower bound, and 28 against 30 is it being a good one.

### How "changes nothing" was checked

`render.lighting` and `render.half_pixel` exist so a feature can be A/B'd inside one paused
frame; a data-layout change has no such switch, so this is two launches — which §4.21 warns
costs up to 8.06/255 of drift, and §4.28 answers with "pin the frame". Level02, paused with
`screen.toggle_pause()`, camera set explicitly to the same five values, the build before and the
build after:

| region | MAD | pixels |
|---|---:|---:|
| above the units and the text | **0.0000** | 138,160 |
| the HUD panel | **0.0000** | 35,640 |
| the big rock | **0.0000** | 25,600 |
| floor right of the units | 0.0044 | 9 of 54,064 differ |
| whole frame | 2.2020 | |

**Bit-identical**, on a comparison whose floor is 0.094 — better than the floor, because the two
frames are the *same* frame rather than two settles of it. The whole-frame 2.20 is the objectives
text at a different point in its fade and the two units at a different animation phase, which the
amplified difference image shows and nothing else in it does; the nine floor pixels are those
units' shadow edge. That is the game drifting between launches, which is the thing §4.21 is about,
and it is why the regions are the reading and the frame number is not.

Also unchanged: `render.verify_textures()` 292/292, `render.verify_buffers()` 2953/2953,
`render.validation` empty, every "must be 0" counter 0, 9 pipelines.

## 4.31 `D3DRS_SHADEMODE`: implemented, and every flat-shaded draw in the game is a shadow

§4.29 found this while looking for something else and left it as the one state in its table that
was definitely unimplemented *here* rather than a question about d3d8to9: the game sets
`D3DSHADE_FLAT` as well as `D3DSHADE_GOURAUD`, and nothing in the shader was `nointerpolation`,
so every flat-shaded draw was Gouraud-interpolated. How much of the frame that touched was
unknown.

**It is 2% of the draws, all of it the stencil shadow, and it changes no pixel in any scene
measured.** The state is honoured now anyway, because "the game sets it and we ignore it" is not
a defensible place to leave something — but the number is the point of the section, and it was
worth getting before deciding how to implement.

### Measure first: which draws, not how many

Counting flat-shaded draws would have said 2% and stopped there. `D3DRS_SHADEMODE` went into
`kPipelineStates` instead, so `render.state`'s pipeline histogram splits on it and says *which*
draws they are. Level02, whole session (`shade` 1 = FLAT, 2 = GOURAUD):

```
        fvf atest blend src dst   z zwrite cull cwrite  sten func pass zfail shade
 836  0x112     0     1   1   2   1      0    1     15     1    8    5     1     1
 836  0x112     0     1   1   2   1      0    1     15     1    8    7     1     1
 836  0x1c4     0     1   5   6   0      0    1     15     1    4    2     1     1
```

Three configurations, in equal counts, and all three are the shadow-volume system §4.27 put the
stencil buffer in for:

- the first two are the **volume passes** — `INCRSAT` and `DECRSAT` on stencil pass, and
  `SRCBLEND = ZERO, DESTBLEND = ONE`, which is `dst = dst`. They write no colour at all, so what
  the fragment shader computes for them is discarded whatever the shade mode.
- the third is `0x1c4`, pre-transformed, depth off, `LESSEQUAL`/`ZERO` on the stencil — the
  **50%-black full-screen quad**. Its four vertices carry one colour, and flat and Gouraud agree
  wherever the inputs agree.

Level01 reproduces it exactly: the same three configurations, the same equal counts (4106 each),
and no fourth. **No other draw in either level is flat-shaded**, and the front-end menu has none
at all.

So the prediction was that implementing it correctly would change nothing on screen — which is a
prediction worth *testing* rather than acting on, because §4.19, §4.20 and §4.25 are each a case
where the obvious reading of a state was wrong.

### How it is implemented, and why not as pipeline state

`D3DRS_SHADEMODE` is rasteriser state in D3D and an *interpolation qualifier* in Vulkan, so the
natural translation is a second pipeline per shading mode. That would double the pipeline count
across every blend/depth/cull/stencil combination — 11 becomes 22 on level01 — to serve 2% of the
draws.

Instead the vertex shader emits both colours twice, once interpolated and once `nointerpolation`,
and the fragment shader picks:

```hlsl
struct VertexOut {
    float4 position : SV_Position;
    float4 color : COLOR;
    float4 specular : COLOR1;
    nointerpolation float4 color_flat : COLOR2;
    nointerpolation float4 specular_flat : COLOR3;
    float4 uv : TEXCOORD0;
};
```

Two varyings against a doubled pipeline table. `shading` rides in `GpuMaterial`, in a word that
was padding — 10 useful words round up to 48 bytes either way — so it costs nothing there either.

Three things that would each have been a defect:

- **The provoking vertex already agrees, and it had to be checked.** D3D flat-shades a primitive
  with the colour of its **first** vertex; Vulkan's default convention is first-vertex too, so
  `nointerpolation` needs no `VK_EXT_provoking_vertex` and no state to go with it. Had they
  disagreed, a triangle strip would have taken its colour from the wrong end and looked like an
  off-by-one in the geometry rather than like a convention mismatch.
- **Only the two colours are flattened.** D3D flat shading does not affect texture coordinates,
  so `uv` stays interpolated. Flattening it as well would have made every flat-shaded draw sample
  one texel.
- **The mirroring happens at a wrapper, not at each `return`.** `vertex_main`'s body has two
  exits, and a third would be easy to add; it is now `shade_vertex`, and the entry point copies
  `color` into `color_flat` once. A path that forgot would have flat-shaded black.

### The measurement

`render.shade_mode` is the run-time toggle, so this is the sharp comparison the plan prefers:
pause, shoot, toggle, shoot — same frame, 0.000 floor, and the difference image is exactly the
pixels the feature moved.

| scene | flat draws | MAD | pixels differing |
|---|---:|---:|---:|
| level02, in level, paused | 1,422 | **0.000000** | 0 of 293,904 |
| level01, in level, paused, past the cutscene | 6,360 | **0.000000** | 0 of 293,904 |
| the front-end menu | 0 | 0.000000 | 0 (vacuous — nothing to toggle) |

Zero, to the bit, in both scenes that have flat-shaded draws — which is what the histogram
predicted and is now measured rather than argued. The material table notices the difference even
though the frame does not: 29 materials with `SHADEMODE` honoured against 28 with it ignored, so
exactly one surface splits on it, which is the shadow quad.

**This does not generalise past what was measured.** Two levels and the menu are not fifteen
levels, a cutscene camera, or a multiplayer game; `flat_shaded_draws` in `render.draws` is what
would show a level using it for something else, and the histogram is what would say what.

### A number in the plan was wrong, and it is not this change

Level01 reads **3468/3469** on `render.verify_buffers()` in level, stably across repeated calls,
where the plan's steady-state block claims 3469/3469. It is **pre-existing**: the same run on the
pre-§4.30 build reads the same 3468/3469. The odd buffer is a 96 KB `0x1c4` (pre-transformed)
vertex buffer with 6,207 unlocks whose bytes print identically to what was wanted — the verifier
reading a buffer the game is refilling, which the plan already documents for `fx.snow(true)` and
which turns out not to need snow. Level02 is unaffected at 2953/2953.

The rule that was almost broken here is worth stating: **a number that disagrees with the notes
is a measurement to repeat on the previous build, not a regression to assume or a note to
"fix".** Checking cost one rebuild and one launch, and the alternative was either an
investigation into a defect that is not there or a plan quietly claiming a clean reading it never
had.

## 4.32 The missing glows: a whole draw entry point, and six viewport depth slices

Reported from play: the sky looks missing, and translucent things - fire especially - look wrong.
Two real defects, both found, both fixed, and **the second one is not fully closed**. The route to
them is worth as much as the fixes, because the first one was invisible to every counter in this
renderer *by construction*.

### Getting a comparison that means anything cost more than the diagnosis

Three scenes were measured before one of them reproduced anything, and two of those three were
wasted on a methodology error worth recording.

**A fixed settle is not a controlled comparison on any level with a camera sequence.** The two
renderers run at different frame rates, so the same wall-clock delay lands at a different point in
the intro sweep. junkyard at 20 s gave a close-up under Vulkan and a wide shot under d3d9 - with
the camera globals reading *identically* a minute later, because both eventually settle to the
same place. The frames were from different moments, not from different renderers.

Worse, that scene's world state diverges too: 173 actors against 304. Its pipeline histogram
showed one configuration present in d3d9 and absent in Vulkan, which looked exactly like a
renderer defect and was a level that had got further along.

What works is **polling the camera until it stops moving**, which is renderer-independent by
construction, and then checking the actor count matches. On level03 both renderers settle to the
same five camera values *and* 301 actors, and that frame reproduced the defect immediately: two
bright glow sprites in d3d9, nothing at all in Vulkan. `shoot-settled.ps1` is the harness.

**level02's junk piles are not the symptom.** They match d3d9 to 0.1 mean RGB per region, and six
consecutive unpaused frames show no flicker. What the report describes is the *effect* layers, and
level02's opening has none in view.

### Defect one: `DrawPrimitive` was never wired up

`CaptureDevice::DrawPrimitive` counted the call, forwarded it to d3d8to9, and returned. It never
built a `DrawItem`. One of D3D8's four draw entry points did nothing at all.

**Every "must be 0" counter read zero throughout**, and that is the part to keep. They all count
*reasons a draw was rejected* - a topology we skip, a buffer with no arena slot, an FVF the
converter refuses. A draw that is never offered has no reason to be rejected, so a whole entry
point went missing without moving a single number. `render.draws` reported `0 topology, 0 no arena
slot, 0 no transform, 0 unconvertible, 0 scratch full, 0 no record` on a frame that was missing
draws.

The guard added with the fix is the one reading that compares against **what the game did** rather
than against what the renderer chose:

```
draw calls seen: 718776   submitted: 718776   unaccounted for: 0 (must be 0)
```

`seen` counts every call reaching either emitter; `submitted` counts every `DrawItem` built; the
difference has to be the named skips. Nothing had ever reconciled the two.

The fix folds both buffered paths into one `EmitDraw` with an `indexed` flag. Two things it has to
get right: a non-indexed draw adds `StartVertex` where an indexed one adds D3D's
`BaseVertexIndex`, and **the index buffer is only required to be resident for an indexed draw** -
`DrawPrimitive` reads none, and the game legitimately leaves a stale one bound, so requiring it
would drop every non-indexed draw whose last `SetIndices` named a buffer this layer never wrapped.

### Defect two: Gunlok uses six viewport depth slices and the renderer hardcoded one

`D3DVIEWPORT8::MinZ` and `MaxZ` were recorded nowhere, and `VkViewport` was built with
`0.0f, 1.0f`. Measured on level03:

```
viewport: 640x480  depth range 0.0199..0.0399   distinct ranges ever set: 6
    0.0000 .. 0.0199
    0.0199 .. 0.0399
    0.0299 .. 0.0399
    0.0399 .. 0.0599
    0.1000 .. 1.0000
    1.0000 .. 1.0000
```

**Not one of them is the default.** This is depth-range slicing: the world is confined to
`0.1..1.0` and the effect layers get thin slices in front of it, so an overlay is in front of the
world *by construction* rather than by switching the depth test off. `1.0000 .. 1.0000` is a
backdrop pinned to the far plane, which is what a sky pass looks like.

Collapsing all of that to `0..1` puts world geometry at depths D3D would never have given it, so
it occludes the layers that were supposed to sit in front. It is now per-`DrawItem` and issued
with `vkCmdSetViewport` when it changes - dynamic state, so it costs no pipeline. Level03 changes
slice 16,969 times a session, level02 5,184; a level reading 0 would be one where the engine never
layers anything.

The instrument that found it is worth naming: **`render.draw_range = [i, i]` renders one draw
against an empty frame.** Draw 351 painted the three glow blobs perfectly in isolation and
contributed nothing to the full frame, which converts "the effect is missing" into "the effect is
drawn and then lost", and the only states that can do that are depth, stencil and blend.

### What is fixed, what is not

Level03, same camera, same 301 actors, against d3d9:

| | whole-frame MAD |
|---|---:|
| before | 5.667 |
| after `DrawPrimitive` | 5.623 |
| after the depth slices | **5.540** |

A glow now appears where there was nothing. **It is still much dimmer than d3d9's, and the second
of the three blobs is still missing** - so the depth range was necessary and is not sufficient.
Whatever is left is in the same neighbourhood: the effect layers are drawn, they are no longer
wholly occluded, and something still costs them most of their contribution. The next reading to
take is `render.draw_range` on that draw against `render.draw_hide` of everything after it, which
separates "blended away" from "still partly occluded".

Level02 is unaffected except for the better: 2.803 to 2.704, with validation clean, 292/292
textures and the pre-existing 2952/2953 buffers (§4.31).

### `render.draw_vertices`

Built for this and kept. Set it to a draw index, let a frame pass, read it back for the converted
vertices and indices that draw was actually handed:

```
draw 351: indexed, 354 indices from scratch, base_vertex 23004 first_index 1093285
  indices: 0 1 2 0 2 3 4 5 6 4 6 7 ...
     0  pos    564.0000     37.2500      0.0299  w  33.33333  colour 0xffc6c6c6  uv 0.6855 0.0917
```

It answers the question no other instrument here could: `render.draws` counts what was skipped,
`render.state` histograms what was configured, `verify_buffers` proves the arena holds what D3D
held, and `draw_range` shows what a draw painted - but when a draw paints the wrong *shape*, none
of them says why. It follows the draw's own indices rather than reading the head of the slice,
because the first vertex is usually fine and the degenerate ones are further in. User-pointer
draws only: their vertices are in the host-visible scratch, where a buffered draw's are in an
arena that is deliberately never mapped.

It is also what showed that the one draw sampling `bitmaps\particles.rim` was the **HUD** rather
than a particle system - the atlas is shared - which stopped an hour of chasing the wrong draw.

## 4.33 The ground truth was one LoadLibrary away the whole time

Every comparison in this file up to §4.32 was against **d3d8to9**, and §4.28 and §4.29 had each
already caught that reference being wrong - the HUD's two bright columns belong in the game and
d3d9 drops them. The plan says in as many words that d3d9 "is not an oracle". What nobody
checked is whether the *actual* oracle was available.

It is. **Windows 10 still ships a 32-bit `d3d8.dll` in SysWOW64**, 715 KB of the original
runtime, and the game will run on it.

`GKPLUS_RENDERER=d3d8` now forwards there instead of to d3d8to9. The whole harness survives -
the capture layer still wraps the device, so the REPL, `levels.start`, the camera and every
`render.*` counter work exactly as they do in the other two modes. That is the point: a
reference you cannot drive is a reference you cannot align, and aligning the frame is most of
the work in any of these comparisons.

Two things it takes to get there, both of which bit on the first launch:

- **Load it by full system path.** GkPlus *is* `d3d8.dll`, sitting next to `gl.exe`, so
  `LoadLibraryA("d3d8.dll")` resolves to the module already loaded - this one - and
  `Direct3DCreate8` recurses into itself. `GetSystemDirectoryA` in a 32-bit process returns
  SysWOW64, which is where the 32-bit copy lives.
- **There is no D3D9 device behind it, and five call sites assumed there was.**
  `ResolveD3D9Device` ends in `static_cast<Direct3DDevice8 *>(...)->GetProxyInterface()`, which
  on a genuine `IDirect3DDevice8` reads a field d3d8to9's class has and the real one does not.
  It crashed inside `ImGui_ImplDX9_Init` on the first run - `llvm-symbolizer` on the RVA out of
  the WER log named it in one step. The first fix gated `Init` and missed `NewFrame`; the five
  sites now share one `Dx9Overlay()` predicate rather than each spelling the test out. The
  overlay is simply unavailable in this mode, which is correct for something whose only job is
  to be the reference in an A/B.

### What it says about thirty sections of measurement

Level02, settled and paused, all three renderers at 178 actors and the same camera:

| | whole frame | the junk-pile region |
|---|---:|---:|
| **d3d8 vs d3d9** | **0.017** | **0.008** |
| d3d8 vs vulkan | 2.593 | 2.954 |
| d3d9 vs vulkan | 2.594 | 2.959 |

**d3d8to9 reproduces the original to 0.017/255 on this frame.** So the d3d9 reference was sound
for everything measured through it here, and the residual is now a number against the real thing
rather than against a translation layer: **2.59**, where the plan has been quoting 2.68 against
d3d9.

That is a good outcome and it is not the same as "d3d9 is fine". It is one frame of one level.
§4.28's HUD columns are still a case where d3d9 is the one that is wrong, and this mode is how
that gets settled too - it was never previously possible.

**The rule this replaces**: "compare against d3d9, and treat a disagreement as a question about
which side is right." The rule now is **compare against d3d8**, and use d3d9 only as the second
opinion that says whether a difference is in the translation layer or in the game's own
behaviour. Three-way is cheap - it is one more launch of the same script.

### It did reproduce the report, and "mean RGB per region" is what hid it

The junk-pile decal was called a match here because its **mean RGB agrees to 0.1** across all
three renderers. That was the wrong reading, and the error is worth more than the section it is
in: the reported draw is byte-for-byte identical between the two machines (draw 176, same states,
same textures, same frame), so the report was always about a difference this measurement was
declaring absent.

The number that says so was on the same line the whole time. In that region Vulkan differs from
d3d8 by **2.95** where d3d8 and d3d9 differ by **0.008** - a factor of 370. The mean agreed
because the errors *balance*: 4,039 pixels brighter and 3,735 darker, texel-level speckle spread
over the whole decal.

**The plan's advice - "compare mean RGB per region, not whole-frame MAD" (§4.25) - is about
sub-pixel misalignment noise, and applying it to a real difference cancels it out.** Mean RGB
answers "is this region the right colour"; it cannot see a per-texel difference with zero bias.
Read both, and read them against a floor: with a real reference the floor is 0.000-0.015, so
anything above ~0.1 is signal. Against d3d9 there was no such floor to compare with, which is
part of why this survived.

### The signature: it scales with minification

Same frame, per region, against real d3d8:

| region | d3d8 vs d3d9 | d3d8 vs vulkan |
|---|---:|---:|
| junk pile (oblique, minified) | 0.008 | **2.954** |
| floor (oblique, minified) | 0.015 | 2.723 |
| big rock (near, large texels) | 0.000 | 2.292 |
| HUD panel (2D, 1:1) | 0.000 | 1.112 |
| flat cavern wall | 0.000 | 0.386 |

The error grows with how much texture detail is crammed into a pixel, and is near zero where a
surface is flat-on and unminified. That is a **texture LOD / filtering** signature, not blending,
not a missing stage, and not the material: `render.state` confirms the sampler mapping is right
(no anisotropy, matching D3D8's default of 1; no LOD bias, which the engine never sets; mip mode
mapped from `D3DTSS_MIPFILTER`), and `verify_textures` says the pixels themselves are correct.

So the junk pile is the worst region of the frame rather than a defect of its own, which is
exactly why it is the thing a player points at.

**§4.28's half-pixel origin is confirmed correct against the original**, which was never possible
before: on the pile, 2.90 with it and 5.07 without. So the remaining error is not the pixel-centre
convention - that fix is right and this is on top of it.

### One caution about the numbers above

The three-way table is one launch per renderer. A second Vulkan launch of the same scene put the
pile at 2.90 and the *floor* at 4.42 against the same d3d8 shot, where the first read 2.72 - the
units animate and their shadows move, so region MADs carry a per-launch spread of order 1. The
ranking is stable and the individual figures are not; `screen.toggle_pause()` pins a frame within
one launch but nothing pins the animation phase across launches. Compare features with a run-time
toggle inside one launch (0.000 floor) and use cross-launch numbers only for the shape.

## 4.34 The LOD probe: mip selection is not it

§4.33 left the residual looking like a texture-LOD problem, because it scales with minification.
`render.force_lod` was built to test that: it replaces the computed LOD with an explicit one in
every texture fetch, so both sides can be pinned to the same mip level. **The answer is no**, and
a clean negative is worth having - it removes the hypothesis the plan was about to spend a
session on.

### The readings

Level02, settled and paused at 178 actors, on the junk-pile region the report named. The four
Vulkan shots are one launch, so they are mutually at a 0.000 floor; the d3d8 pair is two more.

```
does forcing mip 0 on BOTH sides make them agree?
  baseline: vulkan normal vs d3d8 normal      2.918
  probe:    vulkan LOD0   vs d3d8 NO_MIPMAP   3.341     <- WORSE, not converged

which forced level best matches d3d8's own choice?
  force_lod = -1 (automatic)   2.918           <- the automatic one wins
  force_lod = 0                3.456
  force_lod = 1                3.192
  force_lod = 2                3.046

what is mipmapping even worth here?
  d3d8 normal vs d3d8 NO_MIPMAP               0.809
```

Three things fall out, in order of how much they close off:

- **Pinning both sides to level 0 does not converge them.** If the difference were which mip is
  chosen, removing the choice would remove the difference. It goes *up*, to 3.34.
- **No forced level beats the automatic one.** Our LOD selection is already the closest match
  available, so it is not biased toward blur or sharpness.
- **Mipmapping is worth 0.809 on that region in the original.** It is arithmetically incapable of
  explaining a 2.9 difference, which the first two readings show independently.

`GKPLUS_NO_MIPMAP=1` is the reference half of this and it only touches the *forwarded* call, which
is exactly what makes it usable under `GKPLUS_RENDERER=d3d8`: the real runtime samples level 0
while our shadow state keeps the true value.

### What it points at instead

Same mip, same texels (`verify_textures` is clean), different result - so the sampling *position*
or the filter weights. A cheap discriminator separates those from a colour-side error, because
stage 0 here is `MODULATE(TEXTURE, DIFFUSE)`: an error in DIFFUSE is multiplied by the texture and
therefore scales with texture **brightness**, while an error in where the texture is sampled
scales with the local texture **gradient**.

```
correlation of |difference| with d3d8 brightness        0.254
correlation of |difference| with d3d8 local gradient    0.389
```

Both are modest, and the gradient wins - which reads as a **sub-texel sampling offset**, on top of
a half-pixel viewport origin that §4.33 confirmed is right against the original (2.90 with it,
5.07 without). That is suggestive rather than settled: 0.39 against 0.25 is a lean, not a proof,
and the honest next step is a probe that does not depend on scene statistics at all - one textured
quad at a known scale, with known UVs, rendered by both and compared texel by texel, which removes
lighting, the lightmap stage and the alpha test from the picture in one move.

Also worth keeping: **the minification correlation from §4.33 does not by itself mean LOD.** The
regions that minify most in this scene are also the ones with the most texture contrast, and both
a sampling error and a colour error grow with contrast. That is why the ranking looked like an
answer and was not one.

## 4.35 The quad probe: not the mips, not the alignment, and the probe itself is degenerate

§4.34 ruled out mip selection and left a lean toward a sub-texel sampling offset. This is the
probe that does not depend on scene statistics - and it kills that lean too.

### What it is

`render.probe("<texture substring>", scale, mipmap)` draws **one textured quad**, pre-transformed
to exact screen pixels, at the end of the frame, **through the capture device's own methods**.
That last part is the whole design: the states and the draw go down both paths at once, so
`d3d8`, `d3d9` and `vulkan` are each handed the same geometry, the same texture and the same stage
setup, with nothing of the scene left in the way.

Everything that could explain a difference in a *scene* is removed rather than controlled for:

- no lighting, no fog, no blending, no alpha test, no depth, no stencil;
- **one stage**, the second explicitly `D3DTOP_DISABLE`, so the lightmap is out of the picture;
- `SELECTARG1(TEXTURE)` for colour and alpha, so the vertex colour cannot contribute;
- `units\alpha junk.rim`, which is the reported decal's own texture and has **exactly one mip
  level** - so mip selection is not merely ruled out, it does not exist.

### The readings

Level02, settled and paused, the quad at three scales:

| scale | quad | minification | d3d8 vs vulkan |
|---|---:|---:|---:|
| 0.25 | 256 px | 4x | 13.13 |
| 0.5 | 512 px | 2x | 7.83 |
| 1.0 | 1024 px | **none, 1:1** | 3.92 |

**A 1:1 quad of a single-mip texture, with every other input removed, still differs.** That is
the finding: whatever this is, it is inside texture sampling and nothing else.

Two things about how the difference is distributed, both from the 1:1 case:

- **30.7% of pixels are bit-exact**, and they are the flat regions - black, and solid colour
  blocks. Every difference sits on a texture *edge*.
- The histogram falls away fast: 74.7% under 8, 17.3% in 8-15, 5% in 16-23.

### It is not an alignment offset, which corrects §4.34

§4.28's technique - resample one shot against the other and find where the difference minimises -
applied to the 1:1 quad, over a grid of sub-pixel shifts:

```
  dy=-0.25   7.217  5.828  4.785  4.349  4.905  6.036  7.468
  dy=+0.00   6.831  5.384  4.290  3.722  4.421  5.613  7.110
  dy=+0.25   7.294  5.924  4.898  4.470  5.011  6.139  7.570
             dx = -0.75 .. +0.75 in 0.25 steps
```

**The minimum is at (0, 0)** and the surface is convex around it. The two images are already
aligned; no sub-pixel shift improves them. So §4.34's lean - gradient correlation over brightness
correlation, read as a sampling offset - was the wrong reading. A gradient-correlated difference
means "the disagreement is at edges", which an offset produces and so does anything else that
resolves an edge differently.

The 4-bit-to-8-bit channel expansion was the other candidate the histogram suggested, since
A4R4G4B4 is CPU-expanded here (§4.13) and an expansion error would cap near 15. It is not that
either: the code does `r | (r << 4)`, which is the correct replication, and the observed maximum
is 122.

### The probe is degenerate at 1:1, and that is the next thing to fix

Placed at integer screen coordinates with UV exactly 0..1, a 1:1 quad puts **every sample on a
texel boundary** - the corner where four texels meet and bilinear weights them equally. That is
the worst case for the comparison: it is precisely where a hair of floating-point difference
decides which texels dominate, and it is not where real geometry samples.

So the 3.72 is an upper bound taken at the most adversarial point, and the sweep above is
measuring alignment of an image built from those coin-flips. The probe should offer a half-pixel
quad offset so samples land at texel *centres*, where bilinear returns the texel exactly and any
remaining difference is unambiguous. That is one parameter and it is the next step, before any
more theorising about filters.

**What the probe has already earned**, whatever that shows: the residual is inside texture
sampling, it is not the mip level, it is not the alignment, it is not the channel expansion, and
it is not lighting, the lightmap, the alpha test or the vertex colour - because none of those is
switched on in the frame that still differs.

## 4.36 It looks like the A4R4G4B4 CPU expansion — SUPERSEDED by §4.37

**Read §4.37 first.** The correlation below is real and reproduces; the conclusion drawn from it
is wrong. A4R4G4B4 is what made the defect *visible*, not what caused it — a 4-bit channel has 16
levels, so a 2% resample obviously falls off the ladder where an 8-bit one merely blurs. The
section is kept because the two negatives in it (the texel-centre offset, and alpha) still stand,
and because "a variable that correlates perfectly across two samples is not thereby the cause" is
the lesson it paid for.

The quad probe gained two parameters and answered the question. Both additions came from
hypotheses that turned out to be wrong, which is the fastest either of them could have been
settled.

### Two clean negatives first

**The texel-centre offset changes nothing.** §4.35 argued that a 1:1 quad at integer coordinates
samples texel *corners*, the worst case for bilinear, and that the 3.92 was therefore inflated.
Sweeping the quad's position across a texel:

| offset | 0 | 0.25 | 0.5 | 0.75 |
|---|---:|---:|---:|---:|
| d3d8 vs vulkan | 3.72 | 3.50 | 4.00 | 3.56 |

Flat. Where in the texel the sample lands is not what differs, so §4.35's caveat was real and not
the cause.

**It is not a per-vertex alpha, and not alpha at all.** Rendering the texture's alpha channel as
greyscale - `D3DTA_ALPHAREPLICATE` on the colour argument, which is the only way to see alpha in a
screenshot - gives **100.0% bit-exact, MAD 0.0000**. The two renderers agree on that texture's
alpha perfectly.

The negative is worth more than it looks, because of *how* it fails to discriminate: that
texture's alpha is uniformly 255, so any filtering returns 255. What it does prove is that the
whole path delivers a constant exactly. It also removes blending, the alpha test and vertex alpha
in one reading.

### The measurement that lands it

Same quad, same states, same offset, same 1:1 scale, same single-level sampling - **only the
texture's format differs**:

| texture | D3DFORMAT | how it reaches the GPU | d3d8 vs vulkan | bit-exact |
|---|---|---|---:|---:|
| `unitslpha junk.rim` | 26 = A4R4G4B4 | **expanded to R8G8B8A8 on the CPU** | **4.00** | 31.6% |
| `Units\Custom Screen bg 1k 01.RIM` | `DXT1` | uploaded as blocks, decoded by the GPU | **0.60** | 64.0% |

**6.7x, and the only variable is which upload path the texture took.** The one format this
renderer does not hand to the hardware as-is is the one that differs.

### Why the junk pile

Level02 holds 53 images: **41 DXT1, 6 DXT3, 5 A4R4G4B4** and one other. So the CPU-expanded path
is under a tenth of the textures - and `unitslpha junk.rim`, stage 0 of draw 176, the decal in
the report, is one of the five.

That is the whole story of why a player points at that object: it is not that decals are wrong, or
that translucency is wrong, or that oblique surfaces are wrong. It is that this particular surface
is drawn from one of the few textures that goes through an expansion the hardware would otherwise
have done itself. §4.33's "scales with minification" and §4.34's gradient correlation were both
reading the *texture's* character rather than the renderer's behaviour.

### What to do about it

§4.13 chose the CPU expansion deliberately: `VK_FORMAT_A4R4G4B4_UNORM_PACK16` is optional even in
Vulkan 1.3, gated on the `formatA4R4G4B4` feature, so mapping it natively means a per-device
support matrix. That was the right call when the only cost was 112 MB of image memory. There is
now a correctness cost as well, and the trade changes.

Two routes, and the first is cheaper to try:

- **Find the expansion D3D8 actually performs and match it.** Ours is `v | (v << 4)`, the
  spec-correct replication. The observed bias is not the shape a naive `v << 4` on either side
  would give - the renderer comes out *darker* in the midtones (-6.4 at luma 128-159) and slightly
  brighter at the bottom, with the ends converging - so the difference is more likely to be in
  where the filtering happens than in the endpoints. A readback of one expanded image against what
  the reference samples would settle it, and `render.probe` on a flat two-texel gradient is the
  minimal case.
- **Map it natively behind a feature check**, falling back to the expansion where the device says
  no. That is the per-device matrix §4.13 avoided, and it also recovers the memory.

Not yet measured: whether the other four A4R4G4B4 textures are as visible, and what the remaining
**0.60** on the DXT1 quad is. That floor is small but it is not the 0.008 that d3d8-against-d3d9
manages, so something else is still there underneath.

## 4.37 It is not the expansion: the game renders 640x480 into a 628x468 window

Going after the A4R4G4B4 expansion found something the expansion is innocent of, and the
measurement that did it is one line of arithmetic on the probe's output rather than anything
about formats.

### The reading that reframes it

Count the *distinct values* in the 1:1 quad rather than the difference between the two images:

| | distinct R values | multiples of 17 |
|---|---:|---:|
| d3d8 | **16** | **100.0%** |
| vulkan | 256 | 38.6% |

Sixteen values, every one a multiple of 17, is what a 4-bit channel replicated to 8 bits looks
like **with no filtering applied at all** - each pixel is one exact texel. That is what the
original produces. This renderer produces a continuum, so it is blending between texels
everywhere.

So the expansion arithmetic was never the problem: `v | (v << 4)` is right, and the reference
lands on exactly the values it produces. What differs is that our samples are not on texel
centres - and §4.35's offset sweep had already shown no *translation* fixes that, which leaves a
**scale**.

### The cause

```
viewport: 640x480          <- what the game set, and what BuildMvp turns pixels into clip with
swapchain: 628x468         <- the window's client area, and what the Vulkan viewport covered
```

Gunlok renders into a **640x480** backbuffer. The window's client area is **628x468**. A
pre-transformed draw's pixels-to-clip matrix is built from the D3D viewport, so a vertex at x=640
maps to NDC +1 - and the Vulkan viewport then mapped NDC +1 onto pixel 628. **Every 2D draw was
scaled by 628/640 during rasterisation**, which resamples the texture and is why no sample landed
on a texel.

`unitslpha junk.rim` being A4R4G4B4 is what made it *visible*: a 4-bit channel has 16 levels,
so blending two neighbours produces values that are obviously not on the ladder. A DXT1 texture
resampled by 2% differs too - that is the 0.60 floor §4.36 measured and could not explain - it
just does not announce itself, because 8-bit channels have no ladder to fall off.

**§4.36's conclusion was wrong**, and the way it was wrong is worth keeping: A4R4G4B4 correlated
perfectly with the defect across two textures, and the correlation was real. It was not the cause
but the *contrast agent*.

### Why the obvious fix is not the fix

Sizing the Vulkan viewport from the D3D viewport instead of the swapchain makes the probe exact -
**16 distinct values, 100% multiples of 17**, matching the original texel for texel. It also makes
the frame much worse:

| | before | viewport = 640x480 |
|---|---:|---:|
| whole frame | 2.593 | **13.066** |
| junk pile | 2.954 | 13.825 |
| HUD panel | 1.112 | 7.901 |

Because D3D8's windowed `Present` **stretches** the 640x480 backbuffer into the 628x468 client,
and a 640x480 Vulkan viewport on a 628x468 swapchain **clips** instead. Right sampling, wrong
framing. The experiment is reverted.

The faithful shape is what the original does: **rasterise at the D3D backbuffer size into an
offscreen target, then scale that to the swapchain at present.** Then geometry lands on texels
exactly as it does in the original, and the downscale happens once, at the end, on finished
pixels - which is also where the original's 16 distinct values survive a 640→628 blit.

That is a real change to `VkRenderer` - a colour target that is not a swapchain image, the depth
buffer sized to match, the ImGui pass attached to the right one, and a final blit - so it is the
next piece of work rather than a patch. It is worth it: it is the difference between every 2D
pixel in the game being resampled and none of them being.

## 4.38 The offscreen target: 2.593 → 0.13, and 93% of the frame is bit-identical

§4.37's fix, implemented. The world is rasterised into a colour target at the **game's** backbuffer
size and blitted onto the swapchain at the end, which is the order the original does it in.

### The numbers

Level02, settled, paused, three launches. **The cross-launch floor is measured in the same
session rather than assumed** - two d3d8 runs of the same procedure - because everything below is
close enough to it to need one:

| | whole frame | pixels differing |
|---|---:|---:|
| d3d8 vs d3d8, two launches (**the floor**) | **0.034** | 1.45% |
| d3d8 vs d3d9, the second opinion | 0.051 | — |
| d3d8 vs vulkan, before | 2.593 | 67.5% |
| d3d8 vs vulkan, after | **0.107 - 0.192** | 6.1 - 6.8% |
| d3d9 vs vulkan, after | 0.122 | — |

The three-way still holds and now says something it could not before: d3d8-vs-d3d9 at 0.051 is
the same order as the d3d8-vs-d3d8 floor, so the two references agree to within launch noise, and
vulkan sits the same distance from both.

And on one paused frame, toggling `render.offscreen`, which is the comparison with no launch
noise in it at all:

| | |
|---|---:|
| the feature, on against off | **2.547** over 65.0% of the frame |
| off, against d3d8 | **2.593** - §4.37's headline number, reproduced on a fresh launch |
| on → off → on | **0.000**, bit-identical, so the run-time rebuild is exact |

**The probe quad is now bit-exact.** Same `render.probe("junk", 1.0)` §4.36 measured at 4.00 MAD
and 31.6% bit-exact on the A4R4G4B4 decal:

| | d3d8 | vulkan |
|---|---:|---:|
| distinct R values in the crop | 61 | 61 |
| multiples of 17 | 46.2% | 46.2% |
| **MAD** | **0.0000, 100.0% bit-exact** ||

So the CPU expansion was never wrong, exactly as §4.37 concluded, and the two "route not taken"
items §4.36 proposed - matching D3D's own expansion arithmetic, or mapping A4R4G4B4 natively
behind a feature check - are **not correctness work any more**. Native mapping is still worth
having for the memory (§4.13), and nothing else.

**The HUD panel is 0.000 against the original**, from 1.260 - the open item §4.28 left. The
objectives text reads 0.198 against a same-region cross-launch floor of 0.167, which is not a
difference.

### What is left, and most of it is not ours

93% of the frame is bit-identical. Of the 6.8% that is not, **93% of the pixels that differ
between d3d8 and vulkan also differ between two d3d8 launches**, in the same bounding box: the
two character models, which idle-animate, so the two shots are of slightly different world state.
At 8x amplification the whole difference image is black apart from a thin outline on each
character, a faint gradient on the rock in the upper right, and one small bright spot.

That mask overlap is the reading to take, not the MAD: it says the loud part of the residual is
the *game*, and no amount of renderer work moves it.

### How it is built

- **The render extent comes from `D3DPRESENT_PARAMETERS8::BackBufferWidth/Height`**, recorded at
  `CreateDevice` and at `Reset` (`d3d8::BackBufferExtent`). Not from the D3D *viewport*, although
  that is what `BuildMvp` maps against and although the two are equal here: the swapchain shows
  the whole backbuffer, so the backbuffer is what the blit's source rectangle has to be. Zero is a
  legitimate value - windowed D3D8 reads it as "match the client area" - and then there is nothing
  to correct for and the swapchain extent is used directly.
- **`render.state` now reports the backbuffer and every distinct viewport rectangle.** One
  rectangle covering the whole backbuffer is what makes "one viewport for the world pass" correct;
  level02 reads exactly that, `0,0 640x480`. A sub-viewport would have to move onto the `DrawItem`
  beside `min_depth`/`max_depth`, and this is what would say so - it prints a marker rather than
  leaving it to be noticed in a screenshot, because a wrongly-scaled sub-viewport looks exactly
  like the defect §4.37 just fixed.
- **The blit filter is NEAREST, and that is a deduction rather than a default.** The original's
  own 640→628 stretch preserves a 4-bit texture's sixteen distinct values (§4.37) - a filtered
  downscale could not, because a blend of two 4-bit levels is not on that ladder - so D3D drops
  columns rather than mixing them. `render.present_linear` is the A/B for it, kept because the
  deduction rests on one measurement.
- **ImGui moved into its own pass on the swapchain image**, loading rather than clearing, after
  the blit. It is drawn for a human and not to match d3d9, so it is the one thing that should not
  go through the scale - and it stays 1:1 with the window. Its pipeline therefore declares colour
  only; it used to have to name the world's depth and stencil formats because it shared that pass.
- **`TRANSFER_DST` on the swapchain is asked for, not assumed.** A swapchain created with a usage
  bit the surface does not support fails outright, and there is a working path without it, so
  `CreateSwapchain` checks `supportedUsageFlags` and the whole thing degrades to drawing straight
  into the swapchain - pre-§4.37 behaviour - with a line in the log.
- **The reconcile runs every frame and waits for idle only when something moved.** A `Reset` can
  change the backbuffer size without the swapchain going out of date, and the toggle can move
  under a paused frame; the on→off→on being bit-identical is what says the rebuild is clean.

### One thing that would have caught it earlier

`render.state` printed the viewport as `640x480` for thirty sections, and `render.vulkan_report`
printed the swapchain as `628x468`, and nothing printed them **next to each other**. The report
does now, on one line, with what closes the gap: `rendering at: 640x480 offscreen, scaled to the
swapchain at present (nearest)`. Two numbers that must agree, in two different reports, agree by
nobody's decision.

## 4.39 Two defects from a play report: the clear colour, and a quad that was not the defect

Reported as "translucent things render differently with the DLL present, in every renderer mode".
Two real defects came out of it - the clear colour and a state-block bug, both fixed - plus a
third thing that looked like a defect for most of a session and **was not one**. §4.40 is how
that was settled; the short version is that the quad is a backdrop the fire effect is supposed to
paint over, so it is §4.32's effect-layer gap seen from underneath. **Read this section for the
two fixes and for the eliminations; do not read the quad as an open defect.**

### The clear colour, and why it is a translucency defect

`CaptureDevice::Clear` intercepted the call and **recorded none of its arguments**, and
`VkRenderer` cleared its colour attachment to a hardcoded `0.10, 0.16, 0.28` - a debug blue-grey
left over from the phase where the Vulkan path only cleared the screen. The game clears to
**`0xff000000`, z 1.0, stencil 0**, measured: 3669 calls on a level02 session, **0 of them with a
rectangle list**, so a load op expresses all of it.

It is easy to file this as "the background is the wrong colour", and that is the small half. The
large half is that an alpha-blended or additive draw over an uncovered background **blends against
it** - so a translucent beam against a black sky comes out lighter and hazier over a blue-grey
one, which is exactly what a player reads as "this renderer gets translucency wrong". The game's
values are used for the depth and stencil clears too, for the same reason: neither is a free
choice, and the stencil one is what the shadow-volume algorithm counts up from.

Visible only where the world does not cover the frame, which is why thirty sections of settled,
world-filling level02 frames never showed it. `render.state` prints the recorded values now.

### The plate quad: reproduced and isolated — and it WAS the defect (see §4.42)

Level02, the two fires on the ledge north of the start. Camera pinned at
`roll 45, distance 45, pitch -30, position (-9.9, -4, 2.48)`.

**Vulkan draws an opaque lattice rectangle over the right-hand flame. d3d8 and d3d9 do not.**
Hiding that one draw restores the flame exactly, so it is a spurious draw and not the fire's own
quad with the wrong texture:

```
draw 222 of 348   topology 4  indexed  count 36  base_vertex 4407  first_index 298648
  vertices from arena, indices from arena
  blend 0 (src 5 dst 2)   depth test 1 write 1 func 4   cull 3   colour write 0xf
  alpha test func 0 ref 0   depth slice 0.1000..1.0000
  stage 0: tex 50 = units\plates 2 1024.rim
```

Eliminated, each by one measurement against the **real D3D8**:

| hypothesis | test | result |
|---|---|---|
| the original culls it | `GKPLUS_NO_CULL=1` | still absent |
| the original's alpha test discards it | `GKPLUS_NO_ATEST=1` (new) | still absent |
| we put it in the wrong viewport depth slice | `render.draw_info` | `0.1..1.0`, the world slice, same as its neighbour |
| its vertices come from a seeded buffer | `GKPLUS_VK_SKIP=s` | still drawn |

**`units\plates 2 1024.rim` is the same texture as §4.29's two bright HUD columns** - the ones
that section concluded "belong in the game: this renderer draws them and d3d8to9 does not". Same
asset, same "we rasterise pixels the reference does not", and §4.40 resolves both the same way:
neither is spurious geometry. They are *uncovered background* - surfaces the original paints over
and this renderer does not.

**Its geometry is sane.** `render.draw_range = [222, 222]` paints one clean, correctly UV-mapped
plate quad, slightly tilted - not garbage, not a degenerate fan. So the vertices, the indices and
the texture coordinates are all fine, and what is wrong is either where the transform puts it or
what state it is drawn with.

### A real state-block bug, found on the way, that is not the whole cause

`Record()` did `ApplyOp(op)` **and** appended to the block being recorded, with a comment
asserting that "the state a block sets is genuinely set on the device at record time too". That
is the opposite of what D3D does: between `BeginStateBlock` and `EndStateBlock` a state-setting
call is captured into the block and **does not change the device**. So from the moment the game
recorded a block, the shadow held the block's values and the device held the old ones, until
something set that state explicitly again - and every draw issued in that window was described
with the wrong state.

Fixed (record *or* apply, never both). It is worth 2.481 → 2.257 whole-frame at the fire camera
and it visibly changes draw 222 - the quad blends now where it was flatly opaque - but **it does
not remove it**. Two lessons: a comment asserting an API behaves opposite to its documentation is
worth checking rather than trusting, and a fix that moves the number is not thereby the fix for
the thing you were chasing.

What had not been read at this point was **where those 36 indices actually put the quad**. §4.40
reads it, and the answer moves the whole thing somewhere else.

## 4.40 Making the draw verifiable — and drawing the wrong conclusion from it (see §4.42)

> The instruments in this section are sound and are still the right ones to reach for. Its
> **conclusion** is not: "draws 223 and 224 sit in front of 222 and in the original they cover it"
> was never measured — those two are the HUD portraits in the top-right corner. And its central
> reading, "0 of 12 vertices differ", compares two values both read back a frame or more *after*
> the draw, which cannot see the defect §4.42 found. Read it for the method, not the verdict.

The mirror is the whole basis of this renderer - every draw is described by what the shadow says
was set - and until now **nothing checked it against D3D**. §4.39's state-block bug is what made
that intolerable: the two diverged silently for a whole scene and no counter could see it, because
every counter is computed *from* the mirror.

`render.verify_state()` reads the fixed-function state back off the device and diffs it against
the shadow; `render.draw_state = <index>` does the same **at the moment one draw is issued**, which
is the form that can see a divergence existing only mid-scene. Same instrument as
`verify_textures` and `verify_buffers`, pointed at state instead of at bytes, and the same
set-and-read-back shape as `render.draw_vertices`.

**It is self-testing, and that was checked before any result was believed.** `GKPLUS_NO_CULL=1`
makes the forwarded state differ from the mirror on purpose, so it is a divergence with a known
answer:

```
188/189 states match the device
NOTE: a GKPLUS_NO_* switch is set, so the device is MEANT to differ from the mirror
  CULLMODE                   mirror 0x00000003  device 0x00000001
```

Exactly the injected difference and nothing else. A verifier that cannot fail proves nothing;
this one fails where it should.

Compared: every render state and texture-stage state the game has ever set, the bound texture at
each of the eight stages (unwrapped - the mirror holds our wrapper and the device holds the inner
object, and a raw pointer compare would report every textured stage as a mismatch), the FVF, the
world/view/projection transforms, the viewport, the material, the lights, **and the stream
bindings** - stream 0 with its stride, the index buffer and `BaseVertexIndex`. Those last three
are state exactly as much as a render state is, and they are the only part of a draw the mirror
can get wrong *without* getting a single `D3DRS_` wrong.

Two limits, both honest rather than incidental: it compares against `inner_`, which is d3d8to9
under `vulkan` and Windows' own D3D8 under `d3d8` - "does the mirror match the device we forward
to", which is the mirror's contract - and the per-draw form indexes the Vulkan draw list, so it
needs `vulkan` mode. The immediate form works anywhere.

### The result, which is a clean negative

```
draw 222, device state at the moment it was issued:
193/193 states match the device
```

Also 193/193 at draws 221 and 223, and 191/191 at frame end. **The mirror is exact.** So the plate
quad is drawn with precisely the state D3D has - and D3D, given that state, rasterises nothing
there. That eliminates the entire "our state is wrong" family in one reading, including the blend
factors, the texture binding and the transform, none of which the §4.39 switches could have
isolated individually.

That left the geometry, so the second instrument got built.

### `render.draw_geometry`: what a buffered draw actually pulled

`verify_buffers` proves a slot holds what its buffer holds; `draw_info` prints the offsets a draw
was given. Neither says the draw addressed the right place - and **the arena is one buffer every
slot shares**, so addressing it wrongly yields *other geometry* rather than garbage, which looks
like a draw in the wrong position and nothing like a bug. That is §4.16's lesson and the gap it
left open.

`vulkan::ReadArena` copies from an arena at an absolute offset, which is how a draw addresses it -
deliberately not expressed in terms of a slot, because the question is not about one; `VerifySlot`
is refactored onto the same primitive. `render.draw_geometry` reports, for the draw
`render.draw_state` is watching, the indices and vertices the shader reads out of the arena beside
the ones D3D holds in the game's own buffer. The buffers are snapshotted when the draw is issued
rather than looked up later - `stream0_` is whatever is bound *now* - and the readback is
deferred, because it submits and waits and doing that mid-scene would stall the frame being
measured.

### The answer, and it is not a defect in that draw at all

```
draw 222: indexed, 36 indices, base_vertex 4407 first_index 298648, D3D bias 0
  stage 0: game bound "units\plates 2 1024.rim" -> samples image 50 "units\plates 2 1024.rim"
  idx  arena (what the shader reads)        D3D buffer (what the game holds)
  0      185.728  273.300  0.987  0.01912 84ff6519    185.728  273.300  0.987  0.01912 84ff6519
  4      179.717  262.687  0.987  0.01918 7bff6519    179.717  262.687  0.987  0.01918 7bff6519
  0 of the 12 vertices shown differ
```

Three things at once. The arena matches D3D **vertex for vertex including colour**, so the upload
path and the offsets are exonerated. The bound texture and the sampled image are the same asset,
so the second mapping - object to bindless index, which `verify_state` does *not* cover and which
nothing had ever checked - is right too. And the vertices are **screen-space with `rhw`**: six
concentric quads, orange, alpha falling 0x84 → 0x7b, at 172..262 x 257..350.

So nothing about draw 222 is wrong. What is wrong is what should be **on top of it**: draws 223
and 224 sit in the `0.0299..0.0399` effect slice, in front of 222's `0.1..1.0` world slice, and in
the original they cover it. This renderer draws them dimmer and smaller, so the backdrop shows
through.

**That is §4.32's open item seen from underneath**, and it collapses two entries of the plan into
one: the plate quad and §4.29's two bright HUD columns are both *uncovered background*, not
spurious geometry. §4.29 spent a section eliminating mip selection, culling, the depth test and an
out-of-range draw call on the columns and found nothing, because it was looking at the wrong draw
- the defect is in whatever was supposed to paint over them.

### Two process notes

- **The reported symptom named the wrong layer, and so did the diagnosis.** "In every renderer
  mode" was wrong - measured, the fire is correct in `d3d8` and `d3d9` and wrong only in `vulkan`
  - but the clear-colour half of the report was real, so reproduce before believing the scope in a
  report *and* before disbelieving it. Then the diagnosis named the wrong layer twice more: "a
  spurious quad we draw and the original does not" survived four eliminations and was false both
  ways round. The original does draw it, and it is not spurious.
- **"The original draws nothing there" was an inference from a black background**, and a black
  quad on black is indistinguishable from no quad. That inference is what sent four measurements
  at the wrong question. When the evidence for a draw's absence is that a *region* is empty, say
  so out loud - it is a much weaker claim than it reads as.
- **A clean negative from a good instrument is worth more than it feels like.** Three readings in
  a row said "this is not it" - 193/193 on state, 0 of 12 on geometry, matching texture names -
  and each felt like no progress. Together they left exactly one place for the defect to be, which
  is how it got found.
- **`GKPLUS_NO_ATEST=1` joins `NO_CULL` and `NO_ZTEST`** as the third "why did this draw vanish"
  switch. A draw whose fragments are all discarded is indistinguishable from a draw that was never
  issued until you switch the test off in the *reference* and watch it appear.

## 4.41 Where this session got to

Four commits, in order: the offscreen render target; the clear colour and the state-block
recording bug; the state verifier; the arena readback and the texture-mapping check. Then
`GKPLUS_NO_BLEND`.

**Landed and measured:**

- **The 2.59 residual is gone** (§4.38). Whole frame against the real D3D8 is **0.13** on a
  settled, paused level02 frame, against a cross-launch d3d8-vs-d3d8 floor of **0.034**, with
  **93% bit-identical**, the HUD panel at 0.000 and the A4R4G4B4 probe quad at 0.0000/100%.
- **Two defects from a play report** (§4.39): the clear colour, which was a hardcoded debug
  blue-grey and is what every blended draw over an uncovered background blends *against*; and
  `Record()` applying state-block writes to the mirror while a block was being recorded, which D3D
  does not do.
- **Three instruments**, each self-tested before its results were believed: `render.verify_state`
  / `render.draw_state` (§4.40), `render.draw_geometry` (§4.40), and `GKPLUS_NO_ATEST` /
  `GKPLUS_NO_BLEND` beside the two switches §4.29 already had.

**Open, and it is one item rather than three:** the effect layers (§4.32). The plate quad and
§4.29's HUD columns are both *uncovered background*, not geometry this renderer invents — the
plan's item 1 has the repro, what is already verified and must not be re-investigated, and the
next measurement.

> **That last paragraph is wrong, and §4.42 is what it turned out to be.** Nothing was supposed
> to cover the quad: the draw was issued for a HUD panel and rendered the fire's glow quad,
> because the arena slot it reads was overwritten later in the same frame. "What is already
> verified and must not be re-investigated" was the costly part — every one of those readings was
> taken a frame or more after the draw, and each was true of a version the draw never saw.

**The methodological residue, which is the part worth re-reading:** every instrument built this
session returned a **clean negative**, and each felt like no progress at the time. 193/193 on
state, 0 of 12 on geometry, matching texture names, four switches that changed nothing on the
reference. Together they left exactly one place for the defect to be, and that is how it was
found — after a wrong diagnosis ("a spurious quad we draw and the original does not") survived
four eliminations while being false in both directions. The inference that started it was that a
*region* was empty, which is a much weaker claim than "the original draws nothing there" and does
not survive the background being black.

## 4.42 The plate quad, found: a slot frozen one rewrite too early

The quad §4.39 and §4.40 chased is a **buffer-versioning defect**, and both of those sections'
conclusions about it were wrong. It is not uncovered background, nothing was supposed to cover it,
and the draw was never issued for that geometry at all: **draw 222 is a HUD panel, and it rendered
the fire's screen-space glow quad** because the arena slot it reads had been overwritten later in
the same frame.

### The mechanism

Gunlok refills one shared 64 KB dynamic vertex buffer (fvf `0x1c4`) about **five times a frame** -
8,639 unlocks over 1,603 frames - and draws the HUD panels, the objectives text and the effect
layers' screen-space quads out of it. §4.23 already established that a slot holds one version and
the later ones are parked in the frame's scratch. The test for "park this one" was:

```cpp
const bool rewritten_after_draw = drawn_frame_ == TheStats.frames && draws_this_frame_ != 0;
```

`draws_this_frame_` counts draws **since the last rewrite**, not since the frame began - it is
zeroed each time a rewrite is versioned, so that `draws_reading_rewritten_buffers` attributes each
rewrite to the draws it endangered. That makes it exactly the wrong thing to gate the freeze on.
Two rewrites in a row with no draw between them, which is the common case in a five-refill frame:

```
unlock A  -> the slot            (no draw yet this frame)
draw 222  -> reads the slot      draws_this_frame_ = 1
unlock B  -> the scratch         versioned, draws_this_frame_ := 0
unlock C  -> the SLOT            draws_this_frame_ == 0, so not versioned  <== over draw 222's data
```

The draw list is not recorded until Present, so draw 222 renders whatever unlock C left there. The
fix is one clause: once **any** draw this frame has read the slot, the slot belongs to it for the
rest of the frame - `drawn_frame_ == TheStats.frames` alone.

`unversioned_rewrites` stayed 0 throughout, and correctly: unlock C was never *classified* as a
rewrite-after-draw, so it never reached the counter that would have said it could not be versioned.

### Why every instrument said the draw was fine

`verify_state` reported 193/193, `draw_geometry` reported 0 of 12 vertices differing, the bound
texture and the sampled image matched, `seen == submitted`, and `GKPLUS_NO_CULL`, `NO_ATEST`,
`NO_ZTEST` and `NO_BLEND` each changed nothing on the reference. All true, and all beside the point.

**Both of `draw_geometry`'s columns were read back a frame or more later**, and by then the game
had refilled the buffer - so the arena and the game's buffer agreed *with each other* on a version
neither of them held when the draw was issued. A deferred read cannot tell "the draw pulled the
wrong bytes" from "the bytes moved on afterwards", which is the entire question. Two columns were
added, both read **at the moment the draw is issued**: the game's buffer under a mid-frame
read-only lock, and the arena under a synchronous `ReadArena`. The first said 12 of 12 STALE, the
second said the arena was *correct* at that instant - which located the defect between the draw and
Present rather than in the upload.

That is a general lesson about this project's readback instruments, and it now has a name: **a
deferred readback proves consistency, not correctness.** `verify_buffers` and `verify_textures` are
built the same way; textures do not move, so they are safe, but anything the game rewrites within a
frame needs the reading taken at the draw.

### The reference was not bisectable, and that is why this took three sections

`render.draw_range` narrows the **Vulkan** list. Every follow-up question §4.39 and §4.40 wanted to
ask - what does the original paint for *that* draw, is it painted and then covered, is it painted
somewhere else - is a `draw_range` question, and there was no such switch for the runtime the layer
forwards to. So "this renderer draws a quad the original does not" could be established four
separate times and never followed.

Two instruments close that, and both work in `d3d8` and `d3d9` mode:

- **`render.ref_range` / `render.ref_hide`** simply do not forward a draw outside the window.
  Self-tested before anything was read off them: `[0, 100]` renders a partial scene, the full range
  renders 91,063 lit pixels against the partial's 10,881.
- **`render.frame_draws([first, last])`** is the capture layer's own list of the last complete
  frame - index, topology, primitive count, FVF, buffered or user-pointer, blend/src/dst, depth,
  cull, alpha test, viewport depth slice, stage-0 `.rim` name - built from the shadow state, so it
  needs no Vulkan draw list. It exists because **an index cannot be carried between runs**: aiming
  `ref_range` at 222 because a `vulkan` session called the quad 222 landed on the HUD portraits.
  With the list, the same draw is found by its signature in whichever mode is running.

Aimed correctly, the reference rendering draw 222 alone paints the two HUD panels at (564, 37) -
which is what the game's buffer held at that moment, and nothing like the corona.

### One clean negative worth keeping

`draws_refused` counts draw calls the forwarded runtime returns a failing HRESULT for, with the
first eight spelled out. Nothing had ever read a draw call's return value, and a refused call is
the one way the reference can render fewer pixels than this renderer while agreeing about every
state, every vertex and every texture. It reads **0**, so that is not what was happening - but the
hypothesis was worth a counter rather than an assumption, and `indexed draws reaching past their
bound buffer` sits beside it claiming D3D8 tolerates what D3D9 rejects, which is now measured
rather than believed.

### What it fixes

The lattice rectangle over the fire is gone, and so is the objectives text rendering as garbage -
same buffer, same defect. **§4.29's two bright HUD columns are the same defect seen from the other
side**: the reference painted the HUD columns where this renderer painted a corona, so "we
rasterise pixels the reference does not" and "the reference draws columns we do not" were one
event. Neither was about d3d8to9, mip selection, culling or the depth test, all of which §4.29 had
eliminated while looking at the wrong draw.

Invariants after the fix, level02 at the fire camera: `seen == submitted`, 0 skips of every kind,
`NOT versioned: 0`, scratch peak 1301 KB against 4096 KB (up from 1087 KB - every post-draw refill
is versioned now, which is the point), 292/292 textures, validation clean.

`verify_buffers` still reads **2952/2953**, and this is the section that explains it. The odd buffer
is *this* buffer, and it must differ: the verifier compares a slot deliberately frozen mid-frame
against a game buffer that has moved on. It now says so, and it no longer runs its re-upload
experiment on a frozen slot - which would have overwritten the version that frame's draws point at.
"A pre-transformed buffer the game refills while the verifier reads it" was the right shape and the
wrong reason.

### Three process notes

- **A wrong conclusion outlived four correct measurements.** §4.40's "draws 223 and 224 sit in the
  effect slice in front of 222, and in the original they cover it" was never checked: rendering
  them alone puts them in the **top-right corner** - they are the HUD portraits, 2 primitives each,
  fvf `0x112`, nowhere near draw 222. The neighbouring index was taken for a neighbouring object.
- **The reported symptom was right and every diagnosis of it was wrong**, three times: "a spurious
  quad we draw and the original does not" (false both ways), "uncovered background the fire fails
  to cover" (nothing was meant to cover it), and "the effect layers are dimmer and smaller"
  (they were being drawn *instead of* something else). The measurement that ended it asked about
  the **instrument** rather than the renderer.
- **A counter that never fires may be measuring a case that never reaches it.** This is §4.32's
  lesson - "every must-be-0 counter reads zero for a draw that was never offered" - repeated one
  level down. `unversioned_rewrites` is a real invariant and it was 0 for the whole life of the
  defect, because the misclassification happened before the counter.

## 4.43 Where this session got to

One defect, one line of code, and three instruments that made it findable.

**Landed and measured:**

- **The plate quad is fixed** (§4.42). A shared dynamic vertex buffer's arena slot was being
  overwritten by the second of two consecutive refills, because the freeze test asked "have there
  been draws since the last rewrite" instead of "has any draw this frame read the slot". The
  lattice rectangle over the fire is gone, the objectives text renders, and §4.29's two bright HUD
  columns were the same event seen from the other side.
- **The reference is bisectable** — `render.ref_range` / `render.ref_hide` and
  `render.frame_draws`, all three working in `d3d8` mode. Self-tested: `ref_range = [0, 100]`
  renders a partial scene against the full frame's 91,063 lit pixels.
- **`draw_geometry` reads at the draw**, not only afterwards: the game's buffer under a mid-frame
  read-only lock and the arena under a synchronous `ReadArena`, beside the deferred columns that
  were all this had. That pair is what located the defect.
- **`draws_refused`**, a clean negative: the forwarded runtime refuses nothing.

**Invariants**, level02 at the fire camera: `seen == submitted` with 0 skips of every kind,
`NOT versioned: 0`, 292/292 textures, validation clean, scratch peak 1301 KB against 4096 KB.
`verify_buffers` reads 2952/2953 and now explains itself — the odd buffer is the frozen slot, by
design.

**The methodological residue.** §4.41 said the previous session's clean negatives "left exactly one
place for the defect to be". They did not: they left one place *given* an assumption nobody had
tested, which was that a readback taken a frame later describes the frame that mattered. Three
sections were spent inside that assumption. The two questions that broke out of it were **"when is
this instrument reading?"** and **"can I ask the reference the same question?"** — and the second
had a one-word answer for three sections, which is that nobody had built the switch.

## 4.44 The material override: the first thing the bindless shape was for

Everything up to here reproduces what Gunlok already drew. This is the first piece that draws
something it never could: a mod names a `.rim` asset and says what should happen to every draw
that samples it.

**It is a rewrite of a material-table entry, not a per-draw interception**, which is the whole
reason §4.30 built the table. `GpuMaterial` is interned per frame from the D3D state, and 274
draws on level02 are 29 materials — so an override applies once where a material is built, and
every draw sharing that surface follows. The per-draw cost is one array lookup by bindless index,
and none at all while nothing is registered.

### The key is the asset name, and a level reload is why

A bindless index is assigned at image creation and depends on load order; a texture wrapper
pointer is not stable even within a session. The name is the only identity a mod author can see
(`render.textures`) and write in a file, so `render.material_override` takes a **case-insensitive
substring of the `.rim` path** — `render.probe`'s rule (§4.35).

That is not a convenience, and the measurement says so. With one override registered on
`gunlok_mk2`, quitting level02 and loading it again re-resolved the same key from **image 34 to
image 35**, and the tint reapplied to the same pixels (0.444 MAD over the same bounding box against
0.445 before). An override written against a slot number would have silently moved onto whatever
asset took slot 34.

Resolution is name → index once, not per draw: `TextureRegistryGeneration()` (VkResources) is
bumped by image create, destroy and name — the three things that change what a key matches — and
the table is rebuilt only when it moves. That is what makes the reload case work with no hook of
its own.

### What one override can do, and what each is worth

level02, settled, **paused**, `GKPLUS_RENDERER=vulkan`, against a baseline of 275 draws and 29
materials a frame with `unaccounted for: 0`. The paused-frame floor is 0.000, so every number here
is the feature and nothing else:

| `render.material_override("gunlok_mk2", …)` | frame | MAD | bounding box |
|---|---|---|---|
| `{tint: [1, 0, 1]}` | 2.04% | 0.445 | (310,240)-(385,404) |
| `{texture: "hark_512"}` | 2.19% | 0.781 | the same |
| `{hide: true}` | 2.25% | 0.995 | the same |
| `clear_material_overrides()` | **0.00%** | **0.000** | **none** |

The bounding box is the player character and nothing else — the Elint unit beside him, the ground
under him and the HUD are bit-identical in all three. The last row is the invariant that matters
most: with no override registered the frame is bit-identical to the build before this existed,
because an un-overridden material carries `tint = 0xffffffff` and multiplying by that is exactly
1.0 per channel.

Three properties fell out of the implementation rather than being designed, and each is worth
knowing before using it:

- **`texture` applies at any stage; `tint` and `hide` key on stage 0.** The replacement keeps the
  original stage's sampler and its colour/alpha ops — only the picture changes — so swapping a
  lightmap works the same way as swapping a skin. Tint and hide are properties of a *surface*, and
  stage 0 is what identifies one.
- **`tint` multiplies after the alpha test**, deliberately. A tint that could change which
  fragments are discarded would move silhouettes, and cutting holes in geometry is not what a
  colour is for. The cost is that `tint: […, 0]` does not make a surface vanish; `hide` does.
- **`hide` drops the draw, not the object.** Hiding `gunlok_mk2` leaves the character's *head*
  (a different asset, `gunloktestface_blue`) and its **stencil shadow**, which is three passes of
  its own with no texture. The picture is honest about what a material is.

`hidden_draws` is in the `seen == submitted + skips` reconciliation (§4.32). Leaving it out would
make that invariant read as broken by the one feature that drops draws on purpose, which is
exactly how a real regression stops being noticed.

Invariants with an override active: `unaccounted for: 0`, validation clean, `verify_textures()`
292/292, `verify_buffers()` 2952/2952, `dropped_materials` 0 and the same 29 materials a frame —
an override rewrites an entry rather than adding one.

### Two ways to think it is broken when it is not

Both cost time in this session, and neither is about the renderer.

- **An override that resolves, counts and paints nothing looks exactly like one that does not
  work.** `render.material_override("city ruins ground 1_a", {tint: […]})` reported image 21
  matched, `overridden_draws` climbed, and the frame was **0.000 different** — because those ~12
  draws a frame are ground sections outside the view. Two readings separate the cases: the draw
  counters say the frame drew *with* the material at all, and a key matching every named image
  (`".rim"`) is the smoke test — it moved 86% of the frame, which proved the shader path before
  any single asset was interrogated. `render.frame_draws()` names each draw's stage-0 asset, and
  is the way to pick one that is actually on screen.
- **Check the baseline is still the baseline before believing a difference.** After a long REPL
  session the "paused" frame had drifted — camera z from 2.48 to 38.99, the scene black — and a
  screenshot taken with `hide` registered was very nearly read as "hide removed the whole world".
  It was the game, and re-shooting with every override cleared is what said so. §4.21's rule
  ("pin the frame") is about two launches; this is the same rule inside one.

## 4.45 The flames that came and went with camera distance: a pre-transformed vertex's z

Reported from play: on level02 the two fires on the ledge north of the start "pop in and out
based on camera distance" — bright plumes under `d3d8`, a small glow under `vulkan`, with the
difference growing as the camera pulls back. It is one defect, it is an API rule this renderer
had wrong from the first frame it drew, and it cost every screen-space draw in every frame.

**D3D does not run the viewport transform over a pre-transformed vertex. It clamps:**

```
depth = clamp(z, MinZ, MaxZ)          no scale, no bias
```

Vulkan has no such bypass — `minDepth + z_ndc * (maxDepth - minDepth)` applies to every vertex a
pipeline rasterises. With Gunlok's world slice of `0.1..1.0` this renderer turned every
screen-space `z` into `0.1 + 0.9 * z`, pushing each one **`MinZ * (1 - z)` further from the
camera**. That error shrinks as a draw approaches the far plane, which is exactly why the symptom
was distance-dependent rather than uniformly wrong, and why thirty-odd sections of settled
world-filling frames never showed it.

### The measurement, because neither the documentation nor the game settles it

Both readings are consistent with everything Gunlok itself does, which is what made this survive
so long. The fire's vertices read `z 0.9878` under a `0.1..1.0` viewport, and §4.32's glow read
`z 0.0299` under `0.0199..0.0399` — each sits inside its own slice, so "the game authored a
device depth" and "the game authored an NDC z" both fit. Reasoning from the six slices is no
better: a ladder of layer constants is what you would build under *either* rule.

`render.depth_probe(armed, quad_z, clear_z, min_z, max_z)` settles it (`D3D8CaptureReport.cpp`).
It clears the depth buffer to a known value, sets a known slice, and draws one opaque magenta
`XYZRHW` quad with `ZFUNC LESS` and no depth write. The quad is either there or it is not, so the
reading needs no precision at all. Run against the **real D3D8**:

| viewport | quad z | depth cleared to | scaled would give | raw would give | observed |
|---|---:|---:|---|---|---|
| 0.2 .. 0.4 | 0.30 | 0.28 | 0.26 → drawn | 0.30 → absent | **absent** |
| 0.2 .. 0.4 | 0.30 | 0.99 | drawn | drawn | drawn (control) |
| 0.5 .. 1.0 | 0.60 | 0.70 | 0.80 → absent | 0.60 → drawn | **drawn** |
| 0.5 .. 1.0 | 0.60 | 0.99 | drawn | drawn | drawn (control) |
| 0.0 .. 0.5 | 0.80 | 0.90 | 0.40 → drawn | clamp 0.50 → drawn | drawn |
| 0.5 .. 1.0 | 0.10 | 0.60 | 0.55 → drawn | clamp 0.50 → drawn | drawn |

Rows 1 and 3 discriminate **in opposite directions** and both say raw, which no single row could
do — a monotonic map preserves ordering, so one test can always be explained by the other model
plus an offset. Rows 5 and 6 put `z` outside the slice and separate clamp from clip: a clipped
primitive would be absent in both, and both are drawn.

Two earlier probes, `z` outside the slice with the clear between the two candidate depths, both
came back absent and looked like a contradiction. They were not — they were the clamp, before it
was known there was one. **A probe whose input is outside the range under test measures the range
handling, not the mapping**, and running two of them first cost an hour of a wrong model.

### The fix is two halves and neither works alone

- **`BuildMvp` compensates.** The pre-transformed branch's depth row becomes
  `z_ndc = (z - MinZ) / (MaxZ - MinZ)`, so Vulkan's viewport transform hands back `z` exactly. A
  degenerate slice (`MaxZ == MinZ`, which level03's backdrop pass uses) is left alone: Vulkan
  collapses every depth onto the single value, which is what clamping to it would have produced.
- **`PipelineState::depth_clamp` carries the clamp.** With the compensation in place, "z outside
  the slice" *is* "z\_ndc outside [0,1]" — the case Vulkan clips and D3D clamps. `depthClampEnable`
  on the pre-transformed pipelines turns it back into a clamp. It is a `VkPhysicalDeviceFeatures`
  bit, requested in `VkContext` where supported and degrading to a clip where not, and it is part
  of the pipeline key because it must stay **off** for the 3D path, where D3D really does clip at
  the near and far planes.

The first cut had only the first half — viewport `0..1` for pre-transformed draws — and it is
worth recording what that looked like, because it was nearly convincing. The fire region went
15.5 → 9.97 and the whole frame 2.72 → 2.02, but the **HUD went from 0.000 to 1.406**: a status
bar beside each portrait, dim in the original, rendered bright green. That bar authors a `z`
*below* its slice, so D3D clamps it up to `MinZ` and the panel that should cover it wins; drawn
raw it landed in front of everything. A regression in a region that was previously bit-exact is
the cheapest possible signal that a rule is half-right, and it is the reason the HUD is in the
region list at all.

### What it is worth

Level02, camera pinned at `position (-19.02, -0.785, 12.96) roll 341.33 pitch 586.36
distance 60.48`, paused, against the **real D3D8** with a cross-launch d3d8-vs-d3d8 floor:

| region | before | after | floor |
|---|---:|---:|---:|
| whole frame | 2.78 | **1.92** | 0.52 |
| fire | 15.91 | **9.87** | 3.66 |
| HUD | 0.000 | **0.000** | 0.000 |

The residual on the fire is above its floor and is not claimed to be closed. Much of that floor
*is* the fire: the particle animation phase is not pinned by pausing, so two d3d8 launches differ
by 3.66 in that region against 0.52 over the whole frame, and a repeat of the "after" run read
11.09 rather than 9.87 for the same reason.

The reported symptom is better shown by counting the flame's own pixels down a distance sweep,
which needs no reference alignment at all:

| camera distance | d3d8 | before | after |
|---:|---:|---:|---:|
| 30 | 15049 | 11836 (79%) | 14029 (93%) |
| 40 | 7961 | 5372 (67%) | 7300 (92%) |
| 50 | 4712 | 2030 (43%) | 4335 (92%) |
| 60 | 3261 | 700 (21%) | 2954 (91%) |
| 70 | 2321 | 214 (9%) | 2129 (92%) |
| 85 | 1518 | 13 (0.9%) | 1385 (91%) |

**The defect is the trend, not the value.** Before, the flame's retention collapses monotonically
from 79% to 0.9% as the camera pulls back — which is the player's "pops in and out with
distance", and no single-camera MAD would have shown it as anything but a number. After, it is
flat at ~92% across the whole range, and the residual 8% is the animation phase.

### What was ruled out, and one thing worth knowing anyway

| hypothesis | test | result |
|---|---|---|
| the geometry, texture or blend of the fire draw is wrong | `render.draw_range = [166,166]` | pixel-identical to the reference's flame, and identical with the toggle either way — the draw is fine and is being *lost* |
| the fire is occluded by world geometry | `render.draw_hide = [0,165]` | restores it to exactly its isolated value, so yes — which made it a depth question and not a blending one |
| `D3DRS_ZBIAS` is being ignored | `render.state` | only ever 0, all session |
| depth-buffer precision | the new `depth buffer the game asked for:` line | the game asks for **`D3DFMT_D24S8`** and this machine's Vulkan picks `D32_SFLOAT_S8` (`D24_UNORM_S8` is unsupported there), but a 24-bit unorm step and a float32 step at `z ≈ 0.99` are both `6e-8` — no difference where the defect lives |

That last row is a real gap that this defect happened not to be: the depth format the game asked
for was recorded **nowhere** before this section, and `render.state` prints it now. Precision is
uniform for the unorm buffer and exponent-dependent for the float one, so the two agree near the
far plane and diverge near the near plane — which is the opposite end from where anything has
been measured.

## 4.46 The ledge that was much redder: a specular highlight on faces turned away from the light

Reported from play, immediately after §4.45: "the nearby concrete ledge looks much redder in
Vulkan than in d3d8". It is one line of shader, and the whole of it is a condition that reads
like a restatement of one already there and is not.

**D3D's specular sum runs only over lights with `N·L > 0`.** A light behind a surface contributes
no highlight. `max(0, N·H)` does *not* express that: `N·H` stays positive well past the point
where `N·L` goes negative, because `H` is halfway to the eye and the eye is by definition in front
of anything being rasterised. So an ungated term paints a specular wash on every face turned
**away** from a light — which on level02 is the near face of the concrete ledge and the ground in
front of it, lit orange by two fires standing behind them.

```
specular = Cs * SUM over lights with N·L > 0 of k*L.specular*pow(max(0, N·H), power)
                                ^^^^^^^^^^^ this
```

### Reading the channels is what named the term before anything was measured

The ledge read `R 56.95  G 33.56  B 15.85` against d3d8's `R 37.83  G 27.32  B 15.87`. **Blue was
identical to 0.02 and the excess ratio was 3.00 : 1.00 : 0.00.** The fire lights' *diffuse* colour
is `4.00 1.50 0.19` — ratio 2.67 with a non-zero blue — and their *specular* colour is
`0.80 0.26 0.00`, ratio 3.08 with blue exactly zero. An excess with exactly no blue can only come
from the term whose colour has exactly no blue. Six sections of whole-frame MADs would not have
said that; three numbers did.

### `render.specular`, and why one-sided switches cannot answer this

`GKPLUS_NO_SPECULAR` has existed since §4.20 and reaches **only the forwarded call**, so it
removes the term from the reference and never from us. That is enough to ask "does the original
have specular here at all" and not enough for anything else: with only one side switchable, "we
add specular the original does not" and "we add three times as much of it" are the same
measurement. `render.specular` is the other half, and with both, on one paused frame:

| draws 0..138 (all lit world geometry) | before | after |
|---|---:|---:|
| base R, vulkan vs d3d8 | identical, **max per-pixel diff 0** | identical, max diff 0 |
| specular R, vulkan mean | 15.162 | **5.421** |
| specular R, d3d8 mean | 5.421 | 5.421 |
| peak, both | 157 | 157 |
| pixels with specular in one only | 46,644 vulkan-only, **0 d3d8-only** | **0 and 0** |
| max per-pixel specular difference | 122 | **0** |

The specular term is now bit-identical to D3D's over all 104,693 lit pixels. Level02 at the fire
camera, whole frame, against the real D3D8 with a cross-launch floor of 0.521: **2.093 → 0.522**.
The frame is at the floor — a second d3d8 launch differs from the first by as much as this
renderer does.

### The eliminations, and why each mattered

The excess was 3.16× and *uniform in colour*, which reads exactly like a scalar error in `Cs`, so
the first hour went on inputs. All of them were right, and the instrument that said so is new:
**`render.draw_state` now dumps the lighting equation's inputs read off the device at the moment a
draw is issued** — the material with its `POWER`, the enabled lights with their colours, ranges
and attenuation, the four `D3DRS_*MATERIALSOURCE` states, the FVF, and the eye with the view
matrix it came from.

| candidate | reading | verdict |
|---|---|---|
| the mirror is lying | `render.draw_state` | 191/191 states match the device |
| `Cs` — the material specular colour | device says `1.00 1.00 1.00` | same as ours |
| `power` — the exponent | device says `POWER 1.000` | same as ours |
| the light colours, range, attenuation | device says `0.80 0.26 0.00`, range 6, atten `0.9599 0.0333 0.1666` | same as ours |
| `to_eye` / the halfway vector | dumped the view matrix and inverted it properly in Python | rigid to 1e-5; `StoreEye` is right to **0.0006 units** |
| the depth-slice work of §4.45 | base is bit-identical | unrelated |

**`render.state` answers none of those questions**, and that is why the dump exists: it prints the
material and lights for the *last* draw of the frame, which on a level02 frame is the text.

### What actually found it, after every input had been eliminated

Two readings, neither of which is a difference image:

- **The base of the offending draw is neutral grey.** Draw 92's unlit-by-specular base reads
  `R 161.2 G 160.9 B 160.4` — no orange at all, so the fire lights contribute **no diffuse** to
  it. And yet we were adding their specular, in their colour, at `R 2.97 G 0.99 B 0.00`. A light
  that contributes no diffuse and some specular is `N·L ≤ 0` with `N·H > 0`, which is the defect
  stated as a measurement.
- **Zero pixels where d3d8 has specular and we do not**, in either direction of the map, ever.
  Our set strictly *contained* D3D's, and inside it we matched exactly. A scale error does not do
  that; a missing condition does. The map was the reading that ruled out every "we compute it
  wrong" hypothesis in one image, and it was taken before the cause was known.

### Two traps this cost time to

- **Editing a `.slang` and rebuilding changes nothing.** SPIR-V is compiled offline into
  `src/Shaders.gen.inc.h` and the header is the checked-in artifact — nothing in CMake runs the
  generator, deliberately, so `d3d8.dll` needs no shader toolchain. Run `python src/gen-shaders.py`
  **and then** `cmake --build build`. The first measurement of the fix reported it had changed
  nothing, and the screenshots were byte-identical to the ones before it — which is the signature.
  Hash the shots when a change is supposed to move pixels and does not.

  **No longer true, and §4.50 is the fix**: `cmake --build` compiles the shaders now, and refuses a
  stale header on a machine that cannot. The *signature* in the last sentence still stands and is
  the part to carry forward.
- **A draw index found by `find-draw.ps1` is the draw that painted that *pixel*, not the draw that
  produced the *difference* being chased.** Aiming at a pixel where both renderers agreed found
  draw 12, whose specular is zero in both, and half an hour went into asking why an agreeing draw
  disagreed. Pick the pixel off the *difference* map, not off the frame.

## 4.47 The upgrade screen filling the window: D3DVIEWPORT8 has a rectangle as well as a slice

Reported from play, and the third such report in a row: "the inventory / upgrade screen shows up
as filling the whole screen, and as a result selection rectangles and text are shifted".

**Gunlok sets two viewport rectangles.** Everything in a level uses `0,0 640x480`, the whole
backbuffer. The upgrade screen sets **`32,24 575x431`**, and it is the only thing in the game that
does — which is why this survived forty-six sections with every counter clean, and why no
whole-frame number measured on a level is affected by the fix.

This layer recorded `D3DVIEWPORT8`'s `Width`, `Height`, `MinZ` and `MaxZ`, and never `X` or `Y`.
The depth slice went onto the `DrawItem` in §4.32; the rectangle did not, and the world pass set
one Vulkan viewport over the whole render target for every draw. So on that screen:

- the **3D** draws — the panel is `fvf 0x152`, and the character `0x252`, both untransformed —
  had their NDC spread over 640x480 instead of 575x431, growing by `640/575` and anchored at 0,0,
  so the edges fall off the window;
- the **pre-transformed** draws were scaled by `BuildMvp`'s `2/width` in the same proportion;
- and the frame **mixes rectangles** — the HUD plates at draws 0-2 stay at `0,0 640x480` while
  everything from draw 3 on is the sub-rectangle — so the parts that moved ended up displaced
  *relative to* the parts that did not. That is the "shifted" half of the report, and it is why
  it does not read as a plain zoom.

The plan had predicted the shape of this exactly, in the sentence next to `distinct viewport
rects ever set`: "a sub-viewport would have to move onto the `DrawItem` beside
`min_depth`/`max_depth`". The marker fired the first time the screen was opened.

### The rule the fix needed, and why it had to be measured

Putting the rectangle on the Vulkan viewport is not the whole of it, because **Vulkan applies its
viewport transform to every vertex and D3D does not**. The 3D path is fine either way. The
pre-transformed path needed an answer to: *does D3D add the rectangle's origin to a `D3DFVF_XYZRHW`
vertex, or are its x and y absolute screen pixels?*

The documentation does not settle it, and — exactly as in §4.45 — **no reading of Gunlok's own
draws can**, because every rectangle it had ever set was at `0,0`, where the two answers coincide.
So it was asked of D3D directly. `render.viewport_probe(armed, x, y, w, h)` draws one opaque
magenta `XYZRHW` quad 20 pixels in from the rectangle's own origin, and the reading is a
**differential**, which is what makes it immune to the frame the shot is taken in:

| viewport rect | quad authored at | measured, in backbuffer pixels |
|---|---|---|
| `0,0 200x150` | (20,20) | (31,64) window px |
| `100,60 200x150` | (120,80) | (129,123) window px |
| | | **delta 100,60 — exactly what the coordinates moved by** |

Origin added would have moved it by 200,120. **D3D ignores the rectangle's X/Y for a
pre-transformed vertex.** So `BuildMvp` **subtracts** it, cancelling what Vulkan's viewport is
about to add, and a vertex at pixel `p` lands on pixel `p` under any rectangle.

A third question came free from the same probe: with the rectangle set to `0,0 60x40` the 64x32
quad rasterises **780 pixels** where the whole quad is ~1960 and the clipped part is ~800. **D3D
clips to the rectangle**, and a Vulkan viewport does not — so the rectangle is the **scissor** as
well as the viewport. All three parts are one state and they are set together in `RecordDraws`.

### What it is worth

On the upgrade screen, level02, Gunlok selected, against the **real D3D8**:

| | whole-frame MAD | bit-identical |
|---|---|---|
| `render.viewport_rect = false` (the old behaviour) | **17.23** | 5.5% |
| `render.viewport_rect = true` | **0.089** | 95.4% |
| the floor: two Vulkan shots of the same screen | 0.043 | 99.7% |

The residual is the character, which idle-animates — the difference bounding box is his torso and
head and nothing else. Validation clean.

In level, the toggle is worth **nothing at all**, and that is the reading rather than a
disappointment: two pinned level02 shots across the toggle differ by 0.0869 at 99.85% identical,
which is *the same number* two `d3d8` shots of the same pinned frame differ by. One rectangle in a
level means the two behaviours are the same code path.

### Three things this cost time to, all of them about the instrument

- **`utils/rendertest/shot-gunlok.ps1` was passing `PrintWindow` flag 2, not 3.** The plan has
  said flag 3 — `PW_CLIENTONLY | PW_RENDERFULLCONTENT` — since §4.20, and the script disagreed
  with it. `PW_RENDERFULLCONTENT` alone renders the **whole window**, title bar and border
  included, into a bitmap sized from `GetClientRect`: the picture is pushed down and right by the
  border and the bottom and right edges of the game's own frame fall off the bitmap. It is silent
  — the shot looks like a screenshot of a game in a window. Every shot in this repo taken before
  this section is missing about 40 rows and 10 columns of what it was measuring. Cross-launch
  comparisons between two such shots stay valid; anything about *where* something is does not.
- **A comparison against a baseline shot in a different game state reads as a renderer defect.**
  The first world-frame regression check read **2.14** against d3d8 and the fix looked like it had
  broken the level. It had not: that session had opened the upgrade screen and *selected a unit*
  on the way, so the game was drawing a selection marker and a different HUD. Re-run from a clean
  launch with the identical script it is **0.329**, and the toggle inside one session moves it by
  the noise floor. §4.44's rule — re-shoot the baseline before believing a difference — applies to
  the game's state and not only to a drifted pause.
- **The upgrade screen will not open while the game is paused**, so this is the one comparison
  that cannot use the "pin the frame" procedure. `render.viewport_rect` exists for that: the A/B
  has to happen inside one session, on consecutive shots, and the 0.043 repeat floor above is what
  makes that good enough.

## 4.48 Lighting maps: a texture the capture layer never saw, addressed by file name

The first thing here that loads an asset of its own. `render.material_override` (§4.44) proved a
material-table entry can be rewritten, and could only ever point at a texture the *game* had
already loaded — which is enough to prove a mechanism and not enough to reskin anything. This is
the other half §5 named: an image created, uploaded and given a bindless slot by this side, that
D3D has never heard of.

**The whole interface is a file name.** For a texture the renderer knows as `Ground\gunlok
rust.RIM`, `graphics/ground/gunlok rust lighting.dds` — from a mod under `gkplus/mods` or from the
install — is loaded, and every material whose **stage 0** is that texture carries it. Nothing is
registered, no script call exists to make one appear, and a texture with no companion costs one
hash lookup for the life of the session. `src/VkLighting` is the whole subsystem; the shader half
is `shade_lighting_map` in `world.slang`.

Three channels, and the semantics are deliberately not PBR's: **R is a height field**, **G is the
highlight's intensity** and **B is its sharpness**. The normal is derived from R's gradient at draw
time against a tangent frame taken from the fragment's own derivatives (Mikkelsen's cotangent
frame), which is what lets the canonical 48-byte vertex stay as it is — a tangent would have cost
12 bytes on every vertex in the game to serve the few surfaces a mod maps.

### It is keyed like the override, and resolved on the same trigger

`TextureRegistryGeneration()` again: an image created, destroyed or named is the only event that
can change what a name-keyed table resolves to, so `EnsureLightingMapsResolved` is a comparison
against a counter per draw and a rescan when it moves. The generation is re-read at the **end** of
a resolve rather than the start, because creating and naming our own image bumps it — reading it
first leaves the table permanently stale and rescans every draw.

Two things fall out of the two features sharing a name space, and both are real:

- **A lighting map's own name contains its base texture's**, so `render.material_override("lava")`
  would find `bitmaps\lava lighting.dds` as readily as `bitmaps\lava.rim` and could swap one in as
  a replacement texture. `IsLightingImage` keeps them out of both name searches.
- **The map is keyed on the stage-0 texture *after* an override may have replaced it**, which is
  the opposite of how `tint` is keyed and is deliberate: a retextured wall is a different surface,
  and giving it the old wall's bumps would be wrong.

### What had to be measured, and what each measurement changed

Every one of these was a default this feature would have shipped wrong.

- **Gunlok's lights author a black specular colour where it matters.** All 49 `light` sections in
  the shipped `.gsh` set carry a `specular red/green/blue` — but on level02 the four lights
  reaching the ground (`render.draw_state`) read `specular 0.00 0.00 0.00` every one, including
  the key directional. Keyed on the authored specular, the metallic channel does **nothing at all**
  over most of a level: measured, the highlight moved 0.27% of pixels at 0.002 MAD. So
  `specular_from_diffuse` exists and defaults to 1 — the highlight reflects the light a player can
  see — with 0 restoring the game's own answer, which is what the fixed-function specular term uses.
- **Gunlok over-drives its lights**, and that sets `specular_scale`. Level02's key light is
  `diffuse 4.00 4.00 4.00`. With the highlight taking that colour, a fully-metallic texel at scale
  1.0 saturates a whole floor to white. The default is **0.25**, the reciprocal of that intensity,
  so `metallic = 1` reaches exactly 1.0 at normal incidence.
- **A bump that only shapes highlights is invisible wherever metallic is 0**, which is most of a
  real map. So the derived normal also reaches the diffuse, as a *ratio* — the same light sum with
  the bumped normal over the same sum with the geometric one — blended by `bump_diffuse`, default
  1. A ratio because the fragment does not know which colour `D3DRS_DIFFUSEMATERIALSOURCE`
  selected, and the ratio cancels it; the ambient term is in both halves so a surface facing away
  from every light does not divide by nothing, and it is clamped to 4x.

### The measurements

On level02, paused, camera at rest, against the same frame with `render.lighting_maps = false`,
with a synthetic 256x256 map (checkerboard height, metallic 1, roughness 0.25) on the two textures
that actually cover the frame:

| | |
|---|---|
| off vs on, defaults | **2.00 MAD over 22.3% of the frame** |
| off vs on, `bump_diffuse 0`, `specular_scale 6`, `gloss_max 32` | 54.1 MAD over 58% — the highlight alone, and blown out, which is what set the default |
| **off vs off across the toggle** | **0.0000 MAD, 0 pixels** — an un-mapped material is byte-identical to the build before this |
| mod-served vs install-served | the same picture; the report says which, `mod:graphics/...` or the absolute path |
| validation, `seen == submitted`, materials, pipelines | unchanged — 0 errors, 274 draws / 29 materials / 12 pipelines |

Both container forms are verified in the running game: uncompressed A8R8G8B8 (which needed the one
new entry in `MapFormat`, `VK_FORMAT_B8G8R8A8_UNORM` — the game has never created that format, so
`unsupported_formats` reading 0 for the renderer's whole life is what says adding it changes
nothing) and **DXT1**, which is what a modder should use: three channels, no alpha, 8 bytes a
block. The decoder is `src/Dds` unchanged, shared with the engine-facing codec — so DXT5 is
refused **by name** here too, which is stricter than Vulkan needs and keeps one set of rules for
one file format.

### Two traps, one of which cost most of the session

- **A key that resolves and paints nothing looks exactly like a broken feature** — §4.44's trap,
  and it caught this one anyway. The first two textures mapped were `Ground\city ruins ground 1_a`
  and `Ground\Ruins_MESSY CONCRETE 1024`; the report said 2 images and 3400 draws lit, and the A/B
  moved **zero pixels**. Both are drawn every frame and neither is *visible* from the camera at
  rest. What settled it was not reading more of the shader: `render.material_override(".rim",
  {tint})` moved 99.4% of the frame, proving the material path, and then tinting twelve candidate
  keys one at a time ranked them by how much of the frame each owns — `city ruins water tranch`
  58%, `gunlok rust` 25%, the two originally chosen 0.15%. **Rank your target by tinting it before
  concluding anything about a feature that paints through a material.**
- **A cache of misses has to be droppable, or a map cannot be authored.** The negative cache is
  what keeps a texture with no companion from costing a file probe per frame; it also means a file
  dropped in while the game runs is never noticed. `render.lighting_maps = false` then `true` now
  destroys every image and clears the cache, so it is a full reload — measured, an edited map
  changes 17% of the frame on the next toggle, and `images: 55 live / 63 created` says the old
  ones went.

## 4.49 Gunlok already had a sphere map, and it collided with §4.48

`chrome_enabled` @ 0x006a3000 — the `CHROME` console command's byte — is **on by default in the
file image**, and it is live retail code rather than one of the game's several dead features. It
made §4.48 apply a lighting map **twice** to every reflective unit, which is what sent this
looking.

### What the engine does

Chrome is **not a modification of a draw; it is an extra draw of the same mesh**. A `Unit` whose
`Role+0x78` carries bit 0x08 — the `reflective` flag, and 48 shipped roles set `reflective yes` —
gets an 0x80-byte `WorldEffect` of kind 7 from `CreateChromeEffect` @ 0x0051bd30, called by
`Unit_EnterWorld` @ 0x004b57c0. That is **Unit-tree vtable slot 51**, and `FUN_004b5b20` runs it
over every entry of `UnitsTable` at level start, from the client's session-start handshake — so
these exist from the moment the level is up rather than one per later spawn. `CreateChromeEffect`
is idempotent per owner: it pre-walks `WorldEffectList` for a kind-7 entry whose `+0x34` is this
unit. `DrawWorldEffects` @ 0x005201c0, case 7, re-submits that geometry every frame with a second
material:

| | per-unit `ChromeMaterialUnit` (`0x007b9e80`) | map-wide `ChromeMaterialMap` (`0x007b9f90`) |
|---|---|---|
| stages | 2 | 1 |
| stage 0 | **the unit's own texture**, bound at draw time by `SceneMesh_Render` | `units\reflect.rim` |
| stage 1 | `units\reflect.rim`, `COLOROP = ADDSIGNED` | — |
| `TEXCOORDINDEX` | 1 — a plain second UV set, no texgen | **0x00010000 = `D3DTSS_TCI_CAMERASPACENORMAL`** |
| reachable by | any `reflective` role | typing `REFLECT` with nothing under the cursor |

The map-wide one is effectively debug-only, but it is worth keeping in view for one reason: it is
**the only texture-coordinate generation anywhere in gl.exe**, and so it is the engine's own
statement of how a chrome coordinate should be built. Neither `D3DTSS_TEXTURETRANSFORMFLAGS` nor
`D3DRS_SPECULARENABLE` is set anywhere in the binary.

### The collision, and why it was the good kind

§4.48 hands a lighting map to every material whose **stage 0** is the mapped texture. The chrome
pass's stage 0 *is* that texture — so a reflective unit with a companion file got the bump-derived
normal and the highlight computed once per pass, with the ADDSIGNED reflect layer stacked on the
second. On by default, no user action, 48 roles.

The same accident is what made the fix cheap. Because stage 0 is the unit's texture, the chrome
pass's `material.lighting_texture` **already resolves**, with no new plumbing to find the map. So
rather than suppressing the map on that pass, the pass now *uses* it:

- **`texgen_only`.** `shade_lighting_map` runs the height gradient and returns. No light loop, no
  highlight, no diffuse ratio — those already ran in the base pass, and running them again was the
  double-apply. Incidentally the cheap half: the chrome pass costs a few taps rather than a light
  sum.
- **The coordinate is generated from the bumped normal**, as `N_view.xy * 0.5 + 0.5`. That formula
  is the engine's, taken from the map-wide material above and applied per pixel to a normal the
  height field has tilted. An authored UV1 is fixed to the surface and cannot respond to a height
  field at all; this is the only coordinate that can.
- **The metallic channel weighs the reflection**, exactly as it weighs the highlight. G is
  documented as intensity rather than a metal/dielectric switch, so one channel answering "how
  shiny is this texel" for both responses is the coherent reading rather than a reuse.
- **Roughness was meant to blur it**, as a mip bias, on the same reasoning. **It does not work,
  and `chrome_blur` therefore defaults to 0** — see the measurements below.

### The measurements

level02, Vulkan, the settled start camera (`p{-9.909, 4.508, 2.482}`, roll 341, d 20), frame
pinned with `screen.toggle_pause()`, all shots in one session. `render.lighting_map_report` reads
`1 slot hold units\reflect.rim` and 86,850 chrome draws; `render.draw_info` finds **90 draws a
frame** with `units\reflect.RIM` at stage 1.

| comparison | MAD /255 | pixels differing |
|---|---|---|
| same settings twice — **the floor** | 0.104 | 0.21% |
| chrome on vs off (`chrome_scale` 1 vs 0) | 0.435 | 2.61% |
| generated coordinate vs the engine's UV1 | **0.573** | 2.99% |
| engine's UV1 vs chrome off | 0.389 | 2.47% |
| `chrome_scale` 1.0 vs 0.5 | 0.224 | 1.94% |
| `chrome_scale` 0.5 vs off | 0.217 | 1.94% |
| `chrome_blur` 0 vs 8 | 0.004 | 0.03% |

Reproduced across two independent runs (0.435 against 0.431, 0.573 against 0.574). The two
`chrome_scale` halves being equal within 0.007 is the check that the weighting is linear and that
blending the stage's *result* degenerates the way it was meant to.

The headline is the third row: **the generated coordinate moves the picture more than chrome
itself does**, so the two coordinates genuinely disagree, and the engine's UV1 is not a sphere
projection of anything the camera is doing. Visually the UV1 pass washes the units evenly while
the generated one picks out surfaces by facing, which is what a reflection should do.

**`chrome_blur` does not work**, and this is unresolved rather than explained. Ruled out: the
texture (`units\reflect.RIM` is 256x256 and carries all 5 mips), the channel (the unit maps' B
averages 0.58, so the requested LOD is ~2.3 not 0), the push-constant layout (`chrome_scale` and
`chrome_texgen` sit either side of it and both work), and the branch (`chrome_texgen` only acts
when the shade ran, so `shade.roughness` is being written). The open lead is the sampler: the
chrome stage is `D3DTEXF_NONE` (`render.state` shows `filt 220`), which `AcquireSampler`
reproduces as `maxLod = 0.25` per §4.28, so a bias cannot reach a second level. `MippedSamplerFor`
was added to hand that stage a mipping variant and should have fixed it — but the bindless table
still reports **4 samplers** with `chrome_blur` at 20, so the swap is not allocating one. Check
the sampler count first.

### Two traps in the instrument, not in the renderer

- **The camera edge-scrolls while you are not looking.** Between two shot batches it walked from
  `x -9.9` to `x -55.2` — the cursor was sitting over the window. Every knob then measured 0.000
  or exactly the floor, which reads precisely like a dead feature; it was §4.44 again, with the
  reflective units simply off screen. Park the cursor at the window centre, and **assert the
  camera before and after a batch** rather than only settling before it. `camera.position` is
  writable, which makes recovering a known viewpoint a one-liner rather than a relevel.
- **A REPL string arrives with literal `\n`,** not newlines, so `-split "`n"` silently returns one
  element and `Where-Object { $_ -match ... }` matches the entire blob. It looks like a successful
  filter that found one row. Unescape first.

### The three things that are easy to get wrong here

- **Scale the stage's RESULT, not its argument.** The stage is ADDSIGNED — `current + (tex - 0.5)`
  — so a zeroed *argument* darkens the surface by half instead of leaving it alone. Blending
  between the value before the stage and the value after it degenerates exactly at 0, is
  bit-identical at 1, and does not hard-code the op.
- **An absent lighting map must read as metallic 1.0.** A 0 default is the natural-looking one and
  it silently deletes the reflection from all 48 reflective roles on a stock install — a
  regression that would read as "GkPlus broke chrome" rather than as a missing file.
- **`StoreViewRotation` does not transpose, and `StoreEye` does.** They sit next to each other and
  take the same matrix. `StoreEye` transposes because it is *inverting* the view to recover the
  camera position; the sphere-map coordinate runs in the same direction the game does. Getting it
  backwards mirrors the reflection left-to-right, which looks like a plausible picture rather than
  like a bug.

### A correction this turned up

`AwTextureStage +0x2c` was modelled in `src/Render.h` as `field0x2c`, commented "read by
[`AwMaterial_ApplyStage`] but not issued as a stage state". It **is** issued — as
`D3DTSS_TEXCOORDINDEX`, and issued *last*, so it overrides the identity write that function makes
first. It is now `texcoord_index`. The prior comment is what an audit that stops at the first
write concludes; the field is the whole reason the chrome pass has a second UV set at all.

### Not yet measured

The per-unit chrome path uses UV set 1 with no texgen, and **whether a `reflective` unit's mesh
carries authored sphere-map UVs there or is reusing a lightmap set is not established** — nothing
in the chrome path touches vertex data, so the binary cannot answer it. It decides how this
feature should be described: if UV1 is authored sphere coordinates, `chrome_texgen` is a
deliberate change of look; if it is a lightmap set, the engine's own chrome pass was always wrong
and this is a fix. One reflective unit through the Blender addon settles it.

Also open, and a design question rather than a defect: a metallic texel now carries the map's
highlight (base pass) **and** the reflection (chrome pass), both scaled by the same G. The clean
resolution would be for the base pass to know a chrome pass is coming and back its highlight off —
a **cross-draw dependency**, which is a different class of thing from anything else in this
renderer. Not built speculatively; `specular_scale` is the lever until it is shown to need one.

## 4.50 The shaders are built by the build now, and a stamp alone was not enough

§4.46's second trap — editing a `.slang`, rebuilding, and measuring that nothing changed — is a
defect in the *build*, not in anyone's attention, and it gets worse the moment there is more than
one shader source. This closes it. It is a prerequisite for the per-pixel lighting work rather
than part of it: every measurement that work takes is worthless if the binary can silently hold
the previous SPIR-V.

**The header stays checked in.** That is the constraint, not a detail: `d3d8.dll` must build on a
machine with no Vulkan SDK, which is the same reasoning that makes the renderer reach Vulkan
through volk rather than the loader's import library. So the requirement is not "compile the
shaders" but **"never silently compile against a stale header"**, and it has two halves:
regenerate where `slangc` exists, and **fail the build** where it does not and the header is stale.

### The one thing that had to be right, and it was wrong first

**The generated header must be a declared `add_custom_command` `OUTPUT`, not merely a side effect
of one that outputs a stamp.** The first version stamped only, on the reasoning that the generator
rewrites the header just when its contents change, so an unchanged shader should leave every
dependent TU out of the rebuild. It does — and it also loses the dependency entirely: Ninja treats
a file no edge declares as a plain source, decides `VkDraw.cpp.obj` is clean from the header's
*current* mtime while building the graph, and only then runs the generator. Measured on a real
edit: `[1/1] Compiling Slang shaders`, `wrote src/Shaders.gen.inc.h`, no recompile, and a d3d8.dll
built against the previous SPIR-V. That is §4.46's exact symptom with an extra step in front of it,
and it would have been read as "the fix does nothing".

Both outputs is the answer, and each does a different job:

- the **header**, so Ninja knows it is generated and orders every consumer after it;
- the **stamp**, always touched, so the edge is clean next build even on the runs where the header
  was not rewritten — without it `slangc` runs on every single build.

CMake sets `restat = 1` on custom-command edges, which is what makes the pair work: the generator
writes the header only on a real content change, so an unchanged shader leaves its mtime alone and
Ninja re-stats and keeps the dependents clean. Verified in both directions — a real edit rebuilds
`VkDraw.cpp` and relinks, and two consecutive builds report `ninja: no work to do`.

### Staleness is a hash, because mtimes lie

`--check` needs no `slangc`, so it can run on the machine that cannot regenerate. It compares
hashes embedded in the header as comments: one over each source's bytes and one over the *recipe*
(`ENTRY_POINTS` + `SLANGC_ARGS`), so adding an entry point or changing a flag marks the header
stale even though no shader file moved. A timestamp rule would report stale on a clean tree and
fresh after a branch switch that changed a shader — a git checkout does not preserve mtimes.

`--deps` prints the sources `ENTRY_POINTS` names, and `CMakeLists.txt` consumes that at configure
time rather than listing shaders itself. Two lists would drift, and the failure mode is a shader
that silently stops being rebuilt — which is the thing this section exists to prevent.

### What was measured

| | |
|---|---|
| regenerating the checked-in header with the local `slangc` | **byte-identical SPIR-V** — 3558 and 5886 words, only the new hash comments differ |
| second build with nothing changed | `ninja: no work to do` |
| a real shader edit | generator runs, header rewritten, `VkDraw.cpp.obj` rebuilt, DLL relinked |
| the same with the stamp as the only output | header rewritten, **nothing recompiled** — the defect above |
| `--check` against an edited shader | exits 1, names `src/shaders/world.slang` and the command to run |
| `--check` after changing `-O2` to `-O1` | exits 1, names the recipe |
| a configure with `slangc` forced absent, then an edited shader | build **fails** with the same message |

Every one of those guards was fired deliberately before being believed. A d3d8.dll hash is **not**
usable as the signal here, incidentally — MSVC embeds a link timestamp, so it changes on every
relink whether or not any input did; the reading is whether `VkDraw.cpp.obj` appears in Ninja's
output.

## 4.51 There is no lightmap: stage 1 is the fog of war, and the name was a join artefact

§4.19 identified the second texture stage as the whole flat-and-bright gap, which was right, and
called it "the lightmap … `bitmaps\LEVEL01.rim`", which was not. It is the game's **fog of war**.
This matters beyond tidiness: a plan to re-light Gunlok's maps at runtime has to know what of the
existing picture is baked lighting to be replaced, and the answer is now **`SHPVTINT` and nothing
else** — there is no baked lightmap *texture* anywhere in a frame.

### What was measured

level02, Vulkan, settled start camera (`p{-9.909, 4.508, 2.482}`, roll 341, d 20), paused, 178
actors, 273 draws. Every stage-1 configuration in the whole frame, by texture and colour op:

| stage-1 texture | op | draws | what it is |
|---|---|---|---|
| `-1`, none | — | 112 | single-stage draws |
| **8** — unnamed, 256x256, `format 28` = `D3DFMT_A8`, 1 level, 65536 bytes | `0x0d` `BLENDTEXTUREALPHA` | **71** | the fog of war |
| **92** — `units\reflect.RIM`, 256x256 DXT1 | `0x08` `ADDSIGNED` | **90** | the chrome pass (§4.49) |

Both stage-1 subsystems are therefore accounted for, and 90 reproduces §4.49's independently
measured "90 draws a frame with `units\reflect.RIM` at stage 1" exactly.

`render.draw_info(3)` on a world draw, decoded:

```
0: tex 28 colour 0x00000204   MODULATE(TEXTURE, DIFFUSE),        texcoord 0
1: tex  8 colour 0x0101020d   BLENDTEXTUREALPHA(TEXTURE, CURRENT), texcoord 1
                    alpha 0x00010203   SELECTARG2 - the stage does not touch alpha
```

**The causal test, which is what settles it rather than the arithmetic**: `world.fog.enabled =
false` takes that draw from **2 stages to 1**, with stage 1's texture going to `-1` and its ops to
zero; setting it back restores `tex 8` and op `0x0d`. Nothing about a lightmap would respond to the
fog switch.

### Why it had to be the fog, in hindsight

Four things already in these notes point at it, and each was read as being about a lightmap:

- **An `A8` texture samples as `(0,0,0,a)`**, so `BLENDTEXTUREALPHA(TEXTURE, CURRENT)` is
  `lerp(current, black, a)` — darkening toward black by a per-texel amount. `world.fog` reports
  `color {0,0,0,1}`, i.e. **black**, which is the same statement from the other side.
- §4.19's own `D3DFMT_A8` swizzle defect is a fog defect: `{ONE,ONE,ONE,R}` instead of
  `{ZERO,ZERO,ZERO,R}` "faded the distance to **white** instead of to black". A lightmap has no
  reason to be alpha-only.
- §4.19's `GKPLUS_NO_STAGE1` A/B renders "the ceiling structure that should be hidden clearly
  visible". Hidden structure *becoming visible* is fog of war. A missing lightmap brightens; it
  does not reveal.
- `stealth_and_fog_notes.md` had the whole other half: a **256x256** grid uploaded every frame, and
  `SubmitAndFlushMapGeometry` using the fog material as the map's material when fog is on. The two
  files were describing one thing and neither cited the other.

### The name, and the trap it is an instance of

`bitmaps\LEVEL01.rim` is the **minimap / briefing map**: 512x512 DXT1, and decoding it shows a
top-down render of the level with a radar reticle in the corner. It is one of a set — one per level,
sitting in `Graphics\Bitmaps` beside the splash screens and briefing backdrops — it comes from the
`.gls` `map` section's `bitmap` field, `pbr/README.md` measures it at 5,445 draws, and **it is not
resident at all while a level is up** (absent from `render.textures` on the frame above). It could
not have been on 75,000 two-stage draws.

Two ways the wrong name survived so long, both worth carrying:

- **A name arrived at by joining two lists is not a measurement.** The `.gls` names a per-level
  `bitmaps\<level>.rim`, the frame has a per-level stage-1 texture, and the join is irresistible and
  wrong. The stage-1 texture has **no name at all** — the engine creates it, so nothing in
  `AcquireRimTexture` ever sees it, which is exactly why §4.14's "one unnamed image, a 256x256 with
  no cache record" was sitting in these notes unexplained.
- **A contradiction in the arithmetic was available the whole time.** `BLENDTEXTUREALPHA` reads the
  stage texture's *alpha*, and `bitmaps\LEVEL02.rim` is DXT1 **with no alpha** — under which that op
  degenerates to replacing the surface with the texture, i.e. painting the world with a picture of
  itself from above. The named candidate could not perform the op it was credited with.

### What is still open

**Where `uv1` comes from is still not established**, and this narrows it rather than answering it:
the coordinate now has a known consumer (a level-wide 256x256 grid), which makes a world-space
planar projection the obvious shape, but no uv1 value has been read. `render.draw_geometry` prints
positions and colours and not UVs, and the `.rif` carries one UV list (`SHPUVCRD`), so it is
generated somewhere in the geometry builder `rendering_notes.md` §5 leaves undissected.

This also tightens §4.49's "not yet measured": that section asked whether a `reflective` unit's UV1
is authored sphere coordinates or "a lightmap set". **There is no lightmap set**, so the second
option as phrased is gone — but a unit's UV1 and the world's UV1 need not be the same thing, and
only the world's has been looked at here.

Incidentally, the vertex dump on that draw shows diffuse `ff080808`, `ff0c0c0a`, `ff090908` — the
`SHPVTINT` bake, and `0xFF080808` is the commonest value in the whole shipped set.

## 4.52 The light sum, per pixel — and a cross-launch MAD that cannot see it

The first change here that is meant to make the game look *different* rather than to make it match.
D3D8's light sum now runs per fragment; `render.per_pixel_lighting = false` restores the
fixed-function path. **Worth 0.48 MAD over 26.9% of a paused level02 frame.**

The equation is untouched — the same lights, the same attenuation and spot factors, the same
`N·L > 0` gate on the specular sum (§4.46). What changes is what the rasteriser interpolates:
a finished colour, or the position and normal it is computed from.

### It is mostly promotion, not new code

The fragment shader already contained a working per-pixel light loop — `shade_lighting_map`
(§4.48) runs the same `light_geometry` over the same `GpuLight` array — and `VertexOut` already
carried `world_position` and `world_normal` for it. So the work was to factor the loop into
`light_sum` and the material resolution into `resolve_lit_colour`, and call them from either stage.
Both are shared *deliberately*, for §4.46's reason: two copies of the same sum diverging silently
is a defect that reads as an authoring problem rather than as a shader disagreeing with itself.

**No new varying.** On the per-pixel path the vertex shader writes the **raw vertex colour** into
`color` instead of a lit one, which is what `D3DRS_*MATERIALSOURCE` needs and the only thing
per-vertex data still contributes. Both stages decide which meaning `color` carries from the same
push constant, so they cannot disagree.

Three real differences, each a departure rather than an approximation:

- **The interpolated normal is renormalised per pixel**, where the vertex path normalises only
  under `D3DRS_NORMALIZENORMALS`. Interpolating two unit normals gives a shorter vector between
  them, so skipping it darkens the middle of every triangle — an artefact of interpolation, with
  no per-vertex equivalent to reproduce.
- **`D3DRS_LOCALVIEWER`'s eye vector is per fragment.**
- **`D3DSHADE_FLAT` now flattens the vertex colour the sum consumes**, not the sum's result.
  Invisible on level01 and level02, where every flat-shaded draw is one of the stencil shadow's
  passes and none is lit (§4.31).

Ordering matters in one place: the per-pixel sum runs **before** the lighting-map block, because
that feature's whole output is a scale on the diffuse the texture stages consume. Computing the sum
afterwards would discard the scale and look exactly like the map having stopped working.

### What it is worth

level02, Vulkan, settled start camera, paused, 178 actors, 273 draws, all in one session:

| | MAD /255 | pixels |
|---|---|---|
| off vs off, two shots — **the floor** | 0.0083 | 0.05% |
| off → on → off, back to off | 0.0168 | 0.07% |
| **off vs on — the feature** | **0.4794** | **26.9%** |

The floor is not 0.000 as §4.48's was, and the reason is visible rather than mysterious: the
differing pixels sit in a single 92x79 box, one animating prop. Everything else is static.

**The difference image is the reading, not the number.** It is shaped exactly as a per-vertex →
per-pixel change should be: the two units blaze (the most articulated geometry in the frame, where
Gouraud loses most), broad smooth gradients appear across the large ground and structure polygons
where linear interpolation of a colour cannot follow a falloff, and the flat mid-right is black.

### `false` is bit-identical — and a whole-frame cross-launch MAD cannot say so

This is the part worth carrying forward. Checking "off restores the previous build" needs two
*builds*, so it cannot use the toggle, and a cross-launch comparison on level02 runs straight into
§4.31's warning that **the animation phase is not pinned**:

| comparison | whole frame |
|---|---|
| old build vs old build, two launches | 0.0198 |
| old build vs new build, per-pixel off | 0.1223 (reproduced at 0.1200) |
| **old build with a 3-second delay vs old build without one** | **0.6712** |

That last row is the same binary against itself, differing only in when the pause landed — and it
is five times the effect being measured. `Shoot-Settled`'s `-Before` sleeps three extra seconds, so
a shot that sets a knob and one that does not are at different animation phases. Two units
idle-animate and nothing else in the frame moves.

**Restricting to regions with no animating geometry is what makes it answerable**, and the answer
is unambiguous:

| region | old vs old | old vs new, off |
|---|---|---|
| pipe + upper-left | 0.0000 (0.00%) | **0.0000 (0.00%)** |
| right wall | 0.0000 (0.00%) | **0.0000 (0.00%)** |
| HUD panel | 0.0000 (0.00%) | **0.0000 (0.00%)** |
| far ground upper-mid | 0.1187 | 0.1193 — the floor, so something animates here too |
| whole frame | 0.6712 | 0.7119 — the floor again |

So the fixed-function path is reproduced **exactly**, and every whole-frame number above is the
game rather than the renderer. Three procedural rules fall out:

- **Match the `-Before` between two shots being compared**, or compare only static regions. A knob
  set through `-Before` costs three seconds of animation.
- **`render.draws` collapsing to a couple of dozen a frame means the shot is void.** One relaunch
  here landed the camera somewhere empty and produced 31.77 MAD against everything — read as a
  catastrophic regression for exactly as long as it took to notice `draws: 20 this frame`. Assert
  the draw count and the actor count beside the camera.
- **A cross-launch whole-frame MAD on level02 has a floor of order 1**, not 0.09. §4.28's 0.094 was
  measured with the camera set explicitly and is not what `Wait-CameraRest` alone delivers.

Invariants unchanged: `seen == submitted` with `unaccounted for: 0`, every must-be-0 counter at 0,
0 validation errors, 13 pipelines (unchanged — nothing here touches the pipeline key), and
`render.draws` now prints `light sum: per PIXEL` or `per vertex (the original)` unconditionally,
because "no line" would read as the original to anyone who had not been told the feature existed.

## 4.53 The level's own lights, loaded — the rig that baked it, which nothing reads

Phase 3a of runtime map lighting: get the `STDLIGHT` set into world space. Nothing renders from it
yet; this is the loader and the reading that says it found the right file and put the lights in
the right place.

`src/Rif` is the decoder and is **pure** — bytes in, records out — so it is the only file in
`src/` with a test that runs without Gunlok: `utils/riflights` over all 563 shipped files against
`blender/io_scene_rif`, **3,794 lights, every field exact**, integers with no tolerance. Breaking
one offset fails it 3,794 times.

### It has to be a hook, and on the right function

**Neither the path nor the rif object survives the level load.** `LoadLevel` calls
`RifCache_Clear` @ 0x004aead0 immediately after `ConvertParsedObjects` (@ 0x004e0e70), and
`LoadOrGetRifFile` clears the cache on every miss as well — so by the time a level is playable the
rif is freed and the cache is empty. No global holds either: `ToMap` keeps the handle in a
register, and `Map` retains only the **shadow** object's rif name (field 0x54), never the map
section's own `file` (0x01).

So it is caught in flight, and the seam is **`LoadOrGetRifFile` @ 0x004ae960** rather than the
more obvious `AcquireLevelRifForLocators`: `ToMap` reaches Acquire on both warm paths but calls
`LoadOrGetRifFile` **directly on the cold one**, which is the load where a level is being built
for the first time. Hooking Acquire alone would work on every run except the first.

`__fastcall` with arguments in ECX *and* EDX, so it is a free function pointer and not the
member-pointer trick `InputFix` uses — a `__thiscall` member puts only `this` in ECX and
everything else on the stack.

**The shortcut was measured and does not work.** Deriving the rif path from `ScriptFileName` holds
for 28 of the 32 shipped scripts and fails on four: `prison.gls` → `levels\S3 Level.rif`,
`cityruins.gls` → `levels\city ruins.rif`, `mplay_machine.gls` → `levels\mplay_the machine.rif`,
and `railway.gls` → `railway.rif` with no `levels\` prefix at all. Guessing would have produced no
lights on a campaign level and looked like a feature that simply does nothing.

### What it reads back

| | level02 | level01 |
|---|---|---|
| lights | **51** | **686** |
| omni (flag 7) | **27** | **285** |
| light set / ambience | `NORMALLT` / 0.0312 | same |
| unit scale | 0.001 | 0.001 |
| range | 3.29 .. 83.59 | 5.00 .. 111.70 |

Every one of those is independently predicted: the counts and the omni split match the Python
decoder's, 0.0312 is 2048/65536 exactly, and the ranges are the file's rif units times the scale.
0.001 is the default scale, which is what **every** shipped asset uses — only 2 of 563 files carry
an `ENVSDSCL` at all.

**The bounds comparison is the check that the transform is right**, and it is the one no per-light
number could give: the light set's world bounds sit inside the map's own on both levels. Get the
scale or the origin wrong and they land orders out.

### Two corrections it produced

- **`Map::bounds_min` and `bounds_max` were named backwards.** 0x128 holds the *larger* corner:
  level02 has (68.6, 10.0, 66.5) there against 0x134's (-65.8, -14.1, -65.1), and level01 (47.4,
  30.0, 141.0) against (-50.8, -28.5, -96.1) — six components, two levels, all the same way round,
  and the lights bracket correctly only when read this way. Renamed in `src/Map.h`,
  `address_map.md` and `level_loading_notes.md`, including the `MapCameraPlane` fallback formula
  there, which was written against the old names and had to be rewritten to keep its meaning.
- **`src/CustomLevel.cpp`'s comment was wrong in both halves.** "The rif is already in the cache by
  now, so this is a lookup, not a load" — the cache is empty by then, and the `.loc` branch passes
  flag 0, which flushes and re-reads even on a hit. Every `LevelRifLocators` call is a full disk
  load.

Also corrected from the same investigation: `rif_chunk_format.md`'s `Chunk` layout (it is **0x28**,
not 0x24, and every field after the vptr was at the wrong offset; `children` is a head pointer, not
a list), and `level_loading_notes.md`'s claim that `.opt`/`.loc` are "shipped with the rif" — they
are generated by `ToMap`'s cold path, and both carry the whole `LIGHTSET`.

### Not yet decided, on purpose

`MapLight` carries the orientation as a raw 3x3 and has **no `direction` field**. Which row a
light points along is not established: AvP's own consumer reads neither the orientation nor
`spread` (`setup_light_data`, `avp/win95/Objsetup.cpp:3362`), and the shipped data refuses the
tidy reading that the orientation is authored only where the flags ask for a spread — it is
authored on both. `engine_light_flags` does decode cleanly against AvP's header (3 =
`CosAtten|CosSpreadAtten`, 7 = that plus `Omni`), which is more than the notes had before.

The only ground truth for what actually shaped the bake is `SHPVTINT` itself, so the offline fit
against it is what gets to choose — inventing a direction here would bake a guess into an ABI that
the fit would then have to argue with.

One incidental measurement worth keeping: the game loads the level rif **20 times on level02 and
43 on level01** during a single level load (`rif loads seen ... under levels\`), and per the above
each of those is a real disk read rather than a cache hit.

## 4.54 What baked the levels: the model, fitted against the bake itself

Phase 3b has to choose an attenuation curve, decide whether there is a diffuse term, and settle
whether `spread` and the orientation matrix mean anything — and AvP's own consumer of the same
chunk reads neither of the latter two. A guess here is a plausible picture with no way to tell it
is wrong. `SHPVTINT` is ground truth, so the choice gets a residual instead.
`utils/riflights/fit_bake.py` is the harness; it needs no game, only the shipped files and
`blender/io_scene_rif`.

### The model

```
for each light with d < range:
    atten  = 1 - d/range                                  # linear, beats cosine and inverse-square
    atten *= max(0, N·L)
    if not (flags & LFlag_Omni):
        atten *= max(0, dot(row2(orientation), normalize(P - light.position)))
    sum += colour * brightness * atten
result = max(ambience, sum * gain)                        # gain 0.9 .. 1.35
```

### The residuals, which are what "same-ish" means

| level | lights | r | MAE /255 |
|---|---|---|---|
| level05 | 356 | **0.957** | **4.87** |
| level04 | 431 | **0.926** | **9.85** |
| level01 | 686 | 0.875 | 27.73 |
| level02 | 51 | 0.367 | 36.38 |

### The ablation is the evidence, term by term

Pearson r against baked luminance, each row adding one term to the row above:

| | level05 | level04 | level01 | level02 |
|---|---|---|---|---|
| distance only | 0.360 | 0.506 | 0.399 | −0.131 |
| + `max(0, N·L)` | 0.838 | 0.831 | 0.720 | 0.197 |
| + cone on **every** light | 0.923 | **0.640** | 0.809 | 0.235 |
| + cone, **omni exempt** | **0.957** | **0.926** | **0.875** | 0.367 |
| cosine falloff instead of linear | 0.942 | 0.918 | 0.850 | 0.359 |
| inverse-square instead | 0.891 | 0.847 | 0.831 | 0.215 |

Three things that are measurements rather than choices:

- **The axis is row 2** of the orientation. Rows 0 and 1 make the fit worse; **negating row 2
  collapses it** — 0.02 on level05, 0.17 on level01. A wrong axis degrades a fit; the *opposite*
  axis destroys it, which is what tells one from the other.
- **`LFlag_Omni` is a real switch.** Level04 goes 0.831 → **0.640** when the cone is applied to
  its omni lights and to 0.926 when they are exempted. That is a term making things worse in one
  configuration and better in another, which no amount of gain fitting can fake.
- **`spread` shapes nothing.** As a cone exponent it makes the fit worse everywhere
  (level01 0.723 → 0.646). It is authored on every light and read by nothing — the same standing
  as `V` on an ambient emitter.

### Level02 is the outlier, and it is the level everything else is measured on

r 0.367 against 0.87–0.96 elsewhere, and it is the only level where the hue test fails: the baked
colour is no closer to the dominant light's hue (0.44) than to a randomly chosen light's (0.33),
where level04 gets 0.378 against a 0.885 control. It also has **66% of its bake effectively grey**
against level01's 13%.

The explanation is in its light rig: **51 lights with ranges up to 83,600 rif units over a
130,000-unit level**, so every vertex is within range of a mean of 11.5 of them and no light
dominates anywhere. There is nothing for a spatial model to discriminate on. Every term still
improves the fit in the same order as elsewhere, so this is a level whose bake the lights explain
weakly rather than a level that contradicts the model.

**That matters for phase 3b's testing**, because level02 is the level every renderer measurement
in these notes is taken on. Runtime map lighting will look *least* like the bake exactly where it
will be judged. Shoot level04 or level05 as well before concluding anything about the feature.

### Two ways this could have gone wrong, and what ruled them out

- **A wrong placement transform would destroy any correlation.** Both map objects turn out to have
  an identity `OBJHEAD1` (location 0, quaternion identity), and their local vertex extents match
  the map bounds read out of the running game to the digit — level02's −65754..68633 against
  −65.8..68.6 at scale 0.001. That also independently confirms §4.53's `bounds_min`/`bounds_max`
  swap.
- **A misaligned vertex↔colour index would look exactly like a bad model.** `SHPVTINT` count,
  shape vertex count and vertex-normal count are all 16026 on level02, and every normal is unit
  length.

## 4.55 Runtime map lighting: substituting into the slot the bake occupies

Phase 3b. `render.map_lighting` replaces the level's baked per-vertex colour with a per-pixel
evaluation of §4.54's fitted model over its own `STDLIGHT` rig.

**It substitutes rather than adds.** `D3DRS_DIFFUSEMATERIALSOURCE` is `D3DMCS_COLOR1` on every lit
draw, so the vertex colour is the *material diffuse* inside D3D's own equation, not the final
pixel. Replacing that one term leaves the live light sum, both texture stages and the gamma-space
multiply untouched, which is why the level's brightness balance survives. Neutralising the bake to
white and lighting on top is §4.25's defect.

**No gamma conversion anywhere**, and that is a consequence of how the fit was run rather than an
omission: it was fitted against the stored bytes over 255, so the model's output is already in the
encoding the stages consume. The fit absorbed the transfer function.

### The offline fit predicts the on-screen optimum

This is the result worth keeping. §4.54's gain is one free parameter recovered from *vertex* data
with no rendering involved, and it puts level04 at **1.35**. Sweeping the knob in game against the
bake, on a paused level04 frame:

| gain | 0.9 | 1.1 | **1.35** | 1.6 | 1.9 | 2.3 |
|---|---|---|---|---|---|---|
| MAD from the bake | 6.333 | 4.990 | **4.170** | 7.414 | 11.404 | 16.483 |

A clean minimum at exactly the fitted value, rising sharply either side. Two independent
measurements of the same quantity — one over 4,384 vertices offline, one over a rendered frame —
agreeing to within a sweep step.

### Only the map's own geometry, and that too is measured

The first version substituted on **every** lit draw and measured worse. A prop or a unit is a
separate `RBOBJECT` carrying its own file's bake from its own light rig, so putting the *level's*
rig there swaps one object's bake for another's — something §4.54's fit never covered, since it
only ever looked at the map object.

| level04, at gain 1.1 | MAD from the bake |
|---|---|
| map geometry only | **4.990** |
| every lit draw | 6.954 |
| floor (off vs off) | 0.013 |

The marker is the **fog stage**: stage 1 is the fog-of-war grid on exactly the draws
`SubmitAndFlushMapGeometry` submits, and §4.51 tallied a whole level02 frame into only three
stage-1 groups — fog, chrome, none. So "two stages and not chrome" is "this is the map".
`render.map_lighting_all` turns the restriction off, which is what keeps that claim checkable.

Note the ordering effect: with the restriction *off*, the on-screen gain optimum sat at 1.1
instead of 1.35, because the units were contributing error that dragged it. Fixing the wrong thing
first would have made the fit look 20% out.

### What it is worth, and where to judge it

| level | fit r (§4.54) | MAD from the bake, at the fitted gain |
|---|---|---|
| level04 | 0.926 | 4.170 |
| level02 | 0.367 | 3.316 (gain 1.0, unrestricted) |

**Do not read those two against each other.** Level04's bake is brighter and far more saturated
(mean luminance 78.8, saturation 0.437) than level02's (60.8, 0.183), so the same *relative*
accuracy shows up as a larger absolute difference — level04 fits nearly four times better on
vertices (MAE 9.85 against 36.38) and still moves more pixels. A screen MAD against the bake
measures how much the feature *changes*, not how wrong it is; §4.54's residual is the accuracy
number.

Invariants unchanged: `seen == submitted` with `unaccounted for: 0`, every must-be-0 counter at 0,
0 validation errors, 12 pipelines, and off↔on returns to a 0.008–0.013 floor.

### Off by default, and it is a performance decision not a fidelity one

Brute force over every light in the level per pixel — 686 on level01, 431 on level04 — with no
culling until phase 2. Turning it on is what says whether the shading is right; leaving it on is
not yet advisable. The lights ride in a **sixth per-frame scratch slice** rather than a
device-local buffer of their own, deliberately: the set is static for a whole level so a permanent
upload is strictly better, and is exactly what phase 2 has to build anyway, so refilling 33 KB a
frame is the cheaper thing to be wrong about while the shading is still being judged.

## 4.56 The light grid: a compute pass, and the first one in this renderer

Phase 2. Phase 3b evaluated all 431-686 of a level's lights per pixel, which is why it had to
default off. This bins them into a grid so a fragment reads only its own cell — and it is the
first compute shader here, so the pipeline, the descriptor set and the dispatch are all new.

### A world-space grid, not the view-space cluster grid the plan named

The plan specified screen tiles with depth slices, rebuilt per frame. **These lights never move
relative to the world**, which changes the answer:

- it is built **once per level** rather than once per frame — measured, `grid builds` reads 1 and
  stays there;
- it needs no view matrix in the fragment shader. A screen-tile scheme needs a view-depth slice,
  and the view matrix here is per *draw*, inside `GpuDrawRecord` — a real source of quiet error;
- it is resolution-independent. This machine runs at **3072x1728**, where 16-pixel tiles with 24
  depth slices is nearly 500,000 clusters; this is 16,384 whatever the window is.

The trade is that it does nothing for a moving light. Gunlok's dynamic lights are the D3D ones,
capped at eight per draw by the hardware and already carried per draw, so there was nothing there
for a view-space scheme to earn.

32 x 16 x 32 cells over the map's own bounds — half the resolution vertically because a level is
far flatter than it is wide (level01 is 98 x 58 x 237 world units).

### The test is bit-identity, and it passes

**The grid is exact, not an approximation.** A light's `range` is a hard cutoff in §4.54's model,
so a light whose sphere misses a cell contributes exactly zero to every fragment in it. That makes
`render.map_light_cull` a *correctness* A/B rather than a quality trade — and it is the only thing
that can catch a cell silently missing a light, which otherwise looks like art.

On level04, paused, culled against brute force: **0.00000 MAD, 0 pixels differing, over the whole
frame.**

Getting to that number took one correction worth recording. The raw whole-frame comparison read
0.017 against a same-setting floor of 0.007, with a reproducibly *wider* bounding box — which
reads exactly like a small systematic error. It is the **"ACTIVE PAUSE" indicator blinking**:
amplifying the difference shows the text and one animating effect and nothing else, and excluding
that strip takes every comparison — both floors and both across-toggle pairs — to 0.00000 with a
`None` bounding box. A blinking HUD element is a *phase*, so it lands in some pairs and not
others, which is why it moved the box rather than the magnitude.

### Mechanics

- **`src/shaders/lightgrid.slang`**, one compute entry point. `gen-shaders.py` needed exactly the
  one `ENTRY_POINTS` tuple §4.50 predicted; nothing else in that script is stage-aware.
- **The dispatch is the renderer's, not `RecordDraws`'**, because a compute dispatch inside
  `vkCmdBeginRendering` is invalid and `RecordDraws` runs there. `BuildLightGrid` sits beside the
  uploads, before the world pass begins.
- **The compute pass binds three storage buffers; the fragment reads two by device address.**
  Asymmetric on purpose: the bindless set has no storage binding and cannot gain one (binding 1 is
  pinned last by `VARIABLE_DESCRIPTOR_COUNT`), so the graphics side must use addresses — while a
  shader that *writes* is the one place a plain binding is simpler than arguing about pointer
  semantics. The compute layout is its own, so this costs the graphics path nothing.
- **Two passes over the lights per cell** — count, one atomic, then write — rather than gathering
  into a local array, which would spill 128 uints per thread.
- A cell that would overflow the index budget is dropped **whole** rather than half-filled, so
  exhaustion renders as an unlit cell instead of as plausible-but-wrong lighting.

### The push block is now exactly 128 bytes, which is not slack

Adding the grid's two addresses and its parameters took the block to **184 bytes — past the 128
every Vulkan device guarantees**, and AMD commonly reports exactly 128. Caught by the
`static_assert`, which is why that assert exists.

The fix moved the grid's dimensions, origin and cell size **into the grid buffer's own header**
(written by `vkCmdUpdateBuffer`, 48 bytes inline in the command buffer) and packed three booleans
into one `map_flags` word. That lands at exactly 128. **There is no room left**: the next thing to
need push space has to displace something, or move the frame-uniform knobs into a uniform buffer,
which is what they should have been once there were more than a couple.

### Still off by default, and what would change that

`render.map_lighting` stays off. The point of this section was to make it *affordable*, and it
does — but **that it is affordable is not measured**: no frame-time comparison has been taken with
it on against off. Flipping the default is a measurement away, not an opinion away.

## 4.57 The push block ran out, and the frame-uniform data moved into a buffer

A prerequisite for phase 4 rather than a feature: **no pixel changes**, and that is the test.

§4.56 took the push constants to exactly **128 bytes** — the minimum every Vulkan device
guarantees, and what AMD commonly reports. The next thing to need space was phase 4's light-space
matrix, which is 64 bytes on its own, so shadows could not start until this was fixed. §4.56's own
note said what the fix was: a value that is the same for every draw in a frame has no business
being copied into a 128-byte block once per draw.

`GpuFrameData` is now one 96-byte record in a seventh scratch slice, written once at the top of
`RecordDraws` — the ten lighting-map and chrome knobs, the LOD probe, `per_pixel_lighting`, the
whole map-lighting block, and the three addresses only that path reads. The push block is
**56 bytes**, of which the only genuinely per-draw fields are `record`, `material` and
`base_vertex`.

**The four hot addresses stayed in the push.** The vertex shader reaches `vertices` before it has
read anything at all, and putting it behind `frame` would make that a dependent load on every
vertex in the game to save eight bytes that were not needed.

### Proving a refactor changed nothing, across two builds

The same problem §4.52 had: two *builds* cannot use a run-time toggle, and a cross-launch
whole-frame MAD on a level with animation has a floor of its own. Here all three modes — map
lighting off, culled, brute force — differed from the pre-refactor build by **0.27 with an
identical bounding box**, which is itself the tell: a refactor that had broken the lighting path
would move "culled" and leave "off" alone.

Restricted to regions with nothing animating in them, against the pre-refactor build:

| region | map lighting off | map lighting culled |
|---|---|---|
| right of the animation | **0.00000** | **0.00000** |
| top strip | **0.00000** | **0.00000** |
| far left | **0.00000** | **0.00000** |

Bit-identical. Within the build, `culled` against `brute` is still at the floor, so §4.56's
exactness survived the move.

### What is now unblocked

72 bytes of push, a frame-uniform record with three spare words, and somewhere obvious to put a
light-space matrix. Phase 4 - the sun's shadow map, then cascades, then the static atlas for the
map lights - is what this was for, and none of it is started.

---

GkPlus is a modding framework with a JS layer, a VFS and a REPL; the renderer should join that
rather than sit beside it. Concretely: a `render` namespace (draw-list introspection, material
override, register a post-process pass), and shaders loaded through `src/Vfs` so a mod ships
`.spv`/`.glsl` in `gkplus/mods/*.zip` like any other asset. Compile the defaults offline with
`glslc` and embed them; add optional runtime `shaderc` for hot reload, which is nearly free once the
VFS is the loader.

**Open product question, not a technical one:** whether Vulkan eventually becomes the only path. All
in is simpler, but it makes a Vulkan-capable GPU a hard requirement for a mod to a 2000 game.
Deferred until Phase 4.
