# TypeScript definitions for the GkPlus scripting API

Ambient declarations for everything a `main.mjs` can reach: the `"gk"` module and
the objects the host hands to `setup_menus` and `draw_gui`. They give an editor
autocomplete and type checking over plain `.mjs` files - there is no build step
and no TypeScript at runtime, since the game loads the `.mjs` as-is.

| File | What it covers | Maintained |
|------|----------------|------------|
| `gk.d.ts` | the `"gk"` module: `camera`, `console`, `actors`, `roles`, `tokens`, `triggers`, `levels`, `make`, `gls`, plus the `setup_menus` / `draw_gui` / level-module contracts. `menus` is *not* an export - it is `setup_menus`' argument - and there is no global `console`: `log`/`info`/`warn`/`error`/`debug` are on the `console` this module exports | by hand, from `src/Js*.cpp` |
| `check-surface.py` | not a declaration file: it compares every `JSCFunctionListEntry` table in `src/Js*.cpp` against the interface that declares it, both directions. `tsc` cannot do this, because the bindings are C++ | a check - run `python3 types/check-surface.py` |
| `imgui.d.ts` | the `ImGui` interface: 199 functions and 28 enums. A type only - there is no `"ImGui"` module and no global to call, because the calls are only valid inside `draw_gui` | **generated** - run `python3 types/gen-imgui-dts.py` |
| `typedoc.json` | not a declaration file: the TypeDoc configuration that turns the two above into the browsable API reference under `docs/static/api/js/`. The TSDoc comments in `gk.d.ts` are what it renders | a config - see below |
| `package.json` / `index.d.ts` | packages this folder as the npm module `@glmods/gkplus-types` (unpublished - `private: true`, `license: UNLICENSED`). `index.d.ts` has no content of its own, only `/// <reference>`s to `gk.d.ts` and `imgui.d.ts`, since neither is importable - one is an ambient module, the other a global | by hand, mechanical |

## Using them

There are two ways to get these into a mod's own type checking; both end up
checking the same two files.

### Copied next to the script

Copy `examples/jsconfig.json` and this folder next to your script:

```
<Gunlok>\gkplus\main.mjs
<Gunlok>\gkplus\jsconfig.json
<Gunlok>\gkplus\types\gk.d.ts
<Gunlok>\gkplus\types\imgui.d.ts
```

VS Code picks `jsconfig.json` up on its own. On the command line:

```bash
npx -y -p typescript tsc -p jsconfig.json
```

### As an npm package

For a mod that already has a `package.json` (an npm-based build, a monorepo of
several mods), `types/` doubles as the source of an installable package
without a registry: `npm pack` in this directory produces
`glmods-gkplus-types-<version>.tgz`, which `npm install <path-to-tgz>` (or a
`git+https://...#path:types` dependency, or a plain relative `file:` dependency
pointing at this folder) installs like any other package. It is not published
to the public registry - the license is intentionally `UNLICENSED` until that
decision is made, so `npm publish` refuses to run.

Once installed, point the consuming `tsconfig.json`/`jsconfig.json` at it by
name rather than by path:

```json
{ "compilerOptions": { "types": ["@glmods/gkplus-types"] } }
```

which is what makes `import { actors } from "gk"` and the global `ImGui` type
resolve - `@glmods/gkplus-types`'s `index.d.ts` is the thing being referenced,
and it in turn references `gk.d.ts` and `imgui.d.ts`.

Annotate the two entry points so the checker knows what it is looking at -
`examples/main.mjs` shows the whole pattern:

```js
/** @type {import("gk").SetupMenus} */
export function setup_menus(menus) { … }

/** @type {import("gk").DrawGui} */
export function draw_gui(ImGui) { … }
```

## Things the types encode that are easy to get wrong

- **`actor.kind` is a discriminated union.** `if (a.kind === "turret")` narrows to
  `TurretActor`, which is how you reach `turret_enabled`, `goto`, `attack_target`
  and the rest of the subclass-only members. Write `Actor` (the union) in your own
  signatures. `instanceof actors.classes.MobileActor` narrows too, to
  `AnyMobileActor`.
- **A collection lookup can miss.** `actors[12]` is `Actor | undefined`, and every
  wrapper can go stale - check `.valid` on anything held across frames.
- **`tokens` is the only writable collection**, and writing is an upsert.
  `actors[1] = x` is a type error, and throws at runtime.
- **ImGui widgets return their new state** rather than writing through a pointer:
  `const r = ImGui.SliderFloat(…); if (r.changed) x = r.value;`. Optional
  trailing arguments are an options object, not positional.
- **`menu.item.value` is `boolean | undefined`** - undefined for a plain item,
  which makes `value !== undefined` the "is this a toggle" test.
- **An actor is always the object.** No member takes an id or a token name, in
  either half of the surface. `actor.set_target(other)`, `camera.track(a)`,
  `units.set_ai(a, "turret")`, `triggers.create({targets: [a]})`.
- **Failure throws.** A boolean return means a question was answered -
  `actor.damage()` is "did it land", `settings.remove()` is "was there a key" -
  not "did it work".
- **A position is `{x, y, z}` and an array is not one.** `[1, 2, 3]` is rejected;
  it used to be accepted and silently do nothing.
- **`render` is exactly typed and `render.debug` is not.** The measurement
  surface carries the index signature, so a typo in a *setting* is a compile
  error. Every `render` setting persists; nothing on `render.debug` does.

## Generating the browsable reference

The TSDoc comments in `gk.d.ts` are also the source of the generated HTML reference:

```bash
npx -y -p typedoc@0.28 -p typescript@5.9 typedoc --options types/typedoc.json
```

Output lands in `docs/static/api/js/`, which is served at `/api/js/` on the docs
site. Both pins are load-bearing: TypeDoc is not a dependency of this repo (same
reason as `tsc`), and `-p typescript@5.9` is needed because a bare `-p typescript`
resolves to a major TypeDoc does not accept as a peer.

It runs with **zero warnings**, and it is worth keeping that way - every warning it
emits is a `{@link}` pointing at a member that no longer exists, which is the
declaration drifting from the bindings in the one way `tsc` and `check-surface.py`
both look straight past.

## Keeping them honest

The bindings are the source of truth, not these files. After changing
`src/Js*.cpp`, update `gk.d.ts`; after changing `imgui-quickjs.cpp`, re-run the
generator. Both are checked by `types/typecheck.ts`, which uses
`@ts-expect-error` on every construct that *must* be rejected - so it fails both
when something legal stops compiling and when something illegal starts.

`typecheck.ts` proves the declarations are self-consistent. It cannot prove they
match the bindings, which is a separate failure and a real one: `render` once
carried 50 undeclared members behind an index signature, and a deleted
`units.remove_trigger` stayed declared after its C++ entry was gone. That is what
`check-surface.py` is for - run it too:

```bash
python3 types/check-surface.py
```

Adding a namespace means adding a row to its `PAIRS`. Leaving one out is never
right: "not checked" would then be the silent default, which is the failure mode
the whole file exists to close.
