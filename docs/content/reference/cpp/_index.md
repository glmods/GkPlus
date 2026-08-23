---
title: "C++ API Reference"
description: "The generated reference for GkPlus's C++ surface: what clang-doc produces, the command that produces it, and where the output lands."
weight: 10
audience: ["developer"]
---

For **developers** building `d3d8.dll`. The C++ reference is generated from the source by
[clang-doc](https://clang.llvm.org/extra/clang-doc.html); the pages below describe the generator,
its inputs, and how the generated tree is organised.

## Pages in this section

- **[The generated tree](/api/cpp/)**: every namespace, record, function and enum
  clang-doc reached, with its comment. Served from `static/`, outside Hugo's content tree.
- **[Namespace map](/reference/cpp/namespaces/)**: which namespace lives in which header,
  and what each one covers. Read this first; the generated pages are indexed by symbol, not
  by subsystem.

## Where the output is

| | |
|---|---|
| Generated tree | `docs/static/api/cpp/` |
| HTML entry point | `docs/static/api/cpp/index.html` |
| Served at | `/api/cpp/` |
| JSON sidecar | `docs/static/api/cpp/json/`, the same model clang-doc renders the HTML
from. Written by the generator, then **removed from the published tree** by
`docs/tools/unify-api.py`: it is 4.4 MB, more than half the output, and nothing reads it at
view time. To keep a copy, take it between the two commands. |

Each generated page carries its own navigation; nothing is fetched from the JSON sidecar at view
time. The one external dependency is highlight.js, loaded from `cdnjs.cloudflare.com` by
clang-doc's template. Without network access the pages render, with code blocks unstyled.

The tree is a build artifact: regenerated in full by the command below, never hand-edited, and
not intended to be committed.

## Building it

clang-doc reads a Clang [compilation database](https://clang.llvm.org/docs/JSONCompilationDatabase.html).
The Ninja build already emits one: `CMakePresets.json` sets `CMAKE_EXPORT_COMPILE_COMMANDS` to `ON`
in the `vcpkg` preset, which `builtin-vcpkg` inherits, so `build/compile_commands.json` exists after
any configure. No CMake change is required.

From the repository root, with a configured `build/` directory:

```
clang-doc --executor=all-TUs build/compile_commands.json \
  --format=html \
  --output=docs/static/api/cpp \
  --project-name=GkPlus \
  --public \
  --ignore-map-errors \
  --extra-arg=-Xclang --extra-arg=-fparse-all-comments \
  --extra-arg=-ferror-limit=0
```

The backslashes are POSIX-shell line continuations; in PowerShell use a backtick, or put the
whole invocation on one line. `clang-doc` ships with LLVM and is not on `PATH` by default on
Windows; it sits beside `clang-cl` in the LLVM `bin` directory. The invocation above was run
against LLVM 22.1.0. It takes roughly half a minute over the 246 entries in the compilation
database and writes about 8 MB.

**Use LLVM 22.1.0 or newer.** An older clang-doc's HTML backend writes `index_json.js` instead
of `clang-doc-mustache.css`, and builds every sidebar link at runtime from that file's
`RootPath` - the absolute path this machine wrote the tree to. Published anywhere else, that
is a dead link on every page, and it will not show up as a broken `href` anywhere in the
markup: the link only exists as a JavaScript string until a browser runs it. There is no flag
to pick the backend; it is purely a function of the LLVM version. CI pins `LLVM_VERSION` in
`.github/workflows/docs.yml` for exactly this reason, and fails the build if `index_json.js`
reappears.

Each flag is load-bearing:

| Flag | Effect |
|------|--------|
| `--executor=all-TUs` | Process every entry in the compilation database, not a file list. Ninja Multi-Config emits one entry per configuration, so each source appears three times; entries merge by USR and the duplication costs only time. |
| `--format=html` | Emit the Mustache HTML generator's output. A `json/` tree is written alongside it in the same run. |
| `--output` | Root of the generated tree. clang-doc appends `html/` and `json/` itself. |
| `--public` | Document public declarations only. |
| `--ignore-map-errors` | Continue past translation units that failed to map. Required here; see *Diagnostics* below. |
| `--extra-arg=-Xclang --extra-arg=-fparse-all-comments` | Treat every comment as a doc comment. Without it only `///` and `/** */` are attached, and the codebase's convention is plain `//` above the declaration. `-Xclang` is needed because the compilation database drives `clang-cl`, which does not accept the flag directly. |
| `--extra-arg=-ferror-limit=0` | Do not stop parsing a translation unit at 20 errors. |

Nothing in the repository runs this automatically. There is no CI, and the command is not wired
into a CMake target. The only permitted CMake change for this work was the one that emits
`compile_commands.json`, and that was already in place.

## What is covered

Every declaration clang-doc reaches through the compilation database, which is `src/`,
`huffman/`, `imgui-quickjs/` and `utils/`. The DLL's own sources are the bulk of it.

Coverage of the symbols clang-doc can carry a description on, as of the last run:

| Kind | Documented | Total |
|------|-----------:|------:|
| Records (`struct`, `class`) | 200 | 229 |
| Free functions | 656 | 902 |
| Enums | 33 | 35 |

Re-derive these from the JSON sidecar rather than trusting the table: each `Functions[]`,
`Enums[]` and top-level `record` entry in `docs/static/api/cpp/json/**/*.json` carries a
`Description` key when it is documented and omits it when it is not. Run clang-doc and read
the sidecar **before** `unify-api.py`, which deletes it.

## What it structurally cannot carry

Four limits are properties of the generator or of the source layout, not of how much has been
written. They are listed here because the generated pages do not say so themselves.

**Struct fields carry no description.** clang-doc 22 emits no `Description` key for a
`PublicMembers` entry at all, for any of the 1,850 fields in the model. The per-field commentary in
the struct mirrors (the offset, the units, the fixed-point warning) exists in the headers and
reaches no generated page. Read the header for a field.

**One comment attaches to one declaration.** Where a header documents a group of declarations under
a single comment, that comment attaches to the first declaration in the group and the rest come
back undescribed. Where two declarations share a source line, as the paired getter and setter blocks
in `src/Misc.h` and `src/World.h` do, only the first can be described at all, since there is nowhere
to put a comment before the second.

**Getter halves of setter pairs are largely undescribed.** `src/VkDraw.h` documents each renderer
knob once, on the setter, in a paragraph that covers both. That paragraph attaches to the setter;
the getter is one of the 90 undescribed `gk::vulkan` functions in `src/VkDraw.cpp`.

**Nothing here is executable documentation.** `GetBaseAddress()` derives from the host
executable's entry point, so every native-API call faults outside Gunlok. There are no doctests
and no runnable examples in the generated reference.

## Diagnostics the run emits

The run prints roughly 320 copies of:

```
winnt.h(1064,5): error: MS-style inline assembly is not available:
Unable to find target for this triple (no targets are registered)
```

They come from three `__asm` blocks in the Windows SDK's `winnt.h`, reached under the 32-bit
`_M_IX86` branch, which this clang-doc binary cannot parse because it registers no code
generation targets. They are recoverable parse errors: each affected translation unit continues
and its declarations still reach the output. `--ignore-map-errors` is what keeps the run from
stopping on them.

Defining `_M_CEE_PURE` takes `winnt.h` down the macro branch that has no inline assembly, and is
**not** a workaround: it breaks the MSVC standard library headers instead, at
`vcruntime_new.h`.

## Comment conventions in `src/`

The generated pages are only as good as the comments behind them. The house style is documented
where it is enforced, in `CLAUDE.md`; two points matter to anyone regenerating this reference:

- Doc comments added for this reference are Doxygen-flavoured `///`, with `\param`, `\return` and
  `\warning`. The surrounding files use plain `//`, which `-fparse-all-comments` picks up
  identically; the `///` marks a comment written as an API description rather than as a note to
  the next reader of the code.
- `\param` and `\return` survive and render as *Parameters* and *Returns* sections. **`\warning`
  does not**: clang-doc 22 drops the block entirely, and the text never reaches the JSON model or
  the page. Write a caveat as an ordinary paragraph.
- Angle brackets in a comment are lexed as HTML tags. `<profile>` in `src/Profile.h` reaches the
  generated page as an empty tag followed by stray text, and backticks do not protect it, since the
  comment lexer does not know markdown. Avoid angle brackets, or spell the path without them.

## See also

- [Namespace map](/reference/cpp/namespaces/): which namespace lives in which header.
- [Mirror a game struct and prove its layout](/how-to/development/mirror-a-game-struct/): the
  conventions the struct mirrors follow, as a procedure.
- [How a hook reaches the game](/explanation/how-a-hook-reaches-the-game/): why every address is
  resolved on the call that needs it, which is why nothing in `src/` runs outside Gunlok and why
  none of this reference is executable.
- [Why nothing here writes down a count](/explanation/why-nothing-here-writes-down-a-count/): why
  the coverage figures above come with a re-derivation instead of being trusted.
- `address_map.md`: the binary's segment layout, every named global and function address, and the
  Actor class hierarchy. The struct mirrors point at it and it is not reproduced here.
- `actor_vtable_notes.md`: the vtable slot numbering the `Actor` hierarchy models.
