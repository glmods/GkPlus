# Replacing the renderer with bindless Vulkan

The design record for the Vulkan backend. `rendering_notes.md` is the analysis of what Gunlok's
renderer *does*; this file is what replaces it and why. Read §1 before anything else — it is a
measurement that overturned the obvious plan, and every other decision here follows from it.

Status: **Phases 0, 1, 2a, 2b, 2c-i and the vertex half of 2c-ii done, all verified on a
running game.** The D3D8 capture layer
(`src/D3D8Capture.cpp`) mirrors the fixed-function state, replays state blocks into it, and
reduces every draw to a material and a pipeline key, and tracks buffer residency — while still
forwarding every call unchanged. `GKPLUS_RENDERER=vulkan` puts a Vulkan swapchain on the game's
window, presents a clear colour and draws the ImGui overlay on it, clean under validation. Results and the traps
they cost are §4.1 through §4.11. Nothing is drawn from game geometry yet — that is Phase 3.

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

One slice per frame in flight, reset from the same place `ReleaseFrameStaging` is called — the
frame's fence having been waited on is the same proof for both.

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
They are the next thing after fog.

## 5. Fitting the project

GkPlus is a modding framework with a JS layer, a VFS and a REPL; the renderer should join that
rather than sit beside it. Concretely: a `render` namespace (draw-list introspection, material
override, register a post-process pass), and shaders loaded through `src/Vfs` so a mod ships
`.spv`/`.glsl` in `gkplus/mods/*.zip` like any other asset. Compile the defaults offline with
`glslc` and embed them; add optional runtime `shaderc` for hot reload, which is nearly free once the
VFS is the loader.

**Open product question, not a technical one:** whether Vulkan eventually becomes the only path. All
in is simpler, but it makes a Vulkan-capable GPU a hard requirement for a mod to a 2000 game.
Deferred until Phase 4.
