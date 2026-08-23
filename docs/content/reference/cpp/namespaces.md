---
title: "Namespace Map"
description: "Every C++ namespace in GkPlus, the headers that declare it, and what it covers."
weight: 20
audience: ["developer"]
---

For **developers**. Everything in GkPlus lives in `gk`. This page maps each namespace onto the
headers that declare it, so the generated tree under `/api/cpp/gk/` can be navigated from a
header name and the reverse.

The namespace list is derivable rather than transcribed:

```
grep -rhoE '^namespace [a-z0-9_:]+' src/*.h src/*.cpp | sort -u
```

and the generated tree mirrors it:

```
ls docs/static/api/cpp/gk
```

Directories in that listing that are not in the `grep` output (`List`, `HashTableBase`,
`ReadStats`) are class scopes rather than namespaces.

## `gk`

The root namespace, and where most of the surface is. One header per subsystem: decompiled
structs and enums with `static_assert`ed layout, plus free-function declarations over the game's
own functions and globals, implemented in the matching `.cpp`.

| Header | Covers |
|--------|--------|
| `Core.h` | `GetBaseAddress()`, `GetObjectAtOffset()`, `DebugWrite()`, the calling-convention aliases |
| `Memory.h` | `pool_alloc`/`pool_free` and the `pool_unique_ptr`/`pool_string` ownership markers |
| `List.h` | `List<T>` / `List_Member<T>` / `List_Member_Base<T>` |
| `HashTable.h` | `HashTableBase<T>` / `HashTable<T>` |
| `Field.h` | `union field`, a shared 4-byte value slot |
| `DetourUtils.h` | member-function `DetourAttach`/`DetourDetach` wrappers |
| `Encoding.h` | `Utf8FromGameText` / `GameTextFromUtf8` |
| `Varint.h` | variable-length integer encode/decode |
| `Actors.h` | the `Actor` hierarchy and its vtables |
| `Roles.h` | `Role`, `Character`, `Light`, `Projectile`, the destructibility family, the GLS enum tables |
| `Vulnerability.h` | `Vulnerability` and `VulnerabilityType` (header-only) |
| `Map.h` | `Map`, the level mesh headers, the team slots |
| `MapLights.h` | `MapLightSystem` and the level's light rig |
| `Triggers.h` | `TriggerKind`, `TriggerData`, the trigger lists |
| `Camera.h` | `CameraData` and the camera globals |
| `Math.h` | `Vec3`, `Vec4` |
| `World.h` | sun angle and brightness, ambient light, the fog state |
| `Misc.h` | `GLKeysSettings`, `Cheats`, the game-state globals, the executor pause |
| `Menu.h`, `Menus.inc.h` | `Menu`, `MenuListItem`, and the front-end menu roster (`grep -c '^GUNLOK_MENU' src/Menus.inc.h`) |
| `CustomMenu.h` | GkPlus-owned menu items and claimed pages |
| `RenderMenu.h` | the Advanced Graphics page |
| `Console.h` | `CommandData`, `CommandListElem`, the command registry |
| `Tokens.h` | the token table |
| `Font.h` | `GetFont`, `LineHeight`, `QueueText`, `VersionTextSystem` |
| `Music.h` | `MusicTrack` and `MusicSystem` |
| `GLS.h` | the GLS parser: `ParsedThing`, the field tables, `ParseSource` |
| `MakeRole.h` | the `ToXxx` converters over plain description structs |
| `CustomLevel.h`, `Session.h` | levels with no `.gls`, and starting one with no menus |
| `ScriptQueue.h` | the `{kind, body}` envelope on both engine script queues |
| `Script.h`, `Repl.h` | the QuickJS host and the loopback REPL |
| `GUI.h` | the overlay and the three callback seams |
| `FileHooks.h` | the IAT patches, the read-ahead layer, `EnsureFirstOpen` |
| `Render.h` | the AWAPI renderer's struct mirror |
| `InputFix.h`, `HudFix.h`, `WindowPlacement.h`, `Debug.h` | the hook-only behavioural fixes |
| `ActorArg.h` | actor arguments for command-backed bindings |

Some headers declare nothing in the root namespace at all: `Dds.h`, `ImageCodec.h`, `Json.h`,
`Profile.h`, `Profiler.h`, `Rif.h`, `Settings.h` and `Vfs.h` are entirely inside a sub-namespace,
and `GLS.h` and `RenderSettings.h` are split across one. `grep -n '^namespace' src/*.h` is the
current answer.

## Sub-namespaces

| Namespace | Headers | Covers |
|-----------|---------|--------|
| `gk::d3d8` | `D3D8Capture.h`, `D3D8CaptureInternal.h` | the `Direct3DCreate8` seam, the device wrapper, the captured draw state |
| `gk::dds` | `Dds.h` | DDS parsing. Touches no game memory |
| `gk::gls` | `GLS.h` | the GLS parser's API: schema, probe, parse-from-source |
| `gk::image` | `ImageCodec.h` | the engine's image-codec interface and the DDS registration |
| `gk::js` | `Js.h`, `JsBindings.h` | the QuickJS binding layer's shared machinery |
| `gk::json` | `Json.h` | the `{kind, body}` envelope's codec and `json::Document` |
| `gk::loadscreen` | `LoadScreen.h` | the loading-bar present throttle |
| `gk::prof` | `Profiler.h` | the CPU profiler. Touches no game memory |
| `gk::profile` | `Profile.h` | where a launch is configured from. Touches no game memory and reads no file |
| `gk::render_settings` | `RenderSettings.h` | the renderer knobs as persistent settings. Touches no game memory |
| `gk::rif` | `Rif.h` | the `REBCRIF1` container and the light-rig records. Pure |
| `gk::settings` | `Settings.h` | `<profile>/settings.json`. Touches no game memory |
| `gk::vfs` | `Vfs.h` (declared again in `Js.h`) | mount, index, lookup, read, materialize. Touches no game memory |
| `gk::vulkan` | `VkContext.h`, `VkResources.h`, `VkDraw.h`, `VkRenderer.h`, `VkLighting.h`, `VkCapture.h`, `VkInternal.h`, `VertexFormat.h`, and the shared name tables in `RenderSettings.h` | the Vulkan renderer |

Two headers name themselves internal and are documented only incidentally:
`D3D8CaptureInternal.h` and `VkInternal.h`.

## Headers outside the compilation database's `src/` set

`huffman/`, `imgui-quickjs/` and `utils/` are compiled by the same build and therefore appear in
the generated tree, under `GlobalNamespace` where they declare nothing in `gk`. They are not part
of the DLL's own API surface.

## See also

- [C++ API reference](/reference/cpp/): the generator, the build command, and what the generated
  tree cannot carry.
