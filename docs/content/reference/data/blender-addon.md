---
title: "Blender addon"
description: "io_scene_rif: its operators, their options, and the property panels it adds."
weight: 60
audience: ["mod-author"]
---

The `.rif` import/export extension, for mod authors. Pure Python, unrelated to `d3d8.dll`.
Registered by `blender/io_scene_rif/__init__.py`.

## The extension

| Field | Value |
|---|---|
| Extension id | `io_scene_rif` |
| Name | `Gunlok RIF` |
| Type | add-on |
| Minimum Blender | 4.2.0 |
| Manifest | `blender/io_scene_rif/blender_manifest.toml`, which owns the version number |
| Package | `dist/io_scene_rif-<version>.zip`, written by `blender/tools/build_zip.py` |
| Dependencies | none; the addon imports nothing outside Blender's own interpreter |

The build refuses to run when `blender_manifest.toml` and `blender/pyproject.toml` disagree about
the version.

## Operators

Twenty-two operators are registered. The full set of ids is derivable:

```bash
grep -rhoE 'bl_idname = "[^"]+"' blender/io_scene_rif/*.py | sort -u
```

| Id | Label | Surfaced in |
|---|---|---|
| `import_scene.rif` | Import RIF | *File > Import > Gunlok RIF (.rif)* |
| `export_scene.rif` | Export RIF | *File > Export > Gunlok RIF (.rif)* |
| `scene.rif_new` | New Gunlok RIF | *Object* menu; Object properties |
| `object.rif_add` | Add to RIF | *Object* menu; Object properties |
| `object.rif_add_dummy` | Add as Locator | *Object* menu; Object properties |
| `object.rif_add_sequence` | Add Action to RIF | *Object* menu; Object properties |
| `scene.rif_add_sound` | Add RIF Sound | *Object* menu; Collection properties |
| `scene.rif_add_emitter` | Add Ambient Emitter | *Object* menu; Object Data properties |
| `scene.rif_preview_setup` | Set Up Gunlok Preview | *Object* menu; Object properties |
| `scene.rif_preview_restore` | Restore Authored Materials | *Object* menu; Object properties |
| `scene.rif_add_cutscene` | Add Cutscene | *Object* menu; Object properties (cutscene panel) |
| `scene.rif_remove_sound` | Remove RIF Sound | Collection properties |
| `scene.rif_select_sound` | Select RIF Sound | Collection properties |
| `pose.rif_set_sound` | Set Keyframe Sound | Object properties |
| `action.rif_toggle_setting` | Toggle Sequence Setting | Object properties |
| `object.rif_new_shape_id` | Assign Fresh Shape ID | Object properties |
| `object.rif_new_light_id` | Assign Fresh Light ID | Object properties |
| `object.rif_enable_lighting` | Enable Vertex Lighting | Object properties |
| `object.rif_adopt_color_attribute` | Use Active Color Attribute | Object properties |
| `object.rif_navmesh_preview` | Preview Navmesh | Object properties |
| `object.rif_cutscene_preview` | Preview Cutscene Path | Object properties (cutscene panel) |
| `object.rif_cutscene_add_event` | Add Cutscene Event | Object properties (cutscene panel) |

The *Object* menu entries are appended to `VIEW3D_MT_object` after a separator, and are labelled
with "Gunlok" in the menu (`New Gunlok RIF`, `Add to Gunlok RIF`, `Add as Gunlok RIF Locator`,
`Add Action to Gunlok RIF`, `Add Gunlok RIF Sound`, `Add Gunlok Ambient Emitter`,
`Set Up Gunlok Preview`, `Restore Authored Materials`, `Add Gunlok Cutscene`).

## `import_scene.rif` options

File extension `.rif`; the file filter is `*.rif;*.RIF`.

| Option | Type | Default | Effect |
|---|---|---|---|
| `scale` | float, 1e-6 … 1000.0 | `scene.DEFAULT_SCALE` | Multiplier from RIF integer units to Blender metres |
| `y_down` | bool | on | Treat RIF coordinates as Y-down and convert to Blender's Z-up |
| `fuse_quads` | bool | on | Build each `SHPMRGDT` pair as one quad rather than two triangles. Export writes triangles either way |
| `load_textures` | bool | on | Decode each `.RIM` the file names and pack it into the `.blend`. Texture names are imported either way |
| `texture_dir` | directory path | empty | The directory a texture name is relative to. Empty searches above the `.rif` for one |

Warnings the operator reports: no textures directory found above the file; faces that share their
vertices with another and cannot be represented in Blender; textures that could not be read.

## `export_scene.rif` options

File extension `.rif`; the file filter is `*.rif;*.RIF`.

| Option | Type | Default | Effect |
|---|---|---|---|
| `collection` | string | empty | Which imported collection to write. Empty uses the only one present |
| `textures` | enum `NONE`, `CHANGED`, `ALL` | `NONE` | Whether to write `.RIM` files alongside. `CHANGED` writes only the textures whose pixels no longer match the `.RIM` they were imported from. The `.rif` itself stores names either way |
| `texture_dir` | directory path | empty | Where the `.RIM` files go, laid out as the game's `Graphics` folder is |
| `compress_textures` | bool | on | Pack written `.RIM` files with ByteRun1. Off writes them raw |

The export is cancelled with an error when two objects claim the same shape id.

## Panels

| Panel | Space | Context | Contents |
|---|---|---|---|
| Gunlok RIF | Properties | Object | Object-level RIF properties, and the shape, lighting, sequence, sound and navmesh operators |
| Gunlok Cutscene | Properties | Object | Cutscene path and event properties, and the three cutscene operators |
| Gunlok RIF Sounds | Properties | Collection | The collection's sound table, with add, remove and select |
| Gunlok RIF | Properties | Object Data | Ambient emitter properties, and the add-emitter operator |
| Gunlok RIF | Properties | Material | `rif_texture_name`, the `.RIM` the material names |

## Module surface

`blender/io_scene_rif/` splits into a Blender half and a decoder half. Only `__init__.py` and
`scene.py` import `bpy`; the decoders (`rif.py`, `rim.py`, `shapes.py`, `heads.py`,
`cutscene.py`, `schema.py`, `bmpnames.py`, `emitters.py`, `sounds.py`) do not, and are imported
directly by `pbr/` and `lightmap/`.

## Related

- `blender/README.md`: the addon's own documentation.
- `rif_chunk_format.md`: the file format.
- [gkpbr](/reference/data/gkpbr-cli/) and [gklightmap](/reference/data/gklightmap-cli/): the two
  tools that import these decoders.
- [How to import and export .rif geometry with Blender](/how-to/modding/edit-geometry-in-blender/): installing it and taking a model through
  the round trip.
