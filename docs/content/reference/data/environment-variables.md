---
title: "Environment variables"
description: "Every GKPLUS_* variable d3d8.dll reads, plus the variables the Python tools read, with accepted values and defaults."
weight: 10
audience: ["player", "mod-author", "developer"]
---

Launch-time configuration for players, mod authors and developers. Each variable is read by
`d3d8.dll` unless the table it appears in says otherwise. Variables are read from the process
environment; Steam does not pass them to `gl.exe`.

The list of names is derivable:

```bash
grep -rhoE '"GKPLUS_[A-Z0-9_]+"' src/ | sort -u
```

Unless a row says otherwise, a variable is read **once**, either while `d3d8.dll` attaches or the
first time the value is needed, and a later change to the environment has no effect.

An environment variable outranks `settings.json`. A renderer knob that carries a companion
variable is skipped in both directions while that variable is set, whatever its value. See
[settings.json](/reference/data/settings-json/).

## Profile and scripting

| Variable | Values | Default | Effect |
|---|---|---|---|
| `GKPLUS_PROFILE` | path | `gkplus` beside `d3d8.dll` | The profile directory: `settings.json`, the boot and entry modules, and the anchor a relative mod path resolves against. A relative value resolves against the directory holding `d3d8.dll`, not against the process's current directory. `src/Profile.cpp:56` |
| `GKPLUS_REPL_PORT` | `0`, `auto`, or `1`–`65535` | unset, no listener | Opens the loopback JavaScript REPL on `127.0.0.1`. `0` and `auto` bind an ephemeral port. A value that is neither `auto` nor a port in range logs `repl: GKPLUS_REPL_PORT is not a port; the channel is closed` and the channel stays shut. `src/Repl.cpp:640` |
| `GKPLUS_LAUNCHER_HWND` | window handle, decimal or `0x`-prefixed | unset | A message-only window of class `GkPlusLauncher`, posted `RegisterWindowMessage("GkPlusReplPort")` with the pid in `wParam` and the port in `lParam` once the listener accepts. A handle that is not a window, or whose class name is not `GkPlusLauncher`, is refused with a log line. `src/Repl.cpp:577` |

## Renderer selection

| Variable | Values | Default | Effect |
|---|---|---|---|
| `GKPLUS_RENDERER` | `d3d8`, `d3d9`, `vulkan` | `d3d9` | `d3d8` loads Windows' own `d3d8.dll` from the system directory as the inner runtime; a system runtime that will not load falls back to d3d8to9. `vulkan` draws with the Vulkan renderer. Any other value, and any unset value, uses d3d8to9. The resolved value, rather than the requested one, is what `d3d8::RendererName()` reports. `src/D3D8Capture.cpp:3261`, `src/VkRenderer.cpp:959` |
| `GKPLUS_VK_PRESENT_MODE` | `immediate`, `mailbox`, `fifo` | `mailbox` when the surface offers it, otherwise `fifo` | Vulkan present mode. A mode the surface does not offer is refused with a log line rather than approximated. `src/VkRenderer.cpp:262` |
| `GKPLUS_VK_OFFSCREEN` | `0`, `off`, `no` disable; any other value enables | enabled | Whether the world pass rasterises into a target of its own and blits, rather than into the swapchain. `src/VkRenderer.cpp:715` |

## Vulkan features

Where a row names a settings key, that key is skipped in both directions, neither restored from
`settings.json` nor written back to it, for as long as the variable is set to anything at all,
`0` included.

| Variable | Values | Default | Settings key | Effect |
|---|---|---|---|---|
| `GKPLUS_VK_MSAA` | integer sample count | `1` | `core.render.msaa` | Multisample count for the world pass. `0` and an unparseable value are ignored. `src/VkDraw.cpp:228` |
| `GKPLUS_VK_HDR` | `0`, `off`, `no` disable; any other value enables | off | `core.render.hdr.enabled` | The float world target and the tonemap pass. `src/VkDraw.cpp:3438` |
| `GKPLUS_VK_BLOOM` | `0`, `off`, `no` disable; any other value enables | off | `core.render.bloom.enabled` | Bloom. Requires HDR. `src/VkDraw.cpp:3654` |
| `GKPLUS_VK_PER_PIXEL_LIGHTING` | `0`, `off`, `no` disable; any other value enables | on | `core.render.per_pixel_lighting` | Per-pixel rather than per-vertex evaluation of the light sum. `src/VkDraw.cpp:2484` |
| `GKPLUS_VK_LOCAL_SHADOWS` | `0`, `off`, `no` disable; any other value enables | on | none | Shadows for the point and spot lights. `src/VkDraw.cpp:1314` |
| `GKPLUS_VK_STOCK` | `0`, `off`, `no` disable; any other value enables | off | none | Applies the stock preset once, after the world pipeline is up: every departure from the original renderer switched off. Read once per process, so a device recreation does not re-apply it. `src/VkDraw.cpp:4040` |

## Measurement instruments

Each of the eight `GKPLUS_NO_*` variables is enabled only by the exact value `1`, is read once
while `D3D8CaptureSystem` is constructed, and holds one Direct3D render state off **in the
forwarded call only**, that is, in the reference renderer and not in the Vulkan path
(`src/D3D8Capture.cpp:3461`–`3470`).

| Variable | State held off |
|---|---|
| `GKPLUS_NO_LIGHTING` | Fixed-function lighting |
| `GKPLUS_NO_STAGE1` | Every texture stage past the first |
| `GKPLUS_NO_SPECULAR` | The specular term |
| `GKPLUS_NO_MIPMAP` | Mip selection: `D3DTSS_MIPFILTER` forced to `D3DTEXF_NONE` |
| `GKPLUS_NO_CULL` | Backface culling |
| `GKPLUS_NO_ZTEST` | The depth test |
| `GKPLUS_NO_ATEST` | The alpha test |
| `GKPLUS_NO_BLEND` | Alpha blending |

Bisect switches for the capture and Vulkan layers:

| Variable | Values | Default | Effect |
|---|---|---|---|
| `GKPLUS_WRAP_BUFFERS` | `both`, `vb`, `ib`, `none` | `both` | Which D3D buffer objects the capture layer wraps. `src/D3D8Capture.cpp:114` |
| `GKPLUS_TEXTURE_UPLOAD` | `both`, `seed`, `blits` | `both` | Which half of the texture upload path runs. `src/D3D8Capture.cpp:134` |
| `GKPLUS_VK_TOPOLOGIES` | `all`, `1`, `strip`, `line`, `none`, `0` | `all` | Which non-triangle-list topologies the Vulkan renderer draws. `src/D3D8Capture.cpp:3477` |
| `GKPLUS_VK_SKIP` | any subset of the letters `t`, `s`, `l`, `d` | none | Vulkan features switched off: `t` topologies past triangle lists, `s` seeding a buffer from its own contents, `l` the material colour for unlit-vertex draws, `d` the API's initial state defaults. `t` also forces strips and lines off. `src/D3D8Capture.cpp:3473` |
| `GKPLUS_VK_WATCH_DST` | integer, `strtoul` base 0 | unset | Logs writes to one destination arena offset. `src/VkResources.cpp:1157` |

## Diagnostics and capture

| Variable | Values | Default | Effect |
|---|---|---|---|
| `GKPLUS_PROFILER` | `1`, `zones`, `stacks`, or any value not starting `0` | off | Arms the CPU profiler. `zones` records instrumented zones only; `stacks` adds frame-pointer walks to the sampler; any other non-`0` value gives zones and the sampler. `src/Profiler.cpp:994` |
| `GKPLUS_PROFILER_HZ` | integer `1`–`8000` | 1000 | Sampling-thread rate. A value outside the range is ignored. `src/Profiler.cpp:1000` |
| `GKPLUS_VK_VALIDATION` | exactly `1` | off | Requests `VK_LAYER_KHRONOS_validation` and the debug-utils extension. Ignored when the layer is not installed. `src/VkContext.cpp:242` |
| `GKPLUS_VK_HEAPS` | `small` | full | Small Vulkan arenas: 16 MB vertex, 2 MB index, 8 MB staging. `src/VkResources.cpp:94` |
| `GKPLUS_RENDERDOC` | unset or a value starting `0` disables; any other value enables | off | Loads the RenderDoc in-application API. An already-loaded `renderdoc.dll` is used in preference to loading one. `src/VkCapture.cpp:68` |
| `GKPLUS_RENDERDOC_DLL` | path | `C:\Program Files\RenderDoc\x86\renderdoc.dll` | Which `renderdoc.dll` to load. The default is the 32-bit build; the x64 one fails to load. `src/VkCapture.cpp:82` |
| `GKPLUS_FILE_STATS` | set and not starting `0` | off | Arms the read-count instrument behind `mods.read_stats()`. `src/FileHooks.cpp:820` |

## Behaviour switches

Each of these is on by default and turns the corresponding fix off, restoring the stock behaviour.

| Variable | Values | Default | Effect when disabled |
|---|---|---|---|
| `GKPLUS_HUD_FIX` | `gkplus` keeps it on; `raw` turns it off. Any other value also turns it off, with a log line. | on | The HUD batch is not split, so no health meter, armour meter or item icon is visible in any level. `src/HudFix.cpp:105` |
| `GKPLUS_WINDOW_PLACEMENT` | `work-area` keeps it on; `raw` turns it off. Any other value also turns it off, with a log line. | on | The windowed-mode window is created at the hardcoded 0, 0 rather than at the monitor's work-area origin. `src/WindowPlacement.cpp:84` |
| `GKPLUS_VERSION_TEXT` | `gkplus` keeps it on; `raw` turns it off. Any other value also turns it off, with a log line. | on | The game's own `v1.3 DX8` stamp is drawn in place of `GkPlus - <renderer>`. `src/Font.cpp:98` |
| `GKPLUS_32BIT_TEXTURES` | `raw` turns it off; any other value, and unset, leaves it on | on | `Use32BitTextures` is left as the game's configuration set it, so an uncompressed 24- or 32-bit DDS does not get an `A8R8G8B8` surface. `src/ImageCodec.cpp:407` |
| `GKPLUS_FILE_BUFFER` | a value starting `0`, `r` or `R` turns it off; any other value keeps it on | on | The 64 KB read-ahead layer over the engine's file reads is not installed. `src/FileHooks.cpp:824` |

Two more that change behaviour but are not stock-restoring switches:

| Variable | Values | Default | Effect |
|---|---|---|---|
| `GKPLUS_LOAD_PRESENT_MS` | integer milliseconds, or a value starting `r`/`R` | `32` | Minimum interval between presents during a level load. A value starting `r` or `R`, and any negative number, disables the throttle. `src/LoadScreen.cpp:104` |
| `GKPLUS_RENDER_UNFOCUSED` | exactly `1` | off | Keeps rendering while the game window is inactive. Any other value, `0` included, leaves the game's own behaviour alone. `src/GUI.cpp:312` |

## Variables the Python tools read

Not read by `d3d8.dll`.

| Variable | Read by | Default | Effect |
|---|---|---|---|
| `GUNLOK_DIR` | `pbr/`, `lightmap/` | the Steam registry entry | The Gunlok install directory. `pbr/gkpbr/assets.py:77` |
| `GKPBR_OUT` | `pbr/` | `pbr/out` | Where `gkpbr` writes its stages. `pbr/gkpbr/cli.py:24` |
| `GKPBR_RIMUTIL` | `pbr/` | `build/utils/rimutil/Debug/rimutil.exe`, then `rimutil` on `PATH` | Which `rimutil` executable `gkpbr preview` calls. `pbr/gkpbr/preview.py:94` |
| `GEMINI_API_KEY` | `pbr/` | none; required by `classify` and by `maps --generate` | API key. `pbr/gkpbr/classify.py:336` |
| `GKLIGHTMAP_OUT` | `lightmap/` | `lightmap/out` | Where `gklightmap` writes its PNGs and `.dds`. `lightmap/gklightmap/cli.py:28` |
| `OPENROUTER_API_KEY` | `lightmap/` | also read from a file of that name at the repository root | OpenRouter API key, required by `gklightmap gen`. `lightmap/gklightmap/openrouter.py:30` |

## Related

- [settings.json](/reference/data/settings-json/): the persistent half of the same configuration.
- [Renderer setting keys](/reference/data/render-settings-keys/): the 79 persisted renderer knobs.
- [The profile directory](/reference/data/profile-directory/): what `GKPLUS_PROFILE` names.
- [One settings file, many owners](/explanation/one-settings-file-many-owners/): why a
  variable outranks the stored value in both directions, and is neither restored from the
  file nor written back to it.
- [How to set up a profile](/how-to/modding/set-up-a-profile/) and
  [How to turn on renderer features](/how-to/modding/turn-on-renderer-features/): the two
  guides that set most of these.
