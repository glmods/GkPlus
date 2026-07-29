# Gunlok RIF for Blender

Import and export Rebellion `.rif` assets — the chunk format Gunlok (2000) uses for all its
geometry. Pure Python, no compiled extension: the addon installs as a plain zip and needs
nothing from the GkPlus DLL.

**The scene is the whole file.** Import builds every chunk into a Blender datablock; export
reads nothing but the scene. There is no source-file parameter and nothing is stored as opaque
bytes, so a `.blend` can be moved to a machine that has never seen the original `.rif` and still
produce one.

```
blender/
  pyproject.toml         uv project: dev tooling and lint config only
  io_scene_rif/          the addon
    blender_manifest.toml
    __init__.py          the operators
    scene.py             chunk tree <-> Blender scene (the only bpy module)
    rif.py               container format: huffman, chunk tree, serialization
    schema.py            chunk bodies as typed fields
    shapes.py            REBSHAPE geometry decode/encode
    bmpnames.py          the file's texture table
    rim.py               .RIM textures: IFF container, DXT1/DXT3 decode
  tools/build_zip.py     builds dist/io_scene_rif-<version>.zip
  tests/
    test_roundtrip.py    container format, runs without Blender
    test_schema.py       typed fields, runs without Blender
    test_shapes.py       geometry, runs without Blender
    test_rim.py          textures and the texture table, runs without Blender
    test_scene.py        the scene round trip, runs inside Blender
```

Only `scene.py` and `__init__.py` import `bpy`, which is what lets the first four tests run over
the whole shipped asset set with no Blender in the picture.

## Build and install

```bash
uv run tools/build_zip.py
```

Writes `dist/io_scene_rif-<version>.zip`. `blender_manifest.toml` owns the version because it is
what Blender reads; the build fails if `pyproject.toml` disagrees.

Blender 4.2 or newer (developed against 5.2). Install from *Edit ▸ Preferences ▸ Get Extensions
▸ Install from Disk*, then *File ▸ Import ▸ Gunlok RIF (.rif)*.

```bash
uv run --group dev ruff check .
```

## How the file maps onto the scene

The rule is: **container chunks become objects, leaf chunks become typed fields on their
parent's object.** On top of that, the things with a real Blender equivalent get one.

| RIF | Blender |
|---|---|
| `RBOBJECT` | an object, with location and orientation from `OBJHEAD1` |
| `REBSHAPE` | that object's mesh datablock |
| `OBJCHIER` | parented empties named from `OBJHIERD` — the skeleton (`Waist`, `Chest`, `Hand Right`) |
| `STDLIGHT` | a light — colour, energy, cutoff distance, orientation |
| `OBANSEQS` | Actions on the `OBJCHIER` object it hangs off, in NLA tracks |
| `OBANSEQC` | one of those Actions, named from `OBASEQHD` (`Seq_Walk`, `Seq_Die`) |
| `SHPMRGDT` | a per-face `rif_merge_group` attribute |
| `SHPVTINT` | a per-vertex `rif_vertex_intensity` attribute |
| `BMPNAMES` | the texture table on the collection, plus one material per texture index |
| polygon `engine_type` / `flags` | per-face attributes |
| everything else | typed `int32`/`float32`/string properties |

Three properties carry the structure: `rif_id` (the chunk id), `rif_index` (position among
siblings — Blender does not preserve child order) and `rif_absorbed` (the leaves folded into a
datablock, so they re-emit in place).

**An object finds its shape by id, not by position.** `OBJHEAD1+0x38` matches `SHPHEAD1+0x14`,
which is what AvP's `Object_Chunk::assoc_with_shape_no` does. Document order disagrees with it in
86 of the 563 shipped files, so pairing positionally attaches geometry to the wrong transform in
one file in seven.

**An object's name is the trailing string at `OBJHEAD1+0x3c`, not the field at 0x04.** That field
is a tag (`Player` in 6,383 objects, junk in 2,783); the real names are `Head`, `Waist`, `Chest`,
`Upper Arm Right`. `DUMOBJDT` is the same shape at `0x34`.

**Meshes are parented to the hierarchy node that animates them.** `OBJHIERD` names the object a
node drives, resolved by `strcmp` exactly as AvP does — 3,541 objects and 1,488 dummies, with 221
nodes binding nothing (legal; AvP nulls the pointer). Each node takes the rest transform of the
object it binds, relative to its parent node, and the object sits at identity underneath. Rest pose
is unchanged, so nothing moves on import.

### Animation

A character's animation is one Action **per bone**, not one per character — `Seq_Walk` only means
anything when that strip is active on all 81 nodes at once. So the sequences are laid end to end on
a shared timeline, each in its own frame range, with a **timeline marker per sequence**.

On import the playback (preview) range is set to the **first** sequence, so pressing Play shows one
animation rather than all of them back to back. To watch another: jump to its marker and hit *Set
Preview Range*, or clear the preview range to run the lot. `rif_sequence_ranges` on the collection
holds every name and range.

Two details that are easy to trip over:

- **Every strip uses `extrapolation = NOTHING`.** The default `HOLD` holds a strip's value outside
  its own range, and with 36 tracks stacked on one node the topmost strip masks all the others
  across the whole timeline — nothing appears to animate at all.
- **The root node has no animation.** It is the obvious thing to click, and its sequences are
  genuinely empty: 973 of the game's 29,550 sequences contain no frames. The animation lives on the
  child bones (`Waist`, `Thigh Right`, …).

Export is unaffected by any of this: it restores each object's recorded rest pose before reading
transforms, because animation evaluation *writes into* an object's transform properties and simply
clearing the action does not put them back.

Meshes named `L5#…` are LOD variants and are correctly not rigged.

### Textures

A polygon does not name its texture — it carries an **index** (`colour & 0xfff`) into one
file-level table, the `BMPNAMES` chunk under `REBENVDT`, whose entries are paths like
`Units\baddies3.RIM` relative to the install's `Graphics` folder. The image itself is not in the
`.rif` at all. So three separate things land in three separate places:

| | Where it lives | Written back? |
|---|---|---|
| the table | `rif_bmpnames` on the collection, whole and in order | yes, rebuilt on export |
| the index | `rif_texture_index` on a material | yes, via the table |
| the name | `rif_bmp_name` on the same material | yes — **this is the retexturing knob** |
| the image | a packed Blender image, wired to a Principled BSDF | no, `.RIM` files are read-only |

**To retexture, edit `rif_bmp_name`** in the material's Custom Properties (N-panel ▸ Material ▸
Custom Properties) and export. Every polygon wearing that material moves to the named texture,
and if the file never mentioned it the table gains an entry at a fresh index. Assigning a
different image to the texture node does nothing on export — the `.rif` stores the name, and only
the name.

Materials are made per import, not shared between files, because an index only means something
inside its own file: the same `.RIM` is entry 11 in one level and entry 4 in the next. Images
*are* shared, which is where the cost is. A material for an index the table does not list — the
`0xfff` untextured sentinel, or the junk indices the `_shadow` meshes carry — keeps the name
`rif_tex_<n>` and round-trips as that raw index.

**A stored UV is a texel coordinate, not a fraction** — `SHPUVCRD` holds 0..width and 0..height —
so it is divided by the size of the texture the polygon wears on the way in and multiplied back
on the way out, with V flipped because `v = 0` is the image's *top* row. Both directions are
exact: every texture in the game is a power of two. The scale is taken from the image when it is
loaded, from the table's declared size otherwise, and from `rif_uv_scale` on the material as a
last resort — so a `.blend` exports the same UVs on a machine with no textures installed, while
retexturing onto a different-sized `.RIM` rescales them to that texture.

Import options: **Load textures** decodes each `.RIM` and packs it into the `.blend` (the names
import either way), and **Textures** overrides the directory, which is otherwise found by
searching upwards from the `.rif` for a `Graphics` folder. Nothing about the export depends on
either — a file imported with no textures found still writes its table back unchanged.

`.RIM` is not a RIF chunk file. It is IFF (big-endian, `LIST`/`FORM`/`PROP` groups) carrying
DXT1 or DXT3 in an `S3TC` chunk, plus a mip chain the importer discards since Blender makes its
own. `rim.py` decodes it, and the images are packed rather than pushed in through `pixels` —
Blender keeps a *generated* image's settings across a `.blend` save but not its pixels.

## Semantic, not byte-exact

The bar is that the file coming out *means* the same as the file going in, which is what lets the
exporter regenerate rather than mirror:

- `SHPCENTR` is recomputed from the vertices.
- `SHPPCINF` is discarded — 681 of the 9,357 shipped shapes carry none, and every AvP code path
  guards on its lookup returning null.
- `SHPMRGDT` and `SHPVTINT` are authored per-element data, so they ride as mesh attributes and
  survive an edit instead of going stale.

Nothing is silently dropped when you edit a mesh.

The **texture table is the exception, and is held to byte-exactness**: it is carried whole rather
than regenerated, so an import/export cycle that touches no material has no reason to disturb a
byte of it — uninitialised padding after a name included. `test_scene.py` asserts exactly that.

## Testing

```bash
python blender/tests/test_roundtrip.py "<Gunlok dir>"
python blender/tests/test_schema.py    "<Gunlok dir>"
python blender/tests/test_shapes.py    "<Gunlok dir>"
python blender/tests/test_rim.py       "<Gunlok dir>"
```

```bash
blender --background --python blender/tests/test_scene.py -- "<Gunlok dir>" all
```

`test_scene.py` is the one that matters for self-containment: it builds the scene, saves a
`.blend`, **resets Blender**, reopens the `.blend`, and exports from that — the source `.rif` is
never touched during export. If the scene were not self-contained, that is the phase that fails.

## What is verified, and what is not

Measured across all 563 shipped files:

| Claim | Evidence |
|---|---|
| Container format is complete | 563/563 parse, re-serialize byte-identical |
| Chunk bodies survive as typed fields | `encode(decode(body)) == body` for 485,663 leaf chunks across 44 ids |
| Every polygon is a triangle | 1,766,071/1,766,071, three valid indices, `-1` in both spares |
| Object↔shape pairing | id match resolves all 9,313 objects, no shape claimed twice |
| `OBASEQFR` is a keyframe | unit quaternion in 100.000% of 323,334; time non-decreasing in all 28,577 sequences |
| `STDLIGHT` orientation | orthonormal 3×3 in 16.16 for 100.00% of 3,794 lights |
| Face winding | recomputed normals agree with `SHPPNORM` on 99.91% of 1.77M faces |
| Texture table rebuilds exactly | `encode(decode(body)) == body` for all 527 tables, 1,601 entries |
| A polygon's texture index is a table `index` | resolves 1,518,963 of 1,766,071 polygons; of the rest 15,327 are the `0xfff` sentinel and 138,893 are in `_shadow` files |
| `.RIM` decoding | 490 of the 513 shipped textures decode; the other 23 are the palettized `*_fmv_*` set, which carries no S3TC image. Of the 365 a `.rif` actually names, 361 are DXT1, 3 DXT3, 1 is `*_fmv_*` and 4 are missing from the install |
| UVs are texels, not fractions | the 99th percentile of `\|u\|` is within 7% of each texture's own width, at every size in the game; 374,658 of 376,641 sampled pairs are whole numbers |
| V grows downward | 86.3% of the 8,916 axis-aligned wall polygons across the levels put the low V at the top of the wall; 16 of 17 levels lean that way |
| UV index survives `colour` | 1,766,071/1,766,071 re-encode exactly, including the four shapes whose table needs more than 16 bits (282,412 of their 282,454 polygons index their own position) |

Not verified:

- **Nothing exported has been loaded back into Gunlok.** The output is checked against the format
  and against itself, not against the game. This matters more now that the exporter regenerates
  rather than mirrors: the `SHPPCINF` drop is reasoned from AvP's null-guards and the shapes that
  ship without it, not from watching Gunlok load one.
- **The import scale is a convention.** RIF coordinates are integers; the engine's own factor is
  per-rif data read at level load (`gk::RifUnitScale`), not a constant this addon can know. The
  0.001 default comes from character shapes spanning about ±1900 units against a roughly
  two-metre character.
- **The vertical axis is settled, not assumed: RIF is Y-down.** A biped's parts all sit at
  negative Y — feet nearest the origin (~-100) and the top of the head furthest (~-1990) — so the
  body extends in -Y from the ground and -Y is up. Assembled in Blender, a character spans
  Z = 0.000 to 2.589 m, feet exactly on the ground plane. The mapping is `(x, y, z) -> (x, z, -y)`,
  determinant +1, so it does not mirror — which is right because RIF is right-handed: an ordinary
  right-handed cross product of raw RIF coordinates agrees with the shipped `SHPPNORM` on 99.91%
  of 1.77M faces.

## Known limitations

- **Two faces on the same three vertices cannot survive.** Blender merges them, so a shape with a
  doubled or reverse-wound triangle loses one — 775 of them ship, across 193 shapes. The importer
  drops them itself rather than letting Blender do it silently (which would shift every later
  face onto the wrong texture and UVs) and reports the count.
- **`.RIM` files are read-only.** Textures are decoded for display and their *names* round-trip,
  but nothing writes an image back: that would need a DXT compressor and a mip chain, and would
  re-compress a lossy format. To ship a new texture, author the `.RIM` with the game's own tools
  and point a material's `rif_bmp_name` at it.
- **One texture in the game does not decode**, and 23 `.RIM` files overall: the `*_fmv_*` ground
  set stores three palettized `CMAP`/`BODY` variants instead of an `S3TC` image. Their materials
  still carry the name and their table entries are untouched, so only the preview is missing.
- **26 chunk ids still use the generic typed fallback** rather than named fields — they round-trip
  exactly, but their contents read as a `data` int array rather than meaningful names. `OBJHEAD1`'s
  tail, `OBASEQHD`, `DUMOBJDT`, `SHPHEAD1`, `OBINTDT` and `AVPSTRAT` are the notable ones.
