# Runtime-testing GkPlus outside the game

Nothing in `src/` runs in a standalone process without help, because `GetBaseAddress()`
derives from the host exe's entry point. This is how to build a throwaway harness anyway.

Two harnesses, and reach for the cheap one first: a **32-bit exe with stubbed natives** for
anything in `src/`, and, for a *script* module, **Node with a stub `gk`** — no compiler at all.

## Driving a script module under Node

An entry module, an ImGui panel and a level module all import `"gk"` and nothing else, so a bare
specifier plus a stub package is the whole harness:

```
harness/node_modules/gk/{package.json,index.mjs}   # {"name":"gk","type":"module","main":"index.mjs"}
harness/run.mjs                                    # imports the panel and drives it
```

The stub's job is to be *shaped* like `types/gk.d.ts` says the real one is — accessors that count
their reads, setters that record what was written — and the harness then asserts on that record.
This is what catches the things neither `tsc` nor a screenshot can: **an exception or an
unbalanced `Begin`/`End` disables `draw_gui` for the rest of the session**, and a panel that
queries per frame is a performance defect invisible in a screenshot. Give the stub ImGui a
`{open, changed}` mode pair — `open` decides whether sections expand, `changed` makes every widget
report an edit — and one pass with each covers the draw paths and the write paths separately.

Two traps, both of which produced a wrong answer first:

- **An ImGui id is its label within the current id scope, and `TreeNode`/`BeginChild` push one.**
  A flat "have I seen this label" check reports `frames` under *Recording* as colliding with
  `frames` in the window picker, which is two different widgets and not a bug. Track the scope
  path, and clear it per frame — a set that accumulates across frames reports every widget as a
  duplicate of itself on frame two.
- The stub cannot catch a **wrong property name** against the real binding, because the stub is
  whatever you wrote. `types/gk.d.ts` is what covers that, so the two are complements: run
  `npx -y -p typescript tsc -p examples/jsconfig.json` as well, and keep the `.d.ts` honest
  against `src/Js*.cpp`.

The usual rule applies here too, and it is cheap: **break something once and confirm the harness
says so.** Dropping one `TreePop` must fail the balance check, and a semantic assertion needs the
same treatment — verifying that "every trigger write carries `enabled`" actually fails when the
guard is removed is what proves the assertion tests the claim rather than restating it.

### Runtime-testing outside the game

Nothing in `src/` can be exercised outside Gunlok: `GetBaseAddress()` derives from the host exe's
entry point, so every native-API call faults in a standalone process. To runtime-test a layer that
does not itself touch game memory (the `src/Js*` bindings and the script host are the cases in
point), compile it into a throwaway 32-bit exe alongside a stub TU that replaces the `gk::` natives
with fakes:

```
clang-cl -m32 /EHsc /MD -clang:-std=c++23 -Wno-invalid-offsetof -Wno-deprecated-declarations \
  "/Ibuild/vcpkg_installed/x86-windows-static-md/include" "/I." "/Iimgui-quickjs" \
  main.cpp stubs.cpp src/JsCommon.cpp src/JsMenus.cpp src/CustomMenu.cpp src/Script.cpp \
  imgui-quickjs/imgui-quickjs.cpp /Feharness.exe /link \
  build/vcpkg_installed/x86-windows-static-md/lib/{qjs,imgui,detours}.lib
```

Do **not** put `src/` on the include path (see the include collision under Conventions) — put the
**repo root** there instead and include as `"src/Menu.h"`, which is what `-I .` above is for; the
`src/*.cpp` files still resolve their own siblings through quoted includes. The real project flags
live in `build/CMakeFiles/impl-Debug.ninja` (`FLAGS =`) if they drift. Always add a
deliberately-failing assertion once and confirm the harness reports it — a harness that cannot fail
proves nothing.

Two things make the script host reachable at all:

- The harness must **not construct the `*System` objects** — their ctors resolve offsets off a fake
  base and hand them to Detours. Both subsystems therefore expose their hook bodies as ordinary
  functions (`BootScriptHost`, `ReconcileCustomMenu`, `DispatchCustomMenuClick`) and the `*System`
  ctor is only the detour. Drive those directly.
- Supply the harness's own `gk::js::RegisterGkModule` and simply do not compile `JsGk.cpp`. The
  trimmed module registers just the namespace under test, which is what keeps the actor/role/token
  stubs (and the whole pure-virtual scrape below) out of a menus-and-host harness. Building nodes
  for a `List<T>` also needs `template <typename T> List_Member_Base<T>::~List_Member_Base() {}` —
  the pure virtual dtor is declared but never defined in the DLL, because nothing there ever
  constructs a node.

Four more things that cost time the first time round:

- **Run the compile from the PowerShell tool, not Bash.** MSYS rewrites every `/`-prefixed flag into
  a path (`/EHsc` becomes `C:/Program Files/Git/EHsc`), and clang-cl silently forwards the wreckage
  to the linker as input files — so the build *appears* to work until it links, and `/EHsc` `/MD`
  were never applied. Include the src files by **absolute** path from the harness directory; quoted
  includes still resolve each header's own siblings.
- **Stub actors need every pure virtual.** Generate them: scrape `virtual … = 0;` out of
  `src/Actors.h` into per-class `#define`s of `override` bodies, paste those into each stub class.
  Emit the parameter list verbatim — both `char *script` and a bare `float *` are legal in a
  definition, so there is nothing to rename. The generated slot counts are a free cross-check
  against `actor_vtable_notes.md` (82 + dtor / 12 / 5 / 5 / 3).
- **`Actor::~Actor()` is pure and undefined**, and `Inventory` is not defined in `src/` (its layout is in `inventory_notes.md`, but no header
  mirrors it yet). Both
  are fine in the DLL because nothing there instantiates or destroys an actor; a harness that does
  must supply `gk::Actor::~Actor() {}` and a harness-local `struct Inventory {}` (otherwise
  `pool_unique_ptr<Inventory>`'s deleter fails to instantiate on an incomplete type).
- Have the stub hierarchy **restate the class tree independently** of `src/ActorClasses.inc.h`
  rather than including it. If the two disagree the kind/`instanceof` tests fail, which is the
  point of testing the ladder at all.
