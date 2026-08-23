---
title: "How to get true-colour textures into the game"
description: "Serve an uncompressed DDS through a mod: the only route to 24-bit colour and an authored mip chain in Gunlok."
weight: 70
audience: ["mod-author"]
---

This guide shows a **mod author** how to get a texture into Gunlok without DXT quantisation and
without the 4-bit-per-channel surface every other route lands on, by serving a DDS.

GkPlus registers a DDS codec with the engine's own image layer. The layer picks its decoder by
**magic bytes** and nothing on the texture path reads a file extension, so a DDS works wherever
a `.RIM` does.

## Write the file

For true colour, write **uncompressed 24-bit (B,G,R) or 32-bit (B,G,R,A)** with a full mip
chain. `lightmap/gklightmap/dds.py` is a small, dependency-free writer of exactly this form if
you need one to copy.

For ordinary artwork, DXT1 and DXT3 also work and are smaller. Two constraints:

- **A DXT mip chain must stop at 4×4.** The engine's row loop decrements by four and exits on
  exactly zero, so a 2×2 level makes it run past the locked surface. Levels past that are
  dropped, with a line to the debugger.
- **Uncompressed chains may run to 1×1.** The floor is an S3TC rule and does not apply.

The parser refuses, by name, everything the engine cannot render: DXT2, DXT4, DXT5, the DX10
extension header, cubemaps, volume textures, and any pixel layout that would need a swizzle.

Authoring a mip chain matters more here than the format does: **the engine generates none**, so
a source without one shimmers at distance.

## Put it in a mod

Either name it as the file the engine already opens, keeping the `.RIM` name, since the codec
sniffs the bytes:

```
mymod/Graphics/Bitmaps/MAIN MENU 01.RIM      ... holding DDS bytes
```

or, if you also ship the `.rif` that names it, put `Ground\ground.dds` in the `BMPNAMES` entry
and place the file at that path. Both reach the same codec.

Enable the mod from your boot script as usual.

## About the 32-bit surface

The uncompressed path works by reporting 8 alpha bits, which makes every candidate surface
format fail and the fallback descriptor win, and that fallback is only true colour when the
engine's `Use32BitTextures` is on. GkPlus forces it on for you. `GKPLUS_32BIT_TEXTURES=raw`
leaves the game's own value alone, which is the switch to reach for if you are comparing against
stock behaviour.

## Confirm it was picked up

A texture that failed to load is not always obvious on screen. Check `mods.recent` from the REPL
for your path, and watch the debugger output: the codec reports a truncated mip chain and the
image layer reports a refusal.

## Next

- [Replace a texture](/how-to/modding/replace-a-texture/) for the `.RIM` route.
- `file_io_notes.md` §4 documents the image-codec registry and the contract a codec implements.

## Reference and background

- [Command-line utilities](/reference/data/cli-utilities/): `rimutil`, for getting the
  original out first.
- [Environment variables](/reference/data/environment-variables/): the 32-bit texture
  switch this path depends on.
- [Mod metadata](/reference/data/mod-metadata/): packaging the result.
