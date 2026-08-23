---
title: "How to generate a lighting map for a texture"
description: "Turn one .RIM into the companion lighting.dds the Vulkan renderer looks for, and install it as a mod."
weight: 90
audience: ["mod-author"]
---

This guide shows a **mod author** how to produce a `<texture> lighting.dds` - the bump and
highlight map GkPlus's Vulkan renderer picks up by file name - using `lightmap/` (`gklightmap`).

Run everything from the `lightmap/` directory:

```
cd lightmap
```

`OPENROUTER_API_KEY` is read from the environment, or from a file of that name at the repository
root. `GUNLOK_DIR` overrides install detection and `GKLIGHTMAP_OUT` the output directory. Only
`gen` reaches a model - about $0.20 per texture, measured, because it is three images.

## Look at the albedo first

```
uv run python -m gklightmap.cli albedo "ground/gunlok rust.rim"
```

Decodes the `.RIM` to PNG and stops. No API call.

## Generate the three channels

```
uv run python -m gklightmap.cli gen "ground/gunlok rust.rim"
```

Three prompts, three greyscale replies, one packed DDS. What each channel means to the renderer
is **not** what the same name means in a PBR set, and a wrong channel fails silently:

| channel | what the renderer does with it |
|---|---|
| R (bump) | a **height field** - the normal is derived from its gradient at draw time, so an RGB normal map here lights as noise |
| G (metallic) | the **intensity** of the highlight, straight through; not a metal/dielectric switch |
| B (roughness) | the **sharpness** of the highlight, as a specular exponent |

Alpha is unread. `gklightmap/prompts.py` is the whole prompt set and is meant to be edited.

Variations:

- `--map bump` (repeatable) re-asks for one channel and reuses the others from disk.
- `--model`, `--size`, `--samples` and `--timeout` control the request.
- `--install <mod>` installs in the same run.

Nothing is cached against a fingerprint: re-running `gen` re-asks and re-spends. If one channel
is nearly right, edit its PNG by hand and repack instead:

```
uv run python -m gklightmap.cli pack "ground/gunlok rust.rim"
```

A safety refusal on an innocuous texture is worth one plain re-run before investigating: the
providers' moderation is not deterministic, and a batch of rock and gravel textures produced a
handful of HTTP 400s that all passed on a second attempt.

## Install it

```
uv run python -m gklightmap.cli install "ground/gunlok rust.rim" --mod preview
```

Writes `<Gunlok>\gkplus\mods\<mod>\Graphics\<stem> lighting.dds`, which is the first path the
renderer probes. Dropping the file next to the `.RIM` in the install works too.

Like any mod it does nothing until a boot script names it:

```js
mods.enable(mods.load("mods/preview"));
```

Then turn the feature on. It needs the Vulkan renderer:

```
set GKPLUS_RENDERER=vulkan
```

```js
render.lighting_map.enabled = true;
```

See [How to turn on renderer features](/how-to/modding/turn-on-renderer-features/) for the rest
of the family - `bump_scale`, `specular_scale`, `gloss_min`/`gloss_max` and the chrome knobs.

Remove it when you are done:

```
uv run python -m gklightmap.cli install --remove --mod preview
```

**That is not a nicety.** A leftover mod goes on serving in every later session and the game
looks fine.

## Check the engine found it

A texture with no companion map is the normal case and is therefore silent, so "nothing changed"
and "it was never found" look identical from the screen. Ask:

```js
render.debug.lighting_map_report      // every name probed, and what came of it
```

To pick up a map you edited while the game is running, set `render.lighting_map.enabled` false
and then true - that destroys every image and re-reads every file, and nothing else does.

Reading either of those means talking to the running game: see
[How to drive the game from the REPL](/how-to/modding/drive-the-game-from-the-repl/).

## Keep the mip chain

`gklightmap` writes uncompressed 24-bit with a full chain down to 1x1, and both halves are
deliberate: two of the three channels are masks that a DXT block's shared endpoints would smear,
and a mip-less map with texel-scale content produces full-amplitude speckle at minification.
Cost is about 4.2 MB for a 1024-square map against DXT1's 0.7.

## Next

- `lightmap/README.md` records what was measured, which models were compared, and the tool's
  limits - one answer per channel for a whole atlas among them.

## Reference and background

- [gklightmap](/reference/data/gklightmap-cli/): every subcommand and flag, the channel
  layout, the API key discovery order, and the output paths.
- [Renderer setting keys](/reference/data/render-settings-keys/): the lighting-map keys the
  renderer reads.
- [Mod metadata](/reference/data/mod-metadata/): packaging the result so the VFS serves it.
- [Why the renderer seam is the device](/explanation/why-the-renderer-seam-is-the-device/): why
  none of this does anything under the stock renderer.
