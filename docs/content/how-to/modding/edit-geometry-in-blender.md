---
title: "How to import and export .rif geometry with Blender"
description: "Install the io_scene_rif addon, bring a Gunlok model or level into Blender, and export one the game will load."
weight: 100
audience: ["mod-author"]
---

This guide shows a **mod author** how to work on Gunlok's `.rif` geometry in Blender using the
`io_scene_rif` addon. It is pure Python and needs nothing from `d3d8.dll`.

Blender 4.2 or newer.

## Install the addon

From the `blender/` directory:

```
uv run tools/build_zip.py
```

That writes `dist/io_scene_rif-<version>.zip`. Install it through
*Edit > Preferences > Get Extensions > Install from Disk*.

## Import a file

*File > Import > Gunlok RIF (.rif)*.

Import builds every chunk into a Blender datablock: objects and their meshes, one armature per
hierarchy with a bone per node, Actions per animation sequence, lights, Speakers, the texture
table and one material per texture index, and the per-vertex lighting colours as a paintable
colour attribute.

Textures and sounds are read from beside the file and are **never written back**. To change a
texture, ship a replacement `.RIM` instead - see
[How to replace a texture](/how-to/modding/replace-a-texture/).

## Author a new file

**Export reads the scene and nothing else**, and it only reads what the importer would have put
there: `rif_id`, `rif_index`, `rif_objhead` and `rif_absorbed`. A mesh added with *Add > Mesh*
carries none of those and is **silently absent** from the exported file. The operators under
*Object* mint them:

1. *Object > New Gunlok RIF* - an empty collection with the file-level chunks a `.rif` needs.
2. Model as usual, then select the objects and use *Object > Add to Gunlok RIF*. A mesh becomes
   an object with its own shape and a fresh shape id; an armature becomes the file's hierarchy;
   a light becomes an entry in its light set.
3. For an animated model, parent each mesh to its bone and mark each Action with
   *Object > Add Action to Gunlok RIF*.
4. *File > Export > Gunlok RIF*.

Adding to a file you imported is the usual way to build a level, since the file-level chunks and
the texture table come with it.

**Use a small mesh for a spawn locator, not an empty.** Across the shipped files every object
pairs with a shape; the geometry-less path is permitted by the format and never used by the game's
own assets.

## Mind the two identities

Both live inside chunk bodies, so neither is visible in Blender's own UI. Both are in
*Object Properties > Gunlok RIF*:

- **Name in file** - the only name the engine sees. A map section's `name`, every
  `for "<rif object>"` spawn point and every hierarchy node binding resolves against it by
  string comparison. **Renaming the object in the outliner does not change it.** A duplicated
  name is fine.
- **Shape ID** - duplicating an object in Blender copies it, and two objects claiming one shape
  is not representable. Export **refuses** that rather than writing it, and the panel offers a
  fresh id.

## Ship the result

Export writes an uncompressed `.rif`, which the game reads - 150 of the files it ships are stored
that way. Put it in a mod at the path it replaces, `rif/units/bug.rif` and so on, and enable the
mod: [How to package a mod](/how-to/modding/package-a-mod/).

To compress one anyway, `utils/rifutil` is the only compressor in the repository:

```
rifutil compress in.rif out.rif
```

## Check the round trip before you trust an export

The suites take the Gunlok directory and run with **Blender absent**, over the whole shipped
asset set:

```
python blender/tests/test_roundtrip.py "<Gunlok dir>"
python blender/tests/test_shapes.py    "<Gunlok dir>"
```

They are silent for minutes and then print everything at once - `test_shapes.py` takes a couple
of minutes and `test_rim.py` around twenty, so do not take the silence for a hang.

The scene round trip needs Blender itself:

```
blender --background --python blender/tests/test_scene.py -- "<Gunlok dir>" [N|all]
```

## Next

- `blender/README.md` is the full reference for how the file maps onto the scene, what is
  verified and what is not, and the known limitations.
- `rif_chunk_format.md` documents the format itself.

## Reference and background

- [Blender addon](/reference/data/blender-addon/): every operator, its options, and the
  property panels the addon adds.
- [Command-line utilities](/reference/data/cli-utilities/): `rifutil` and `riflights`, for
  inspecting a `.rif` outside Blender.
