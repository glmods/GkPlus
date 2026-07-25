# TypeScript definitions for the GkPlus scripting API

Ambient declarations for everything a `main.mjs` can reach: the `"gk"` module,
the `"ImGui"` module, and the host's global `console`. They give an editor
autocomplete and type checking over plain `.mjs` files - there is no build step
and no TypeScript at runtime, since the game loads the `.mjs` as-is.

| File | What it covers | Maintained |
|------|----------------|------------|
| `gk.d.ts` | the `"gk"` module: `camera`, `console`, `actors`, `roles`, `tokens`, `triggers`, `menus`, plus the `setup_menus` / `draw_gui` contract and the global `console` | by hand, from `src/Js*.cpp` |
| `imgui.d.ts` | the `"ImGui"` module: 197 functions and 28 enums | **generated** - run `python3 types/gen-imgui-dts.py` |

## Using them

Copy `examples/jsconfig.json` and this folder next to your script:

```
<Gunlok>\gkplus\main.mjs
<Gunlok>\gkplus\jsconfig.json
<Gunlok>\gkplus\types\gk.d.ts
<Gunlok>\gkplus\types\imgui.d.ts
```

VS Code picks `jsconfig.json` up on its own. On the command line:

```bash
npx tsc -p jsconfig.json
```

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

## Keeping them honest

The bindings are the source of truth, not these files. After changing
`src/Js*.cpp`, update `gk.d.ts`; after changing `imgui-quickjs.cpp`, re-run the
generator. Both are checked by `types/typecheck.ts`, which uses
`@ts-expect-error` on every construct that *must* be rejected - so it fails both
when something legal stops compiling and when something illegal starts.
