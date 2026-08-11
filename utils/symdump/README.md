# Symbol maps for the profiler

The sampling half of the profiler (`src/Profiler.cpp`, `profiler_notes.md`) records raw
instruction pointers. Without a symbol map it can only say `gl.exe+0x001a3373`; with one it says
`Actor::Update`. **gl.exe ships no symbols — but its Ghidra database is heavily named**, and that
database is what this exports.

## Format

One function per line, sorted by RVA, `#` for comments:

```
# gkplus symbol map
# module gl.exe
# image_base 0x00400000
# file_size 2968064
# file_time 1655226954
# functions 12487
# format: <hex rva> <hex size> <name>
1000 2d FUN_00401000
15a360 24 MemMapBase::`scalar_deleting_destructor'
16a310 3cc ParticleTester::Ctor
```

**RVAs, not absolute addresses.** The profiler subtracts the module's runtime base before
looking anything up, so a map does not care what the image base was or where the module landed.

`# file_size` is checked. A map built against a different build of the binary would shift every
RVA and produce names that are confidently wrong rather than absent, which is worse than hex — so
`prof.symbols()` reports `stale: true` when it disagrees with the module actually loaded. It
still loads; being told is the point.

Unnamed functions are exported too, as `FUN_004a1b30`. That is deliberate: it is worth more than
a bare RVA, because it gives the enclosing function's *boundary* (so a hit reads
`FUN_004a1b30+0x12`) and the name pastes straight into Ghidra's Go To.

## Generating one

`gl_symbols.py` is a Ghidra Jython script. From the Script Manager, or headless:

```
analyzeHeadless <project dir> <project> -process gl.exe -noanalysis \
    -scriptPath utils/symdump -postScript gl_symbols.py <output path>
```

With no argument it writes `<program name>.sym` beside the executable Ghidra imported.

Last run against the gl.exe database: **12,487 functions, 7,758 named (62.1%), 977 of them
`::`-qualified, 363 KB.** 216 external functions are not exported — they are imports, and an
address in one belongs to the DLL that owns it, which gets its own map.

## Installing one

```
<Gunlok>\gkplus\symbols\gl.exe.sym
```

`prof.symbol_dir` reports that path. Nothing has to be loaded by hand: the first time `Describe`
needs a name for a module it looks for `<module>.sym` there, once, and remembers a miss. To load
one from somewhere else:

```js
prof.symbols("gl.exe", "D:\\maps\\gl.exe.sym")   // -> {entries, stale, note}
```

## GkPlus's own d3d8.dll

There is no generator for it and it does not need one — we have the PDB, and the profiler prints
`d3d8.dll+0xrva` which resolves offline:

```bash
llvm-symbolizer --obj=build/Debug/d3d8.dll --relative-address 0x146ec
```

RVAs move with every build, so record the *names* that comes back, never the offsets. If reading
our own frames in-game ever becomes the common case, the cheap route is a linker `/MAP` file
converted to this format rather than a second Ghidra database.
