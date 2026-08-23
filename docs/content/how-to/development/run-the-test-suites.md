---
title: "Run the test suites"
description: "Invoke every harness in the repo (the Blender suites, pbr, lightmap, the CLI tests, the JS surface checks and the vertex differ) and read the result correctly."
weight: 20
audience: ["developer"]
---

This guide shows a **developer** how to run the checks that exist, from the right directory, with
the right arguments.

Nothing is wired into CTest (`grep -rn 'enable_testing\|add_test'` over the CMake files finds
nothing) and there is no unified task runner. Every suite is a script you invoke by hand, and each
family has its own argument convention. Run only the ones your change touches; the whole set takes
well over half an hour.

## Know which suites can lie

- **`pbr/`'s suites are not pytest and must not be run under it.** Their `test_*` functions append
  to a module-level `FAILURES` list instead of asserting, so pytest collection reports the suite
  green whatever fails. Run each file as a script and read the exit code.
- **`lightmap/`'s and `blender/`'s do assert**, so they are safe under any runner.
- **Nothing in `src/` runs outside Gunlok**, since `GetBaseAddress()` derives from the host exe's
  entry
  point. The one exception with a real suite is `src/Rif`, which touches no game memory and is
  driven by `utils/riflights` (below).

## The Blender suites

These need no Blender at all, because the decoders import no `bpy`. Each takes the Gunlok
directory:

```bash
python blender/tests/test_roundtrip.py "<Gunlok dir>"   # the container, byte-exact
python blender/tests/test_schema.py "<Gunlok dir>"      # every leaf chunk
python blender/tests/test_shapes.py "<Gunlok dir>"      # REBSHAPE geometry
python blender/tests/test_heads.py "<Gunlok dir>"       # record chunks + keyframe timing
python blender/tests/test_cutscene.py "<Gunlok dir>"    # the cutscene codecs
python blender/tests/test_rim.py "<Gunlok dir>"         # every shipped texture, ~20 min
```

These walk the whole shipped asset set and **print nothing until they finish**: `test_shapes.py`
runs for over two minutes in silence, `test_rim.py` for about twenty. Do not interrupt one that
looks hung.

The rest take the scene through a real `.blend`, so they need `blender` on PATH:

```bash
blender --background --python blender/tests/test_scene.py -- "<Gunlok dir>" [N|all]
blender --background --python blender/tests/test_authoring.py -- ["<Gunlok dir>"]
blender --background --python blender/tests/test_cutscene_authoring.py -- ["<Gunlok dir>"]
blender --background --python blender/tests/test_emitter_authoring.py -- ["<Gunlok dir>"]
```

`test_scene.py` samples rather than running every file; pass `all` for the full set.

## The CLI tests

Both take the **built executable first**, then the Gunlok directory:

```bash
python utils/rimutil/tests/test_decode.py <rimutil.exe> "<Gunlok dir>"
python utils/riflights/tests/test_lights.py <riflights.exe> "<Gunlok dir>"
python utils/rimutil/tests/test_encode.py <rimutil.exe> ["<Gunlok dir>"]
```

`test_lights.py` is the check on **`src/Rif`**, the only test in the repo that exercises a `src/`
file, and it cross-checks against `blender/io_scene_rif`, which decodes the same format by a
different route. Run it after any edit to `src/Rif.cpp`.

`rimutil` is built only with the `rimutil` vcpkg manifest feature, which `CMakePresets.json`
enables by default.

## The Python projects

Run each from its own directory, as a script, and read the exit code:

```bash
cd pbr      && uv run python tests/test_pipeline.py        # and test_addon_boundary, test_cache,
                                                           # test_renderstate, test_preview
cd lightmap && uv run python tests/test_dds.py             # and test_openrouter, test_prompts,
                                                           # test_source
```

None takes arguments; the install comes from `GUNLOK_DIR` or the Steam registry. `lightmap`'s
suites stub the network, so none of them spends money.

Lint is per project, three separate invocations:

```bash
uv run --group dev ruff check .    # from blender/, from pbr/, and from lightmap/
```

## The JS surface checks

These are a pair and neither substitutes for the other. Run both after touching `src/Js*.cpp`:

```bash
npx -y -p typescript tsc -p types/tsconfig.json      # the declarations, and examples/
npx -y -p typescript tsc -p examples/jsconfig.json
python3 types/check-surface.py                       # ... and that they match src/Js*.cpp
```

`tsc` proves the declarations are self-consistent; it cannot prove they match the bindings, because
the bindings are C++. TypeScript is deliberately not a dependency of this repo, which is why a bare
`npx tsc` refuses to run. After touching `imgui-quickjs/imgui-quickjs.cpp`, regenerate and check the
`any` count it prints is still 0:

```bash
python3 types/gen-imgui-dts.py
```

## The vertex-format differ

`src/VertexFormat` is the one part of the renderer with no game dependency, so it can be
differential-tested against itself at any revision:

```powershell
.\utils\vfdiff\run.ps1                 # current tree vs HEAD
.\utils\vfdiff\run.ps1 -Ref HEAD~3
.\utils\vfdiff\run.ps1 -SelfTest       # must exit non-zero
```

It is not in `utils/CMakeLists.txt`; `run.ps1` compiles it. Run `-SelfTest` when you have changed
the harness itself: without the `namespace vulkan` → `refvulkan` rewrite both halves resolve to the
same symbol and every case passes trivially.

## When you add a harness

Break something once and confirm the harness reports it. That rule is applied throughout this repo
(`harness_testing_notes.md`, `utils/vfdiff/README.md`) because a harness that cannot fail proves
nothing. `pbr/tests/test_pipeline.py` carries a deliberate `FAIL by design` line for that reason.

## Related

- [Add a binding to the `gk` module](/how-to/development/add-a-gk-binding/): which of these to run,
  and in what order.
- [Mirror a game struct and prove its layout](/how-to/development/mirror-a-game-struct/): the
  `static_assert`s are the check there, and a build is how you run them.

## Reference and background

- [Command-line utilities](/reference/data/cli-utilities/): the exact invocation and exit
  codes of the executables three of these suites drive.
- [JavaScript API](/reference/javascript/): what `check-surface.py` and the two `tsc` runs
  are checking against.
- [Design records index](/reference/data/notes-index/): which notes file records what a
  given harness was written to catch.
- [Why nothing here writes down a count](/explanation/why-nothing-here-writes-down-a-count/): why
  a suite's file count is derived rather than quoted, here and everywhere else.
