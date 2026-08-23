---
title: "gklightmap"
description: "The lighting-map generator's subcommands, flags, environment, channel layout and output paths."
weight: 80
audience: ["mod-author"]
---

The lighting-map generator, for mod authors. A `uv` project in `lightmap/`, run from that
directory. Not part of `d3d8.dll`.

```
uv run python -m gklightmap.cli <subcommand> [options]
```

A subcommand is required.

## The artefact

One `<stem> lighting.dds` per texture, uncompressed 24-bit with a full mip chain. The interface to
the renderer is the file name: `src/VkLighting.cpp` probes four candidates for a texture, in this
order, and nothing registers a map.

```
graphics/<stem> lighting.dds
<stem> lighting.dds
graphics/<stem>_lighting.dds
<stem>_lighting.dds
```

### Channels

| Channel | Meaning |
|---|---|
| R | Height field. The normal is derived from it at draw time |
| G | Highlight intensity, not a metal switch |
| B | Highlight sharpness |

The map names the CLI uses for these are `bump`, `metallic` and `roughness`, in that order
(`prompts.ORDER`).

## Environment

| Variable | Default | Effect |
|---|---|---|
| `OPENROUTER_API_KEY` | also read from a file of that name at the repository root | API key. Required by `gen` |
| `GKLIGHTMAP_OUT` | `lightmap/out` | Output root |
| `GUNLOK_DIR` | the Steam registry | Install directory, used by `install` and by `--install` |

Without a key, `gen` exits with `no OpenRouter key: set OPENROUTER_API_KEY, or put one in <path>`.

## Output layout

Per texture, under `<out>/<slug>/`:

| File | Written by |
|---|---|
| `albedo.png` | `albedo`, and by `gen` |
| `bump.png`, `metallic.png`, `roughness.png` | `gen` |
| `<stem> lighting.dds` | `gen`, `pack` |

## The texture argument

A `.RIM` path, or a `BMPNAMES`-style name such as `ground/cracks.rim`.

## `albedo`

```
gklightmap albedo <texture>
```

Decodes the `.RIM` to PNG and stops. No API calls.

## `gen`

```
gklightmap gen <texture> [--model M] [--map NAME]... [--size S] [--samples N]
                         [--timeout SEC] [--install MOD] [--no-mips]
```

Generates the three maps and packs the `.dds`. Calls the API once per map, per sample.

| Option | Type | Default | Effect |
|---|---|---|---|
| `--model M` | string | `openrouter.DEFAULT_MODEL` | OpenRouter model slug |
| `--map NAME` | `bump`, `metallic`, `roughness`; repeatable | all three | Only (re)generate this map. The others are reused from disk |
| `--size S` | tier (`1K`) or pixels (`1024x1024`) | the source's own size | What to ask the endpoint for. The reply is resized back either way |
| `--samples N` | int | 1 | Ask N times per map and keep the per-pixel median. 1 is one call and is bit-identical to omitting the option |
| `--timeout SEC` | int | 300 | Seconds per request |
| `--install MOD` | mod name | none | Also copy the result into `<Gunlok>\gkplus\mods\MOD` |
| `--no-mips` | flag | off | Write the base level only |

Nothing is cached against a fingerprint: re-running `gen` re-asks and re-spends.

## `pack`

```
gklightmap pack <texture> [--no-mips]
```

Rebuilds the `.dds` from the PNGs on disk. No API calls.

## `install`

```
gklightmap install <texture> [--mod NAME] [--remove]
```

Copies a built `.dds` into a mod, or removes the mod.

| Option | Default | Effect |
|---|---|---|
| `--mod NAME` | `gklightmap-preview` | Mod directory name |
| `--remove` | flag | Delete the whole mod directory |

The target path is `<Gunlok>\gkplus\mods\<mod>\Graphics\<stem> lighting.dds`, mirroring the game's
`Graphics` tree. A metadata directory is written alongside.

`install` without `--remove` requires a texture argument; with `--remove` it does not.

Writing the mod does not enable it. Nothing scans for mods; a script has to name the directory.
See [Mod metadata](/reference/data/mod-metadata/).

## Related

- `lightmap/README.md`: the design record.
- [Renderer setting keys](/reference/data/render-settings-keys/): the `lighting_map.*` knobs that
  shape the response.
- [gkpbr](/reference/data/gkpbr-cli/): the other generator.
- [How to generate a lighting map for a texture](/how-to/modding/generate-a-lighting-map/): the
  four stages as a procedure.
- [Why the renderer seam is the device](/explanation/why-the-renderer-seam-is-the-device/): what
  reads the file this produces.
