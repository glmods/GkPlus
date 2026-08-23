---
title: "Add a binding to the gk module"
description: "Expose something to scripts through the \"gk\" QuickJS module and keep types/ in step with the C++ that backs it."
weight: 60
audience: ["developer"]
---

This guide shows a **developer** how to add a member, or a whole namespace, to the `"gk"` module,
and how to keep the declarations honest afterwards.

## 1. Decide which kind of binding it is

There are two, and the split is load-bearing:

- **Native**, wherever there is state to read back: `camera`, `game`, `world`, the `console` colours
  and registry. A wrapper over the game's own function or global.
- **Command-backed**, in `src/JsCommands.cpp`: format a console command line and run the game's own
  handler. Prefer this when a console command already does the job, because those handlers *are*
  argument parsers whose defaults come from the map bounds or the cursor, so dispatching one is
  faithful by construction where a reimplementation silently diverges. It also gets the command's
  broadcast for free, where a native binding would have to reproduce the wire message itself.

Check `console_command_notes.md` first. It classifies every registration against the JS surface,
and §4 explains the split. Two facts from it change what you write: a handful of command names are
registered under `GetResourceString` results and live in `glres<lang>.dll` rather than the exe, so
`console.execute("QUIT")` is a no-op on a non-English install; and **what makes a command hard to
bind is its broadcast, not its setter**.

If you are binding a native setter, check the setter and not the command beside it. Several
existing members do not replicate where their neighbours do, because the console command broadcasts
*around* the same setter, which is invisible in single player. `console_command_notes.md` §6 is
the table.

## 2. Write the table entry

Namespaces come one per translation unit (`src/JsCamera.cpp`, `src/JsWorld.cpp`, …), with the
command-backed clusters sharing `src/JsCommands.cpp` over helpers in `src/JsBindings.h` and
`src/JsCommon.cpp`. Add your entry to that file's `JSCFunctionListEntry` table.

Four QuickJS rules govern this layer, all verified against the pinned quickjs-ng source:

- **Never let a `JS_CGETSET_DEF` reach `JS_SetModuleExportList`.** That switch ends in
  `default: abort()`, so the process dies with no diagnostic, exception or log. This layer never calls
  it at all: every namespace is built object-first with `JS_SetPropertyFunctionList` (through
  `NewNamespace` in `src/JsBindings.h`) and handed to `JS_SetModuleExport` finished. Keep it that
  way.
- **A C module's named exports are values set once at instantiation, not live bindings.** State has
  to live on an exported *object*; a top-level export could only ever be a snapshot.
- **Own properties beat exotic handlers**, which is what lets `actors.count` coexist with
  `actors[12]`. Keep such own properties **non-enumerable** or they appear in `Object.keys`; and an
  exotic `get_own_property` must return `JS_PROP_ENUMERABLE`, because QuickJS ignores
  `JSPropertyEnum::is_enumerable` for exotic objects.
- **A bare specifier needs no module loader**, so nothing has to be registered for `import "gk"` to
  resolve.

For a new namespace, add a row to `Namespaces[]` in `src/JsGk.cpp`. To see the current list:

```bash
sed -n '/Namespaces\[\]/,/^};/p' src/JsGk.cpp | grep -oE '"\w+"\s*,' | wc -l
```

## 3. Follow the surface's own rules

These are contracts the whole surface is held to, not style preferences:

- **An actor is the object, everywhere.** No member takes an id or a token name.
- **Failure throws.** A boolean return means a question was answered: `actor.damage()` is "did it
  land" rather than "did it work".
- **A position is `{x, y, z}`**; an array is rejected.
- Naming: `snake_case` for methods, functions, data properties and accessors; `PascalCase` for
  classes and `JSClassDef::class_name` strings; `camelCase` for JS locals.

Reading an options object, use the readers in `src/JsBindings.h` and remember what their `bool`
means: **`GetInt32Prop` returns "no exception pending", not "the property was there."** It leaves
the out-param untouched for an absent property, which is what makes a pre-seeded default work.
Reading it as presence caused three separate defects in one session: every window resolving to
`{capture: 0}`, `prof.trigger = {min_ms: 30}` zeroing `pre` and `post`, and `prof.configure` setting
every numeric field to the same value. Seed the out-param with the value already in force, and check
the return only for an exception.

## 4. Declare it in `types/`

`types/gk.d.ts` is hand-maintained from `src/Js*.cpp` and is the only machine-readable description
of the surface. Add the member to the interface that declares its namespace. If you added a **new
table**, add its row to `PAIRS` in `types/check-surface.py`. Leaving one out makes "not checked"
the silent default, which is the failure the file exists to close.

`types/imgui.d.ts` is generated. After touching `imgui-quickjs/imgui-quickjs.cpp`:

```bash
python3 types/gen-imgui-dts.py     # check the `any` count it prints is still 0
```

## 5. Run both halves of the check

```bash
npx -y -p typescript tsc -p types/tsconfig.json
npx -y -p typescript tsc -p examples/jsconfig.json
python3 types/check-surface.py
```

`tsc` proves the declarations are self-consistent and that the shipped `.mjs` compile against them.
It cannot prove they match the bindings, because the bindings are C++. `check-surface.py` is that
half, comparing every table against its interface in both directions. Both failure directions have
happened: `units.remove_trigger` stayed declared after its C++ entry was deleted, and `render`
accumulated fifty undeclared members behind an index signature.

Then regenerate the browsable API reference, which renders the TSDoc comments you just edited:

```bash
npx -y -p typedoc@0.28 -p typescript@5.9 typedoc --options types/typedoc.json
```

Keep it at zero warnings: every warning it emits is a `{@link}` pointing at a member that no
longer exists, which is the one kind of drift `tsc` and `check-surface.py` both look straight past.
Both version pins are load-bearing; `types/README.md` says why.

If you edited a long hand-maintained list, grep it for duplicates. Nothing checks that any more,
since `JS_SetPropertyFunctionList` happily sets the same property twice:

```bash
grep -oE 'JS_(CFUNC_DEF\("[A-Za-z0-9_]+"|ENUM_DEF\([A-Za-z0-9_]+)' imgui-quickjs/imgui-quickjs.cpp \
  | sort | uniq -d
```

## 6. Exercise it

Two harnesses, and neither needs a full session:

- **A script module** (a panel, a level module) imports `"gk"` and nothing else, so Node plus a
  stub `gk` package and a recording ImGui object drives it in-process. That is what catches
  `Begin`/`End` balance, the query cadence and which writes a widget performs; an exception or a
  stray `TreePop` disables `draw_gui` for a whole session. `harness_testing_notes.md`, "Driving a
  script module under Node".
- **The binding itself** needs the game, because `GetBaseAddress()` derives from the host exe's
  entry point. Launch with `GKPLUS_REPL_PORT` and drive it over TCP; a binding that crashes the
  game names itself, because the snippet that stopped answering is the one that did it. Check the
  process afterwards rather than trusting the timeout.

The `src/Js*` layer *can* be built into a throwaway 32-bit harness with the `gk::` natives stubbed,
which is worth it for the marshalling and the collection scaffolding;
`harness_testing_notes.md` has the recipe and the four traps. Do not put `src/` on the include path:
`quickjs.h`'s `#include <math.h>` finds `src/Math.h` on a case-insensitive filesystem. Put the repo
root there and include as `"src/Menu.h"`.

## Related

- [Run the test suites](/how-to/development/run-the-test-suites/)
- `js_bindings_notes.md`: the collection scaffolding, the Actor prototype chain, and the members
  that do not replicate.

## Reference and background

- [JavaScript API](/reference/javascript/): the surface being extended, and the conventions
  the whole of it follows.
- [C++ API](/reference/cpp/): the native side, and the `gk::js` collection scaffolding.
- [Why the script host boots twice](/explanation/why-the-script-host-boots-twice/): why a
  namespace is an object rather than a set of module exports.
- [Why nothing here writes down a count](/explanation/why-nothing-here-writes-down-a-count/): why
  the namespace roster is derived from `src/JsGk.cpp` and never quoted.
