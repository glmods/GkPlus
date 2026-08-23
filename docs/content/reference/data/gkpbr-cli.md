---
title: "gkpbr"
description: "The PBR map generator's subcommands, flags, environment and outputs."
weight: 70
audience: ["mod-author"]
---

The PBR map generator, for mod authors. A `uv` project in `pbr/`, run from that directory. Not
part of `d3d8.dll`.

```
uv run -m gkpbr.cli [--game DIR] <subcommand> [options]
```

`gkpbr/cli.py` also runs as `uv run python -m gkpbr.cli`. A subcommand is required.

## Global options

| Option | Default | Effect |
|---|---|---|
| `--game DIR` | `GUNLOK_DIR`, else the Steam registry | The Gunlok install directory |

## Environment

| Variable | Default | Effect |
|---|---|---|
| `GUNLOK_DIR` | the Steam registry | Install directory, when `--game` is absent |
| `GKPBR_OUT` | `pbr/out` | Where every stage writes |
| `GKPBR_RIMUTIL` | `build/utils/rimutil/Debug/rimutil.exe`, then `rimutil` on `PATH` | Which `rimutil` executable `preview` calls |
| `GEMINI_API_KEY` | none | Required by `classify`, and by `maps --generate` / `--delight` |

## `inventory`

```
gkpbr inventory [texture ...]
```

Finds the used textures and segments the atlases. Writes `<out>/manifest.json`,
`<out>/albedo/<slug>.png` and `<out>/labels/<slug>.png`. No API calls.

## `observed`

```
gkpbr observed [--from HARVEST ...] [--note TEXT] [--renderer NAME] [--top N]
```

Reports what the running game draws each sheet with, from the render-state profile at
`pbr/render_profile.json`, which is checked in and lives beside the package rather than in
`GKPBR_OUT`.

| Option | Type | Default | Effect |
|---|---|---|---|
| `--from HARVEST …` | one or more paths | none | Rebuild `render_profile.json` from `harvest-draws.ps1` dumps. Several sum |
| `--note TEXT` | string | none | What run produced it, recorded in the profile |
| `--renderer NAME` | string | none | The `GKPLUS_RENDERER` the harvest ran under |
| `--top N` | int | 40 | How many sheets to list |

## `probe`

```
gkpbr probe [texture ...] [--green-down]
```

Measures whether the image model holds registration. Writes `<out>/probe/`. Calls the API.

| Option | Effect |
|---|---|
| `--green-down` | DirectX normal convention |

## `classify`

```
gkpbr classify [texture ...] [--model M] [--force] [--refresh-stale] [--adopt-cached]
               [--level L] [--seen-only]
```

Stage 1: one material JSON per texture, cached at `<out>/materials/<slug>.json`, with a region
overlay at `<out>/regions/<slug>.png`. Calls the API for anything not cached.

| Option | Default | Effect |
|---|---|---|
| `--model M` | `classify.DEFAULT_MODEL` | Model slug |
| `--force` | off | Re-classify cached textures |
| `--refresh-stale` | off | Re-classify only the entries whose inputs have moved |
| `--adopt-cached` | off | Stamp today's fingerprint on entries that have none |
| `--level L` | all | Only the textures this level's assets can show |
| `--seen-only` | off | Skip sheets the profiled run never saw drawn |

## `maps`

```
gkpbr maps [texture ...] [--generate] [--delight] [--escalate] [--regenerate]
           [--model M] [--level L] [--seen-only] [--green-down]
```

Stages 2 to 4: the map set, at `<out>/maps/<slug>/<kind>.png` plus `<out>/maps/index.json`. The
kinds are `color`, `roughness`, `metallic`, `normal`, `emissive`, `height`. Model output is cached
under `<out>/generated/`.

| Option | Default | Effect |
|---|---|---|
| `--generate` | off | Use the image model for height |
| `--delight` | off | De-light the regions stage 1 says carry painted lighting |
| `--escalate` | off | Retry on the pro image model |
| `--regenerate` | off | Ignore cached model output and call the API again |
| `--model M` | `generate.DEFAULT_MODEL` | Model slug |
| `--level L` | all | Only the textures this level's assets can show |
| `--seen-only` | off | Skip sheets the profiled run never saw drawn |
| `--green-down` | off | DirectX normal convention. The default is OpenGL/Blender |

Without `--generate` and `--delight` this subcommand reaches no API.

## `preview`

```
gkpbr preview [texture] [--map NAME] [--format dxt1|dxt3|body] [--mod NAME]
              [--rimutil PATH] [--remove]
```

Packs one generated map as a `.RIM` into a mod under `<Gunlok>\gkplus\mods\<mod>\`, so the game
can display it.

| Option | Default | Effect |
|---|---|---|
| `texture` | required | A manifest key, e.g. `"ground/city ruins ground 1_a.rim"` |
| `--map NAME` | `normal` | One of `color`, `roughness`, `metallic`, `normal`, `emissive`, `height` |
| `--format` | per map kind: `body` for `normal`, `dxt1` for the rest | Override the `.RIM` encoding |
| `--mod NAME` | `gkpbr-preview` | Mod directory name |
| `--rimutil PATH` | see `GKPBR_RIMUTIL` | Path to `rimutil.exe` |
| `--remove` | flag | Delete the preview mod and exit |

The written mod is not enabled by writing it. Nothing scans for mods; a boot script has to name
the directory. See [Mod metadata](/reference/data/mod-metadata/).

## Related

- `pbr/README.md`: the design record.
- [rimutil](/reference/data/cli-utilities/): the `.RIM` encoder `preview` calls.
- [gklightmap](/reference/data/gklightmap-cli/): the other generator.
- [How to generate PBR maps for a texture](/how-to/modding/generate-pbr-maps/): the stage
  order as a procedure.
- [Why mods are named, never discovered](/explanation/why-mods-are-named-never-discovered/): why
  the preview mod this writes does nothing until something names it.
