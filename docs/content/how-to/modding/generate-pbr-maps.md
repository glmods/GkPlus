---
title: "How to generate PBR maps for a texture"
description: "Run gkpbr over the textures the shipped geometry uses, and put one map on screen to check it."
weight: 80
audience: ["mod-author"]
---

This guide shows a **mod author** how to drive `pbr/` (`gkpbr`), which produces
colour / roughness / metallic / normal / emissive / height sets for the `.RIM` textures Gunlok's
geometry actually samples - per UV region, because most of the sheets are atlases.

Run everything from the `pbr/` directory. It is a `uv` project and shares nothing with
`d3d8.dll`.

```
cd pbr
```

`GUNLOK_DIR` overrides install detection and `GKPBR_OUT` the output directory. `GEMINI_API_KEY`
is required by `probe`, by `classify` and by `maps --generate`; every other command is
arithmetic and file copies.

## Find out what there is

```
uv run python -m gkpbr.cli inventory
```

Walks every `.rif`, resolves each `BMPNAMES` entry against the install, and writes an albedo PNG,
a label image and `manifest.json` per texture - a region being "the texels one named part
samples". Nothing here calls a model.

## Check the model holds registration

```
uv run python -m gkpbr.cli probe
```

Run this before spending anything on a batch. A generative image model re-renders rather than
edits, and a few texels of drift that roughness survives will ruin a normal map or an emissive
mask.

## Classify, then derive the maps

```
uv run python -m gkpbr.cli classify "ground/gunlok rust.rim"
uv run python -m gkpbr.cli maps     "ground/gunlok rust.rim"
```

`classify` is stage 1 and is cached; `maps` is stages 2-4 and is derived arithmetic unless you
ask otherwise. Useful variations:

- `maps --generate` uses the image model for height instead of deriving it.
- `maps --delight` removes painted lighting from the regions stage 1 says carry it. Gunlok lights
  per vertex, so baked lighting in an albedo is wrong for it.
- `maps --green-down` writes the DirectX normal convention; the default is OpenGL/Blender.
- `classify --level <name>` and `--seen-only` restrict the run to what a level can show, or to
  what a profiled run actually drew.
- `classify --force`, `--refresh-stale` and `--adopt-cached` differ in what they assert about the
  cache - `gkpbr/cli.py` ranks them by that.

Omit the texture argument to run over the whole set.

## Put one on screen

The gates can say a height map is registered to its albedo; they cannot say a normal map's relief
points the right way. Looking is one command:

```
uv run python -m gkpbr.cli preview "ground/gunlok rust.rim" --map normal
```

That packs the map as a `.RIM` through `utils/rimutil` and writes it into
`<Gunlok>\gkplus\mods\gkpbr-preview\Graphics\...`, replacing the sheet it was derived from. Like
every other mod it is **not enabled by being there** - your boot script has to name it:

```js
mods.enable(mods.load("mods/gkpbr-preview"));
```

Then launch and look, or drive the capture from `utils/rendertest`.

```
uv run python -m gkpbr.cli preview --remove
```

**`--remove` is not optional.** A leftover preview mod goes on replacing that asset in every
later session and the game looks *fine*, which has already cost this project a session of
debugging.

`render.material.override` cannot substitute for the mod: it re-points a draw from one **loaded**
image to another, and it has no way to introduce an image the engine never loaded.

## Expect this from a derived normal map

A `.RIM` written this way carries no mip chain, and a derived normal map has almost nothing but
texel-scale content, so it speckles at minification. The height map from the same field renders
clean. Judge the two separately before concluding the pipeline is broken.

## Next

- [Generate a lighting map for a texture](/how-to/modding/generate-a-lighting-map/) - the map the
  Vulkan renderer reads directly.
- `pbr/README.md` is the design record, the measurements and the full flag list.

## Reference and background

- [gkpbr](/reference/data/gkpbr-cli/): every subcommand and flag, the environment it reads,
  and where each stage writes.
- [Mod metadata](/reference/data/mod-metadata/): what the preview mod it writes actually
  is.
- [Why mods are named, never discovered](/explanation/why-mods-are-named-never-discovered/): why a
  written preview mod does nothing until a boot script names it, and why leaving one
  enabled has bitten this project before.
