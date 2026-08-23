---
title: "Command-line utilities"
description: "rimutil, rifutil, riflights, vfdiff and symdump: invocation, options, output and exit codes."
weight: 90
audience: ["mod-author", "developer"]
---

The standalone tools under `utils/`, for mod authors and developers. None of them loads into the
game.

Three are CMake targets in the main build and land in `build/utils/<tool>/<config>/<tool>.exe`.
`rifutil` and `riflights` are always built; `rimutil` is built only when the `rimutil` vcpkg
manifest feature is on, which `CMakePresets.json` enables.

## `rimutil`

`.RIM` ↔ PNG, both directions. Built from `utils/rimutil/rimutil.cpp` against spng and libsquish.

```
rimutil decompress <in.RIM> <out.png>
rimutil compress   <in.png> <out.RIM> [options]
```

| Option | Values | Default | Effect |
|---|---|---|---|
| `--format` | `dxt1`, `dxt3`, `body` | `dxt3` | Output encoding |
| `--raw` | flag | off | `body` only: skip ByteRun1 compression |

`--format dxt5` is refused by name with a non-zero exit: the engine's texture-format list holds
only DXT1 and DXT3, and the setter that receives the fourcc drops anything else silently, so a
DXT5 file renders with garbage alpha rather than failing.

`body` is exactly lossless on disk, at 2–6× the size. It is not lossless in the engine: Gunlok
ignores the `ALPH` chunk a palettized image carries alpha in, so `body` refuses an image with
graded alpha and warns on a cut-out that cannot use a transparent index.

Fewer than three arguments after the program name, an unknown option, an unknown format, or an
unrecognised mode prints the usage block and exits 1.

## `rifutil`

The `REBCRIF1` container codec, and the only compressor for the format in this repository. Built
from `utils/rifutil/rifutil.cpp` against `huffman/`.

```
rifutil compress   <input_file> <output_file>
rifutil decompress <input_file> <output_file>
```

| Exit code | Meaning |
|---|---|
| 0 | Success |
| 1 | Fewer than three arguments |
| 2 | Cannot open the input file |
| 3 | Cannot open the output file |
| 4 | Invalid mode |

## `riflights`

Reads the light rig out of `.rif` files, over `src/Rif.cpp`. Built from
`utils/riflights/riflights.cpp`.

```
riflights <file.rif> [more.rif ...]
```

Writes tab-separated records to stdout. One `file` line per input, then one `light` line per
light:

```
file	<path>	lights=<n>	name=<set name>	ambience=<n>[	(absent)]
light	<number>	<x>	<y>	<z>	<brightness>	<spread>	<range>	0x<colour>	<engine flags>	<local flags>	<orientation ...>
```

A file with no light set prints its `file` line and nothing else; that is the normal case for most
shipped files. A file that cannot be read, or that `rif::ReadLightSet` rejects, writes a diagnostic
to stderr and counts as a failure.

| Exit code | Meaning |
|---|---|
| 0 | Every input was read |
| 1 | At least one input failed |
| 2 | No input file was given |

## `vfdiff`

Differential test of `src/VertexFormat.cpp` against itself at a git revision. Not in
`utils/CMakeLists.txt`; `run.ps1` compiles it.

```powershell
.\utils\vfdiff\run.ps1
.\utils\vfdiff\run.ps1 -Ref HEAD~3
.\utils\vfdiff\run.ps1 -SelfTest
```

| Parameter | Default | Effect |
|---|---|---|
| `-Ref` | `HEAD` | The revision to compare the working tree against |
| `-SelfTest` | off | Perturb one field in one branch, rebuild, and require a non-zero exit |

Exit 0 means every case matched. The sweep covers every FVF the layout encoding can express, the
same set with unrelated bits set, four strides each, six vertex counts including 0, and the
rejection and null-argument paths. Comparison is `memcmp`, and a `0xAA` guard vertex past each
buffer must still match.

## `symdump`

`utils/symdump/gl_symbols.py` is a Ghidra Jython script that exports the database's function names
as a symbol map for the profiler.

```
analyzeHeadless <project dir> <project> -process gl.exe -noanalysis \
    -scriptPath utils/symdump -postScript gl_symbols.py <output path>
```

With no argument it writes `<program name>.sym` beside the executable Ghidra imported.

### Map format

One function per line, sorted by RVA. `#` begins a comment; several comments are headers.

```
# gkplus symbol map
# module gl.exe
# image_base 0x00400000
# file_size 2968064
# file_time 1655226954
# functions 12487
# format: <hex rva> <hex size> <name>
1000 2d FUN_00401000
16a310 3cc ParticleTester::Ctor
```

Addresses are RVAs, so the map does not depend on where the module loads. `# file_size` is
compared against the module actually loaded; a mismatch is reported as `stale: true` and the map
still loads. Unnamed functions are exported as `FUN_<address>`. External functions are not
exported.

### Where the profiler looks

`<gl.exe directory>\gkplus\symbols\<module>.sym`, found with no call the first time a name is
needed for that module, and a miss is remembered. `prof.symbol_dir` reports the directory;
`prof.symbols(module, path)` loads one from elsewhere. There is no generator for GkPlus's own
`d3d8.dll`; `llvm-symbolizer --obj=build/<config>/d3d8.dll --relative-address <rva>` resolves those
frames offline.

## Related

- [The rendertest harness](/reference/data/rendertest-harness/): the PowerShell half of `utils/`.
- [gkpbr](/reference/data/gkpbr-cli/): calls `rimutil`.
- `rif_chunk_format.md`: the `.rif` and `.RIM` formats.
- [How to replace a texture](/how-to/modding/replace-a-texture/) and
  [How to get true-colour textures into the game](/how-to/modding/true-colour-textures/): what
  `rimutil` is normally reached for.
- [Profile a frame](/how-to/development/profile-a-frame/): what `symdump` is for.
