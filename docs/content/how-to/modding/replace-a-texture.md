---
title: "How to replace a texture"
description: "Get a Gunlok .RIM out to PNG, edit it, and serve the result back through a mod."
weight: 60
audience: ["mod-author"]
---

This guide shows a **mod author** how to replace one of the game's textures.

You need the `rimutil` executable (built from `utils/rimutil`; a normal build of the repository
produces it) and a mod to put the result in; see
[How to package a mod](/how-to/modding/package-a-mod/).

## Find the name the engine asks for

Textures are named by a `BMPNAMES` table inside a `.rif`, so the file name on disk is not always
what you expect. The reliable way is to watch what the engine opens: with a mod already enabled,
play into the area you care about and read `mods.recent` over the REPL. Otherwise, look under
`<Gunlok>\Graphics\` for the `.RIM`.

Textures live under the `graphics` category, so a file that replaces
`<Gunlok>\Graphics\Ground\gunlok rust.RIM` goes in your mod at
`Graphics/Ground/gunlok rust.RIM`.

## Decode, edit, re-encode

```
rimutil decompress "<Gunlok>\Graphics\Ground\gunlok rust.RIM" rust.png
# ... edit rust.png ...
rimutil compress rust.png "mymod\Graphics\Ground\gunlok rust.RIM"
```

Keep the same pixel dimensions. Then enable the mod from your boot script and look.

## Choose the format

`compress` takes `--format`, defaulting to `dxt3`:

- **`dxt3`** is the default. Gunlok accepts only DXT1 and DXT3; DXT3's RGB is no worse than
  DXT1's and its 4-bit alpha strictly beats DXT1's 1 bit.
- **`dxt1`** is half the size, with 1-bit alpha. Fine for an opaque sheet.
- **`body`** is palettized, exactly lossless *on disk*, 2–6× the size, and no DXT compressor
  involved. Add `--raw` to skip its RLE.

**`dxt5` is refused by name.** The engine's format list holds only DXT1 and DXT3 and the setter
that receives the fourcc drops anything else silently, so a DXT5 file would render with garbage
alpha instead of failing.

`body` is *not* lossless in the engine: Gunlok ignores the `ALPH` chunk a palettized image
carries alpha in, so such a texture loads fully opaque. `compress` refuses an image with graded
alpha in that format, and warns about a cut-out that cannot use a single transparent index.

## If the replacement shimmers when it is far away

The engine never generates mip levels, and a `.RIM` written this way carries none. A texture
whose content varies texel to texel, a derived normal or height map especially, sparkles at
minification with nothing to fall back on.

Use the DDS route instead, which carries an authored mip chain:
[How to get true-colour textures into the game](/how-to/modding/true-colour-textures/).

## Confirm the game took your file

Neither the game nor the tool will tell you that a path was slightly wrong, because a miss just
falls through to the shipped asset and the game looks fine. Check from the REPL after loading a
level:

```js
mods.served                                  // non-zero once something was answered
mods.recent                                  // should name your path
```

See [How to drive the game from the REPL](/how-to/modding/drive-the-game-from-the-repl/).

## Next

- [Generate PBR maps for a texture](/how-to/modding/generate-pbr-maps/)
- [Generate a lighting map for a texture](/how-to/modding/generate-a-lighting-map/)
- [Import and export `.rif` geometry with Blender](/how-to/modding/edit-geometry-in-blender/), if
  the texture table itself needs changing.
- `rif_chunk_format.md` documents the `.RIM` container and the engine's constraints on it.

## Reference and background

- [Command-line utilities](/reference/data/cli-utilities/): `rimutil`'s full flag set,
  including which formats it refuses and why.
- [Mod metadata](/reference/data/mod-metadata/): the directory the replacement has to sit
  in for the engine to find it.
- [Why mods are named, never discovered](/explanation/why-mods-are-named-never-discovered/): why
  the file being in the mod is not enough.
