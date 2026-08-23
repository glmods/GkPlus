---
title: "Design records index"
description: "Every notes file in the repository and the question it answers."
weight: 110
audience: ["developer"]
---

The repository's own design records, for developers. They live at the repository root and inside
the sub-projects, and they are the primary source for everything on this site. Each row names the
file and what is in it.

Cross-references between these files, and from source comments, cite a file plus a section number,
as in `vulkan_renderer_notes.md §4.92`. The numbering is load-bearing.

## GkPlus subsystems

| File | Answers |
|---|---|
| `script_host_notes.md` | The QuickJS host and the loopback REPL: both boot points, the facts that pin the design, the REPL protocol and its limits |
| `js_bindings_notes.md` | The `"gk"` module's namespaces and `types/`: the collection scaffolding, the Actor prototype chain, the native-versus-command-backed split, the members that do not replicate |
| `make_role_notes.md` | `src/MakeRole` and the `make` / `gls` namespaces: one `Make*` per GLS section, the conversions that carry unit risk, and what only the parser can answer |
| `script_queue_notes.md` | The `{kind, body}` envelope on both engine script queues, its hooks, and why the console queue is read through it but never written to it |
| `custom_levels_notes.md` | Levels with no `.gls` and no `.gcs`, and starting one with no menus |
| `mod_loading_notes.md` | The PhysicsFS VFS and the IAT patching that makes the engine consult it: the metadata contract, the inspection mount, the interning rule, the in-game verification |
| `profiler_notes.md` | The CPU profiler: instrumented zones plus a sampling thread over both game threads, why the frame is the unit, the two self-checks, the WOW64 stale-context trap, the first measurements |
| `harness_testing_notes.md` | Building a throwaway 32-bit harness for the script layers, and driving a script module under Node |
| `vulkan_renderer_plan.md` | The Vulkan renderer's status, next steps, how to test it, and what a residual can and cannot say |
| `vulkan_renderer_notes.md` | The Vulkan renderer's design record and every measurement behind it. The largest file here |

## The game, reverse engineered

| File | Answers |
|---|---|
| `address_map.md` | Segment layout, every named global and function address, the Actor class hierarchy and subclass sizes, `TriggerKind`, the `Role` and `Map` struct offsets |
| `actor_vtable_notes.md` | The Actor class hierarchy, all its vtable slots, subclass sizes and constructor addresses |
| `role_system_notes.md` | `Role` field by field: the entity hash table, lifecycle, the flags bitfield, the AI-to-Actor-subclass dispatch, the spawn path, the vulnerability subsystem |
| `role_subobjects_notes.md` | `Character`, `Projectile`, `ParticleGenerator` and the three-variant `Destructibility` family |
| `trigger_system_notes.md` | The trigger types, their data structures, console-command syntax and function addresses |
| `gls_system_notes.md` | The GLS/GSH parser: pipeline, `ParsedThingBase` layout, per-section field tables with types, ranges and defaults, the `ToXxx` converters, and parser input sources |
| `gls.txt` | A quick GLS field list. Superseded by `gls_system_notes.md` |
| `level_loading_notes.md` | How a level is built, the sidecar caches, the geometry format, the placed-object binding, both spawn factories, and the navmesh |
| `menu_system_notes.md` | Both menu systems, the item constructors and item types, the menu inventory with titles and populators, the transition map, navigation, rendering, input and the string table |
| `console_command_notes.md` | Every console command the game registers, how the registry and its longest-prefix dispatch work, and each command classified against the JS surface |
| `save_system_notes.md` | The `.sav` / `.msv` format field by field, the carry-to-next-level variant, and the team carry-over roster |
| `input_notes.md` | The input subsystem: the Win32 keyboard path, Raw Input for the mouse, and the vestigial DirectInput device |
| `threading_model_notes.md` | The two game threads, the loopback message queues, the pause handshake, which GkPlus hooks run on which thread, and the script-execution entry points |
| `directplay_protocol_notes.md` | The multiplayer wire protocol: session setup, framing and reliability, and the full command and update message tables |
| `rendering_notes.md` | The AWAPI renderer: the submit-then-drain frame, the class hierarchy, ranked hook points, and the catalogue of producers |
| `rif_chunk_format.md` | The `.rif` asset format: the chunk header, the container, every registered chunk type, the `.RIM` texture format, the cutscene chunks, and the AvP upstream mapping |
| `file_io_notes.md` | Every way the game opens a file: the properties that make a VFS possible, the `GLDir` scheme, the read and write site tables, the memory-source seams, the IAT slot map, and the image-codec registry |
| `game_defects_notes.md` | Bugs in Gunlok itself that reproduce without GkPlus, plus the debugging recipes: where `cdb` lives, the WER dump path, and the symbolizer invocation |
| `ghidra_analysis_notes.md` | The reverse-engineering traps and the Ghidra MCP mechanics. Read before concluding anything negative about the binary |

## The gameplay layer

| File | Answers |
|---|---|
| `ai_behaviour_notes.md` | Enemy perception and the alert state machine, alert propagation, and the reacquiring state |
| `combat_notes.md` | The damage arithmetic end to end, the ballistic solve, aim as a spread, armour as a flat absorb threshold, splash, and the ammo compatibility test |
| `orders_notes.md` | The pending-order FIFO and its kinds, standing orders, selection, Active Pause, and the key-binding table |
| `navigation_notes.md` | The A\* over the level's own triangles, the node budget and partial paths, and per-agent walkability |
| `inventory_notes.md` | The inventory container and its items, the slot model, the deny bitmask, and how module effects are re-derived |
| `stealth_and_fog_notes.md` | Crouch and concealment, camouflage, concealment as a skip in acquisition loops, and the client-only fog grid |
| `gadgets_notes.md` | Mines, the decoy, scrap-pile scavenging, and laser fences |

## Sub-project documentation

| File | Answers |
|---|---|
| `CLAUDE.md` | The project overview, build commands, subsystem roster, conventions and test invocations. Addressed to an agent |
| `blender/CLAUDE.md` | The Blender addon's design record |
| `blender/README.md` | Building and installing the addon, authoring a new file, how the file maps onto the scene, testing, and the known limitations |
| `pbr/README.md` | The PBR generator: the problem, the pipeline, baked lighting, the normal-map convention, cost, and what the probe measured |
| `lightmap/README.md` | The lighting-map generator: scope, the three channels, why the DDS is uncompressed, what was measured, and the boundary with the addon |
| `types/README.md` | Using the `.d.ts` files, what they encode that is easy to get wrong, and how they are kept honest |
| `utils/rendertest/README.md` | Driving the game through the REPL, and the list of things that waste a run |
| `utils/symdump/README.md` | The symbol-map format, generating one, and installing one |
| `utils/vfdiff/README.md` | The vertex-conversion differential test and its self-test rule |

## Upstream source

Gunlok's asset layer derives from Rebellion's `3dc` chunk library, and the published Aliens versus
Predator Gold source is the ground truth for anything chunk- or RIF-shaped.
`rif_chunk_format.md` carries the chunk-id to class and file map. The game layer (roles, actors,
GLS, triggers, menus, saves) has no counterpart there.

## Related

- [Reading a binary that cannot answer back](/explanation/reading-a-binary-that-cannot-answer-back/): how a finding travels from the
  decompiler into these files, and why the negative claims
  in them are the ones to distrust.
- [Why nothing here writes down a count](/explanation/why-nothing-here-writes-down-a-count/): why
  the rosters in these files come with the command that re-derives them.
- [Reverse-engineer a function in Ghidra](/how-to/development/reverse-engineer-in-ghidra/): the
  workflow that adds to them.
