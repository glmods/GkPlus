# RIF Chunk Format Reference

Reverse-engineered from the Gunlok (2000) game binary via Ghidra static analysis, then
cross-checked against the **published Aliens vs Predator (1999) source code** — see
[Upstream](#upstream-the-aliens-vs-predator-1999-3dc-engine) for what that confirms and where
the ground truth lives.

## File Structure

RIF files use a chunked binary format similar to IFF/RIFF.

### Chunk Header

Every chunk (container or leaf) has the same 12-byte header:

| Offset | Size | Type   | Description                         |
|--------|------|--------|-------------------------------------|
| 0x00   | 8    | char[] | Chunk ID (8 ASCII characters)       |
| 0x08   | 4    | uint32 | Total chunk size (header + body)     |

Body data starts at offset 0x0C and is `total_size - 12` bytes long.

### Compression

Files may be compressed with Huffman coding:

- **Uncompressed**: Starts with a standard chunk header (e.g. `REBINFF2`)
- **Compressed**: Starts with `REBCRIF1` followed by:

| Offset | Size | Type       | Description                    |
|--------|------|------------|--------------------------------|
| 0x00   | 8    | char[]     | `REBCRIF1`                     |
| 0x08   | 4    | int32      | Compressed data size            |
| 0x0C   | 4    | int32      | Uncompressed data size          |
| 0x10   | 44   | int32[11]  | Huffman codelength counts       |
| 0x3C   | 256  | uint8[256] | Byte assignment table           |
| 0x13C  | ...  | bytes      | Compressed data                 |

---

## Chunk Types Overview

103 chunk types identified. 24 are **containers** (body = child chunks), 79 are **leaves** (body = raw data).

### Notation

- **string**: Null-terminated ASCII string
- **int32**: 32-bit signed little-endian integer
- **uint32**: 32-bit unsigned little-endian integer
- **float**: 32-bit IEEE 754 float
- **Vec3**: 3 x float (x, y, z) = 12 bytes
- **Vec4**: 4 x float (x, y, z, w) = 16 bytes

---

## Container Chunks

Container chunk bodies consist entirely of concatenated child chunks. Some also initialize extra fields on the in-memory object but do not read additional data from the stream beyond the child chunks.

| Chunk ID   | Class Name                    | Address    | Notes |
|------------|-------------------------------|------------|-------|
| `REBINFF2` | `File_Chunk`                  | 0x005afeb0 | Top-level file container. Handles REBCRIF1 decompression. |
| `RBOBJECT` | `Object_Chunk`                | 0x005b1d30 | Game object. Initializes location/orientation fields to zero. |
| `REBSHAPE` | (Shape_Chunk)                 | 0x005b5ad0 | Shape definition container. Uses `Chunk_With_Children` base. |
| `SUBSHAPE` | (SubShape_Chunk)              | 0x005b67c0 | Sub-shape container. Uses `Chunk_With_Children` base. |
| `MODULEDT` | (Module_Data_Chunk)           | 0x005b31f0 | Module data container. |
| `OBJPRJDT` | (Object_Projectile_Data)      | 0x005b39a0 | Projectile data container. |
| `ANIMSEQU` | (Anim_Sequence_Chunk)         | 0x005b6c80 | Animation sequence. Post-processes after loading children. |
| `ANIMFRAM` | (Anim_Frame_Chunk)            | 0x005b76b0 | Animation frame. Creates ANIFRADT + SHPRAWVT children from data. |
| `ASALTTEX` | (Alternate_Shape_Texture)     | 0x005b7960 | Alternate texture set for shape. |
| `SHPEXTFL` | (Shape_External_File)         | 0x005b9900 | External shape file reference. |
| `SHPMORPH` | (Shape_Morph_Chunk)           | 0x005b9f80 | Shape morphing container. Stores parent ref. |
| `SHPFRAGS` | (Shape_Fragments_Chunk)       | 0x005baa80 | Shape fragments container. |
| `REBENVDT` | (Environment_Data_Chunk)      | 0x005ca1e0 | Environment data. Zeroes out 8 extra fields. |
| `SPECLOBJ` | (Special_Objects_Chunk)       | 0x005ca460 | Special objects container. |
| `OBJCHIER` | (Object_Hierarchy_Chunk)      | 0x005cafb0 | Object hierarchy container. |
| `OBANSEQS` | (OBAnim_Sequences_Chunk)      | 0x005ccb50 | Object animation sequences container. |
| `OBANSEQC` | (OBAnim_Sequence_Chunk)       | 0x005cce30 | Single object animation sequence. Complex: also reads data. |
| `SOUNDEXD` | (Sound_Extended_Data)         | 0x005ce900 | Extended sound data container. |
| `LIGHTSET` | (Light_Set_Chunk)             | 0x005d2a00 | Light set container. |
| `DUMMYOBJ` | (Dummy_Object_Chunk)          | 0x005d2090 | Dummy object. Creates DUMOBJDT child from params. |
| `CUTSHEAD` | `Cutscene_Header_Chunk`       | 0x005d7b50 | Cutscene header. Creates Cutscene_Data_Chunk child. |
| `CUTSCUSR` | `Cutscene_User_Chunk`         | 0x005d81c0 | Cutscene user data container. |
| `CUTTRACK` | (Cutscene_Track_Chunk)        | 0x005d8c90 | Cutscene track container. |
| `SUBRIFFL` | (Sub_RIF_File_Chunk)          | 0x005b0ec0 | External RIF file reference. Opens and loads another .rif file. |
| `OBINTDT\0` | `Object_Interface_Data_Chunk` | 0x005b30a0 | **A container, not a leaf** - and its id is seven characters NUL-padded, `b"OBINTDT\0"`. Every one of the 9,313 in the shipped assets is a child of `RBOBJECT` holding a single `OBJNOTES`, usually the editor's placeholder "Enter notes here". Treating it as a leaf hides 9,313 chunks |

---

## Leaf Chunks

### String Chunks

Body is a single null-terminated string.

| Chunk ID   | Purpose                        | Address    |
|------------|--------------------------------|------------|
| `RIFFNAME` | Object/asset name              | 0x005b1810 |
| `OBHIERNM` | Hierarchy node name            | 0x005cb640 |
| `CUTTRNAM` | Cutscene track name            | 0x005d8f30 |
| `CTUSRHIE` | Cutscene user hierarchy name   | 0x005d8700 |
| `SOUNDDIR` | Sound directory path           | 0x005ca5d0 |
| `SOUNDNAM` | Sound file name                | 0x005cebd0 |
| `SHPEXTFN` | External shape filename        | 0x005b99f0 |
| `SHPFNAME` | Shape filename                 | 0x005ba8f0 |
| `SHPFRGTP` | Shape fragment type name       | 0x005bab30 |
| `TRSNDCAT` | Track sound category name      | 0x005b3f00 |
| `EXTOBJNM` | External object name           | 0x005b9bb0 |

### Fixed-Size Data Chunks

Body is a fixed number of int32/float values read directly.

| Chunk ID   | Purpose                    | Address    | Body Format |
|------------|----------------------------|------------|-------------|
| `RIFVERIN` | RIF version info           | 0x005b18e0 | 1 x int32 (version) |
| `AMBIENCE` | Ambient settings           | 0x005d3410 | 1 x int32 |
| `ENVSDSCL` | Environment sound scale    | 0x005cab80 | 2 x float (8 bytes) |
| `OBASEQFL` | OB anim sequence flags     | 0x005cd8c0 | 1 x int32 - **fully decoded**, see below |
| `OBASEQTM` | OB anim sequence time      | 0x005cd7e0 | 1 x int32 - `sequence_time`, **milliseconds** |
| `OBASEQSP` | OB anim sequence speed     | 0x005cd840 | `{sequence_speed mm/s, angle deg, spare}` |
| `SHPFLAGS` | Shape flags                | 0x005b9170 | 1 x uint32 (flags) |
| `LITSCALE` | Light scale                | 0x005d3010 | 1 x float (scale) |
| `MODFLAGS` | Module flags               | 0x005b37c0 | 2 x int32 (8 bytes) |
| `ENVACOUS` | Environment acoustics      | 0x005ca550 | 3 x int32 (12 bytes) |
| `MODACOUS` | Module acoustics           | 0x005b3920 | 3 x int32 (12 bytes) |
| `CUTTRFOV` | Cutscene track FOV         | 0x005d9370 | 3 x float (12 bytes) |
| `CTUSNDPR` | Cutscene sound properties  | 0x005d8980 | 6 x int32 (24 bytes) |
| `CONSTYPE` | Connection shape type      | 0x005b7cd0 | 1 x int32 (type) |

### Vec3/Geometry Chunks

| Chunk ID   | Purpose                    | Address    | Body Format |
|------------|----------------------------|------------|-------------|
| `SHPCENTR` | Shape center point         | 0x005b90d0 | **int32 x3 centre + float radius** (16 bytes). The centre is `(min+max)/2` per axis with C's truncate-toward-zero division - exact for all 9,244 shipped shapes. The radius is the distance of the furthest vertex from the **origin**, not from that centre (AvP's `ChunkShape::radius`, "radius of points about 0,0,0"). It is only accurate to float32 in about three quarters of the files and drifts up to 3.2 absolute in the rest, so it is partly stale authoring output rather than a function of the final vertex list |
| `FRAGDATA` | Fragment data              | 0x005bad50 | 4 x int32 (16 bytes) |
| `FRAGLOCN` | Fragment location          | 0x005badd0 | Vec3 position + Vec3 orientation + 1 int32 (28 bytes) |
| `HIERBBOX` | Hierarchy bounding box     | 0x005cc3b0 | 2 x Vec3 (24 bytes: min + max) |
| `ANSHCEN2` | Anim shape center v2       | 0x005b6b90 | 5 x int32 (20 bytes) |

### Array Chunks

Body starts with an element count or size, followed by an array of fixed-size entries.

| Chunk ID   | Purpose                  | Address    | Body Format |
|------------|--------------------------|------------|-------------|
| `SHPRAWVT` | Shape raw vertices       | 0x005b8250 | Array of **int32** x3 (count = body_size / 12). AvP's `ChunkVectorInt` - these are integers, not floats |
| `SHPPNORM` | Shape polygon normals    | 0x005b8650 | Array of float x3 (count = body_size / 12). AvP's `ChunkVectorFloat` |
| `SHPVNORM` | Shape vertex normals     | 0x005b8430 | Array of float x3 (count = body_size / 12). AvP's `ChunkVectorFloat` |
| `SHPVFLAG` | Shape vertex flags       | 0x005b8360 | Array of uint32 (count = body_size / 4) |
| `SHPPOLYS` | Shape polygons           | 0x005b8930 | Array of polygon structs (count = body_size / 36). See below. |
| `SHPMRGDT` | Shape merge data         | 0x005b96c0 | **Exactly one int32 per polygon** - AvP's `Shape_Merge_Data_Chunk` is `{int *merge_data; int num_polys}` with `chunk_size = 12 + num_polys*4`, and the element count equals the `SHPPOLYS` polygon count in all 9,357 shipped shapes. Values are a merge-group id, mostly `-1` (none) with small positive ids. Authored per-polygon data, **not** derived from geometry, so it belongs on the polygon rather than being regenerable |
| `HIDEGDIS` | Hierarchy degradation distances | 0x005cc1c0 | uint32 `num_detail_levels` + that many int32 distances. AvP's `Hierarchy_Degradation_Distance_Chunk`; the count is 10 in all 13 shipped files that carry one. Sits at the **file root**, and drives the `L<n>#` level-of-detail convention below. Was documented as "hierarchy edge display" |
| `SHPPRPRO` | Shape preprocessed data  | 0x005bae70 | uint32 count + count x int32 (+ additional context) |
| `CUTPOINT` | Cutscene points          | 0x005d91a0 | uint32 count + count x 16 bytes (4 x float) |
| `CTUSSPPO` | Cutscene special points  | 0x005d8a80 | uint32 count + count x 36 bytes (9 x float) |
| `OBANALLS` | OB anim all sequences    | 0x005cd960 | uint32 count + count x 20 bytes (5 x int32) |
| `OBJTRAK2` | Object track data v2     | 0x005b3aa0 | uint32 count + count x 76 bytes (0x4C) |
| `VMDARRAY` | VMD array data           | 0x005b32a0 | uint32 count + count x 16 bytes (0x10) |

#### SHPPOLYS Polygon Entry (36 bytes / 0x24)

Measured across all 1,766,071 polygons in the 563 shipped `.rif` files.

| Offset | Size | Type   | Description              |
|--------|------|--------|--------------------------|
| 0x00   | 4    | int32  | `engine_type` - render/material mode. Only ever 2, 3, 7 or 24 |
| 0x04   | 4    | int32  | `normal_index` - index into `SHPPNORM`; sequential in every shipped shape |
| 0x08   | 4    | int32  | `flags`                  |
| 0x0C   | 4    | uint32 | `colour` - packs the material refs, see below |
| 0x10   | 20   | int32[5] | `vert_ind[5]` - vertex indices, `-1` terminated |

**Every polygon is a triangle**: exactly three valid indices and `-1` in both spare slots, in
all 1,766,071 of them, with every index in range.

This is *not* AvP's `ChunkPoly` (`CHNKTYPE.HPP`), which is
`{engine_type, normal_index, flags, colour, num_verts, vert_ind[4]}`. Gunlok dropped the
explicit `num_verts` and widened the index array to five, keeping the same 36-byte stride.

`colour` packs two indices:

- **texture index** = `colour & 0xfff`, into the file-level `BMPNAMES` table (Gunlok has no
  `SHPTEXFN` - it never appears in a shipped file).
- **UV index** = `colour >> 16`, into `SHPUVCRD` - **except in the four shapes whose UV table
  does not fit in 16 bits**, where bits 12-15 carry the top of it.

AvP's `ChunkPoly::GetUVIndex` folds bits 12-15 in as a 20-bit index whenever they are set.
Gunlok uses that **only where it has to**, and the two halves of that are each measured across
all 1,766,071 shipped polygons:

- **Four shipped shapes have more than 65,535 UV entries**: `city ruins` (77,669), `level07`
  (70,764), `level15` (68,358) and `level12` (65,663), each storing one entry per polygon in
  document order. Reading those with the nibble reproduces `uv_index == polygon index` for
  **282,412 of their 282,454 polygons**; ignoring it reproduces 262,118, because every index
  past 65,535 wraps *into range* and silently picks another polygon's UVs. That is the failure
  mode to watch for - it does not look like an error anywhere.
- **In every other shape the nibble is not an index.** It takes all fifteen values with no
  pattern (0x3 on 78,643 polygons, 0x8 on 50,160, 0xe on 45,949), folding it in puts 244,763
  indices out of range against 12,097 without it, and **99.05% of the polygons carrying it have
  no usable UV entry either way** - they are the untextured junk that fills the `_shadow`
  meshes.

So the rule is a property of the *shape*, not of the polygon: a table that cannot be addressed
in 16 bits is read with the nibble, one that can is not. Both are decidable from the file. (An
earlier revision of this document said the extended encoding was simply unused, which is right
for 9,353 of the 9,357 shapes and wrong for the four biggest.)

### Struct + String Chunks

Body contains a fixed struct followed by (or interspersed with) null-terminated strings.

| Chunk ID   | Purpose                    | Address    | Body Format |
|------------|----------------------------|------------|-------------|
| `LTSETHDR` | Light set header           | 0x005d2ab0 | 8-char name (strncpy, not null-term) |
| `INDSOUND` | Indexed sound              | 0x005cecf0 | int32 index + string wav_name + additional data |
| `TRAKSOUN` | Track sound                | 0x005b3d70 | 6 x int32 (24 bytes) + string name |
| `SOUNDOB2` | Sound object v2            | 0x005ce360 | 3 x int32 (12 bytes) + string name (or empty if null) |
| `SHPVTINT` | Shape vertex intensities   | 0x005d2df0 | **16-byte header then one int32 per vertex.** A child of `RBOBJECT`, never of a shape - all 4,668 of them - so it is per-*object* vertex lighting, not shape data. AvP calls it `Shape_Vertex_Intensities_Chunk` and keeps it in `LTCHUNK.CPP` with the lighting; `Projload.cpp` looks it up on the object. The array length matches the vertex count of the **id-paired** shape (see `OBJHEAD1`) in 4,666 of 4,668 cases - which is what independently confirms both this layout and the pairing rule |
| `CTUSRDAT` | Cutscene user data         | 0x005d8500 | string name + (padded to 4-byte boundary) + additional bytes |

### Complex Struct Chunks

These chunks have multi-field bodies that don't fit simple patterns.

| Chunk ID   | Purpose                    | Address    | Body Format |
|------------|----------------------------|------------|-------------|
| `ENDTHEAD` | Environment data header    | 0x005ca340 | `flags` + `lock_user[16]` + `version_no`, 24 bytes. AvP's `Environment_Data_Header_Chunk` - the same `flags`/`lock_user`/`version_no` idiom as `OBJHEAD1` and `SHPHEAD1` |
| `ANIFRADT` | Anim frame data            | 0x005b7860 | 1 x int32 + 4 x int32 (at offsets 5-8 from source) |
| `ANISEQDT` | Anim sequence data         | 0x005b7500 | 8 x int32 (32 bytes) |
| `OBJNOTES` | Object notes               | 0x005b30a0 | Raw text, NUL-terminated and padded. Always inside an `OBINTDT`, never at the top level |
| `CONSHAPE` | Connection shape           | 0x005b7c40 | Minimal data (container-like structure with no actual data) |
| `SUBSHPHD` | Sub-shape header           | 0x005b6860 | References parent shape info. Default index = -1. |
| `DUMOBJTX` | Dummy object transform     | 0x005d2540 | Delegated to internal function FUN_005d2700 |
| `MATCHIMG` | Match image data           | 0x005d11f0 | Linked list + byte flag + additional fields |
| `RANTEXID` | Random texture ID          | 0x005ca750 | String + linked list structure |

### Large Fixed Structs

These chunks copy many consecutive fields from the stream.

#### `OBJHEAD1` - Object Header (0x005b2cd0)

Contains object metadata, position, orientation, and properties.

**Fully decoded.** AvP's `Object_Header_Chunk::fill_data_block` (`win95/OBCHUNK.CPP`)
is the authoritative write order and every offset matches Gunlok exactly.

| Offset | Size  | Type      | Description                  |
|--------|-------|-----------|------------------------------|
| 0x00   | 4     | int32     | `flags`. Only two values ship: 0x400 (8,810 objects) and 0xFC00 (503 - the same count as `AVPSTRAT`) |
| 0x04   | 16    | char[16]  | `lock_user` - **the editor's lock holder, not the name.** `Player` in 6,383 objects, uninitialised junk in 2,783 |
| 0x14   | 12    | int32[3]  | `location`, rif units        |
| 0x20   | 16    | float[4]  | `orientation` quaternion (x, y, z, w); unit in all 9,313 |
| 0x30   | 4     | int32     | `index_num` - unique within the file, in all 563 |
| 0x34   | 4     | int32     | `version_no` - AvP bumps it per save; 421 distinct values ship |
| 0x38   | 4     | int32     | `shape_id_no` - the id of the `REBSHAPE` this object draws |
| 0x3c   | ...   | string    | `o_name` - **the object's name**, NUL-terminated, padded to a 4-byte boundary |

**The name is the trailing string, not the field at 0x04.** `len(body) == 0x3c +
padded name` holds for **all 9,313 shipped objects**, which is exactly what the
64..96 byte size range is. Reading 0x04 instead yields `Player` or mojibake and
hides every real name: `Head`, `Waist`, `Ribs`, `Chest`, `Foot Right`,
`Upper Arm Right`, `Index Right A`.

**An object finds its shape by id, never by position.** AvP's
`Object_Chunk::assoc_with_shape_no` (`win95/OBCHUNK.CPP`) walks the file's shapes
comparing `hdptr->shape_id_no == shphd->file_id_num`, and the same pair carries it
in Gunlok: **`OBJHEAD1+0x38` against `SHPHEAD1+0x14`**. Measured over all 563 files,
that resolves **all 9,313 objects with no shape claimed twice**, and it **differs
from document order in 86 files (15.3%)** - so pairing the two lists positionally
silently attaches geometry to the wrong transform.

Two independent checks agree with it: the orientation at 0x20 is a unit quaternion
whose identity case reads `(0, 0, 0, 1)`, and `SHPVTINT`'s per-vertex array length
matches the **id-paired** shape's vertex count in 4,666 of 4,668 cases.

#### `DUMOBJDT` - Dummy Object Data (0x005d22c0)

**Fully decoded**, from AvP's `Dummy_Object_Data_Chunk` (`win95/DummyObjectChunk.cpp`).

| Offset | Size  | Type      | Description                  |
|--------|-------|-----------|------------------------------|
| 0x00   | 12    | int32[3]  | `location`, rif units        |
| 0x0C   | 12    | int32[3]  | `min_extents`                |
| 0x18   | 12    | int32[3]  | `max_extents`                |
| 0x24   | 16    | float[4]  | `orientation` quaternion; unit in all 6,847 |
| 0x34   | ...   | string    | `name`, NUL-terminated, padded with AvP's `(strlen + 4) & ~3` |

`len(body) == 0x34 + padded name` for all 6,847 shipped dummies, which is what the
60..80 byte size range is. The name is what an `OBJHIERD` binds to - 1,488
hierarchy nodes animate a dummy rather than an object.

A nice confirmation of the Y-down convention falls out of this: **`min_extents.y >
max_extents.y` in 6,843 of the 6,847**, because with Y increasing downwards the
"minimum" corner is the bottom one, and the bottom has the larger Y.

#### `STDLIGHT` - Standard Light (0x005d2b70)

**Exactly 84 bytes**, all 3,794 of them, and **entirely integers** - there is not a
single float in this chunk. Reading the earlier "float[2] intensity / range" fields as
floats yields denormals and NaNs, which is what gave it away.

| Offset | Size | Type      | Description                  |
|--------|------|-----------|------------------------------|
| 0x00   | 4    | int32     | Light id / type (0..1105)    |
| 0x04   | 12   | int32[3]  | Position, rif units          |
| 0x10   | 36   | int32[9]  | 3x3 orientation matrix, 16.16 fixed point, row major |
| 0x34   | 4    | int32     | 16.16 scalar, 0.2 .. 2.0 - brightness |
| 0x38   | 4    | int32     | 79 .. 2044                   |
| 0x3c   | 4    | int32     | Range / radius, rif units (3000 .. 357300) |
| 0x40   | 4    | uint32    | Colour, `0x00RRGGBB`         |
| 0x44   | 4    | int32     | Flags - only ever 3 or 7     |
| 0x48   | 4    | int32     | Always 1                     |
| 0x4c   | 8    | int32[2]  | Always zero                  |

The two load-bearing claims are measured: the nine dwords at 0x10 form an
**orthonormal 3x3 matrix once divided by 65536, in 100.00% of all 3,794 lights**
(rows unit length and mutually perpendicular to within 2e-3), and every value of the
0x40 field is `<= 0xFFFFFF` across its 320 distinct values, with the palette reading
as light colours (`#FFFFFF`, `#FFFFBF`, `#FFFF57`, `#FF6600`).

#### `PLOBJLIT` - Placed Object Light (0x005d2c80)

Large struct (~60+ bytes) with position, color, and attenuation data.

#### `OBASEQFR` - OB Anim Sequence Frame (0x005cd470)

One keyframe of an object-hierarchy animation: a rotation, a position and a time.
The body is **always exactly 44 bytes** - all 323,334 of them across the 563 shipped
files - so there is no sub-frame array and no variable tail.

| Offset | Size | Type      | Description                  |
|--------|------|-----------|------------------------------|
| 0x00   | 16   | float[4]  | Rotation quaternion (x, y, z, w) |
| 0x10   | 12   | int32[3]  | Position, rif units          |
| 0x1c   | 4    | int32     | Normalized time, 16.16 fixed point (65536 = end of sequence) |
| 0x20   | 4    | int32     | Frame index within the sequence |
| 0x24   | 4    | int32     | Flags - only the top byte (0x27) is ever non-zero |
| 0x28   | 4    | int32     | Always zero                  |

Each field is measured, not inferred:

- The first four floats are a **unit quaternion in 100.000% of the 323,334 frames**
  (`|q| - 1` under 1e-3).
- `0x1c` is **non-decreasing across every one of the 28,577 sequences** and is **0 on
  the first frame of all of them**, with a maximum of 65530. It is a position within
  the sequence, not a duration: for evenly-spaced sequences the last frame lands on
  `(N-1)/N * 65536` (31 frames -> 63421, 81 -> 64726, 12 -> 60074, each within 3).
  The real duration is the sibling `OBASEQTM`, in milliseconds.
- `0x20` counts frames: the value 0 occurs exactly 28,577 times, once per sequence.
- `0x24` is 0 in 313,130 frames and `0x80000000` in 9,666; the low three bytes are
  zero in all but 11. Treat it as a byte of flags at 0x27.
- `0x28` is zero in every frame.

An animation is bound to a hierarchy node structurally, not by index: `OBASEQFR` only
ever appears under `OBJCHIER/.../OBANSEQS/OBANSEQC`, never under `REBSHAPE`. There is
no vertex-morph animation in the shipped assets - `ANIMSEQU`/`ANIMFRAM` do not occur.

#### `SHPHEAD1` - Shape Header (0x005b8b30)

**Fully decoded**, from AvP's `Shape_Header_Chunk::fill_data_block`. The 68-byte
minimum body size is exactly this header with an empty name list.

| Offset | Size  | Type      | Description                  |
|--------|-------|-----------|------------------------------|
| 0x00   | 4     | int32     | `flags`                      |
| 0x04   | 16    | char[16]  | `lock_user` - the editor's lock holder |
| 0x14   | 4     | int32     | **`file_id_num`** - the shape's id, what `OBJHEAD1+0x38` matches against |
| 0x18   | 4     | int32     | `num_verts` - matches the `SHPRAWVT` count in all 9,357 shapes |
| 0x1c   | 4     | int32     | `num_polys` - matches the `SHPPOLYS` count in all 9,357 |
| 0x20   | 4     | float     | `radius`                     |
| 0x24   | 24    | int32[6]  | bounds, interleaved `max.x, min.x, max.y, min.y, max.z, min.z` |
| 0x3c   | 4     | int32     | `version_no`                 |
| 0x40   | 4     | int32     | count of associated object names |
| 0x44   | ...   | strings   | that many NUL-terminated names, packed |

Note the bounds are **max before min on each axis**, which is easy to get backwards.

**Everything from 0x18 to 0x3b is derived from the geometry, so it can go stale.**
AvP's from-buffer constructor reads all six values straight into `shape_data`, and
Gunlok derives a role's collision extents from a shape's bounds whenever the GLS
gives `radius`/`height` as 0 - so a tool that edits a mesh has to rewrite this
chunk, not carry it. Recomputing them from `SHPRAWVT`/`SHPPOLYS` reproduces the
stored `num_verts`, `num_polys` and both bound corners in **9,357/9,357** shipped
shapes. `radius` is the exception at 42% bit-exact, drifting a median 7e-7
relative (p90 5e-4, four shapes past 1%); it is the same quantity `SHPCENTR`
holds and the two are **byte-identical in all 9,244 shapes carrying both**, so
the pair has to be regenerated together or not at all. `blender/tests/test_heads.py`
is the measurement.

#### `OBASEQHD` - OB Anim Sequence Header (0x005cd610)

**Fully decoded**, from AvP's
`Object_Animation_Sequence_Header_Chunk::fill_data_block`.

| Offset | Size  | Type      | Description                  |
|--------|-------|-----------|------------------------------|
| 0x00   | 4     | int32     | `num_frames` - **always 65536** in Gunlok, i.e. 1.0 in 16.16, matching the normalized `OBASEQFR` time rather than a count |
| 0x04   | 4     | int32     | `sequence_number` - always 0 |
| 0x08   | 4     | int32     | `sub_sequence_number` - **a per-file sequence id**, see below |
| 0x0c   | 4     | int32     | `num_extra_data` - always 0  |
| 0x10   | n*4   | int32[]   | `extra_data`, absent in every shipped file |
| 0x10   | ...   | string    | `sequence_name`, padded with `(strlen + 4) & ~3` |

`len(body) == 0x10 + padded name` holds for **all 29,550**, which is what pins the
name to 0x10. Every name begins `Dz` - that is a convention in the source assets,
not a field, so the sequences really are called `DzSeq_Stand`, `DzSeq_Walk`,
`DzSeq_Die`. (An earlier revision of this file called `Dz` a constant 2-byte field
and put the name at 0x12; the size arithmetic only works from 0x10.)

**973 of the 29,550 sequences carry no `OBASEQFR` at all**, so an empty animation is
normal data rather than a parse failure.

**`sub_sequence_number` is the id that matches one sequence across the skeleton.**
It is distinct among the sequences of each of the 4,270 `OBANSEQS` nodes, and
identical across nodes for the same sequence name in **all 912 (file, name) pairs
with no exceptions** - so every node's copy of `DzSeq_Walk` carries one number.
Anything generating a new sequence has to allocate a fresh one and use it on
every node, not write 0. `sequence_number` (0x04) is 0 everywhere and appears to
be unused.

**A frame's `time` (`OBASEQFR+0x1c`) is authored, not derived.** It is a position
in the 0..`num_frames` span this header declares, but no formula reproduces the
shipped values: only 3,712 of the 27,731 non-trivial sequences match
`floor(k * 65536 / n)` and **none** match `k * 65536 / (n-1)`. The first frame is
at 0 in all 28,577 monotonic sequences; the last is wherever the animator left it,
never reaching 65536 (the largest observed is 65530). `frame_index`
(`OBASEQFR+0x20`) *is* derivable - it equals the frame's position in the list in
**323,334 of 323,334**.

**`OBASEQFR.flags` (0x24) is a sound index, and it resolves entirely inside the
`.rif`.** AvP's `animobs.hpp` splits the word with
`HierarchyFrame_SoundIndexMask 0x7f000000` (`get_sound_index()` is `>> 24`) and
`HierarchyFrame_FlagMask 0x00ffffff`, and both hold in Gunlok: the low 24 bits are
zero in 323,323 of 323,334 frames, and the sound index is 0 in 322,796 with values
1..17 in the 538 frames that trigger one. `OBASEQFR+0x28` is AvP's
`num_extra_data`, zero in every shipped frame, which is why the body is a fixed
44 bytes.

The resolution is `MakeHierarchyFrame` @ 0x005ae510, reached as
`GetHierarchy` -> `BuildHierarchy` -> `BuildHierarchyNode` -> `BuildSequenceList`
-> `BuildSequence`, every step threading the **rif file object** through in EDX
purely so this lookup can happen:

```
005ae75b  TEST EAX,0x7f000000                     ; any sound on this frame?
005ae760  JZ   ...                                ; no
005ae769  SAR  EAX,0x18
005ae76c  AND  EAX,0x7f                           ; 0..127
005ae772  MOV  EAX,[ECX + EAX*0x4 + 0x10]         ; rif->sounds[index]
005ae77b  JZ   ...                                ; empty slot -> silently skipped
005ae792  MOV  EAX,[EAX + 0x2c]                   ; the entry's path string
005ae799  CALL strchr(path, '\')                  ; +1 -> basename
```

**Bit 31 marks the sequence's origin frame**, and it is not in either AvP mask.
Set in 9,693 shipped frames. It is load-bearing, not decorative - it decides the
frame every other frame in the sequence is expressed relative to:

1. `MakeHierarchyFrame` @ 0x005ae74f: a set sign bit puts `4` into the built
   frame's own flags at `+0x24`.
2. `BuildSequence` @ 0x005ae037 scans the sequence for the **first** frame
   carrying that, sets flag `0x4` on the sequence itself (`+0x14`), and
   **rebases every frame onto it** - subtracting its position (the `SUBSS` loop
   at 0x005ae0c0) then rotating by the conjugate of its rotation
   (`XOR EAX,0x80000000` on `w` at 0x005ae06d).
3. `OffsetSequenceFrames` @ 0x005945b0 then **skips** any sequence so flagged,
   where it would otherwise offset every frame's position by a vector.

So a sequence with an origin frame is stored in that frame's local space and the
engine leaves it alone afterwards; one without is stored absolutely and gets
offset at use. Marking the wrong frame silently re-anchors the whole animation.

#### `OBASEQTM` / `OBASEQFL` / `OBASEQSP` - a sequence's optional settings

All three are AvP's (`animobs.cpp`, `animobs.hpp`) and all three are **optional**:
590, 722 and 582 of the 29,550 shipped sequences carry one. Their accessors are
`get_sequence_time` / `get_sequence_flags` / `get_sequence_speed`, each a
`lookup_single_child` on one `OBANSEQC`.

| Chunk | Layout | Shipped values |
|-------|--------|----------------|
| `OBASEQTM` | `int32 sequence_time` | milliseconds; 45 distinct, all round (1000 in 165, 2000 in 55, 1500 in 37) |
| `OBASEQSP` | `int32 sequence_speed` (mm/s), `int32 angle` (degrees), `int32 spare` | speed 1400..3000 - walking and running pace; **`angle` and `spare` are 0 in all 582** |
| `OBASEQFL` | `int32 flags` | exactly four values ship: `0x4`, `0x8`, `0x84`, `0x88` |

`OBASEQFL`'s bits, from AvP: `MummySequenceFlag_UpperSequence` 0x01 and
`_LowerSequence` 0x02 (neither used here), `SequenceFlag_Loops` **0x04**,
`SequenceFlag_NoLoop` **0x08**, `SequenceFlag_NoInterpolation` 0x10,
`SequenceFlag_HalfFrameRate` 0x20. So Gunlok uses it as loop-or-not plus a
**0x80 that is not in AvP's list**, carried by 181 chunks and unexplained. The two
loop bits are never set together.

**Each is stored on a subset of the bones that have the sequence, and is
*nearly* - but not quite - a per-sequence value.** For 908 of the 912
(file, sequence) pairs carrying an `OBASEQTM`, every bone that has one has the
same value. The exceptions are real: `game_cursor.RIF`'s `DzSeq_Walk` carries
800, 600 and 1000 on three different bones, `Binary Laser MkI.RIF` and
`skyburn.RIF` disagree the same way, and `warflash.RIF` has `DzSeq_Die` and
`DzSeq_Fire` flagged `Loops` on one bone and `NoLoop` on another. `OBASEQSP`
never disagrees.

So a tool must treat the per-bone value as authoritative. It must also remember
*which* bones carried a chunk, or a file that shipped a duration on three bones
of eighty comes back carrying it on all eighty.

(An earlier revision of this entry claimed these were per-sequence outright. The
measurement behind it counted "the bones disagree" and "at least one bone lacks
it" and got the same number - which does not imply the *present* values agree.
A scene round-trip over all 563 files is what caught it.)

They sit after the frames, and where after them varies (position 3, 12, 22, 32,
33, 34, 35, 102, ... - it tracks the frame count). When all three are present the
order is `OBASEQFL`, `OBASEQTM`, `OBASEQSP`.

#### `INDSOUND` - Indexed Sound (`Indexed_Sound_Chunk` @ 0x005cecf0)

The table the index above selects from. `BuildRifFileObject` @ 0x005a9b50
`memset`s a **128-entry pointer array at rif+0x10** (0x200 bytes, matching the
0x7f mask exactly) and installs every `INDSOUND` at `rif->sounds[chunk->index]` -
the chunk declares its own slot rather than taking document order.

**They are direct children of the file root**, not of `REBENVDT`: the
`lookup_child` at 0x005a9c40 is on `rif+0xc`, the root `File_Chunk`, and all 240
shipped chunks sit at depth 1 under `REBINFF2`. (An earlier revision of this
entry said `REBENVDT`, inferred from the `lookup_single_child("REBENVDT")` a few
instructions earlier in the same function. Adjacent code is not evidence.)

Body layout, all 240 shipped chunks across 52 files decoding cleanly:

| Offset | Size | Type | Description |
|--------|------|------|-------------|
| 0x00 | 4 | int32 | `index` - 1..17 in the shipped set; the array holds 128 |
| 0x04 | ... | string | path, e.g. `Robots\GL_click08.wav`, NUL-terminated and padded to 4 with **uninitialised junk** (0xcd) |
| +0 | 4 | int32 | min distance, mm - 5000 in 231, 20000 in 9 |
| +4 | 4 | int32 | max distance, mm - 40000 in 237, 10000 in 2, 50000 in 1 |
| +8 | 4 | int32 | volume, 0..127 |
| +12 | 4 | int32 | pitch offset? 0 in 225, else -640 / 512 / -1280 |
| +16 | 8 | int32[2] | always zero |

The runtime object is `body + 0x28`, which is why `MakeHierarchyFrame` reads the
index at `+0x28` and the path at `+0x2c`.

**A dangling index is normal shipped data, not corruption.** 12 of the 52 files
with sound events reference at least one index no `INDSOUND` declares -
`vlowhark.RIF` uses 1/2/3 and declares none at all - and the `JZ` on the null
slot is what makes that silent. Every used index resolves in only 40 of the 52.
`SOUNDDIR` (19 files) is *not* part of this path: the `INDSOUND` path already
carries its own folder, and nothing looks the `SOUNDDIR` chunk up by id.

#### `ALTLOCAT` - Alternate Location (0x005b4130)

| Offset | Size  | Type    | Description                  |
|--------|-------|---------|------------------------------|
| 0x00   | 4     | int32   | Field 0                      |
| 0x04   | 4     | int32   | Field 1                      |
| 0x08   | 4     | int32   | Entry count                  |
| 0x0C   | n*36  | entry[] | Array of 36-byte entries     |

#### `ADJMDLEP` - Adjacent Module Entry Points (0x005b3580)

| Offset | Size  | Type    | Description                  |
|--------|-------|---------|------------------------------|
| 0x00   | 4     | int32   | Count                        |
| 0x04   | n*var | entry[] | Variable-size entries (linked list elements) |

### Linked List / Complex Chunks

These chunks build internal linked lists during loading:

| Chunk ID   | Purpose                    | Address    |
|------------|----------------------------|------------|
| `FRMMORPH` | Frame morph data           | 0x005ba0e0 |
| `HSETCOLL` | Hierarchy set collection   | 0x005cbe90 |
| `OBHALTSH` | OB hierarchy alt shape     | 0x005cb7e0 |
| `CUTEVENT` | Cutscene event             | 0x005d9400 |
| `SHPHEAD1` | Shape header v1 - **fully decoded**, see below | 0x005b8b30 |
| `SHPPCINF` | Shape poly change info - AvP's `Shape_Poly_Change_Info_Chunk`. **Optional**: 681 of the 9,357 shipped shapes have none, and every AvP path guards on `lookup_single_child` returning null, so a writer may omit it | 0x005ba7b0 |
| `BMPMD5ID` | Bitmap MD5 ID              | 0x005d15c0 |

### Level of detail: the `L<n>#` object-name convention

Detail levels are not a chunk. They are a **naming convention on object names**: an
`RBOBJECT` or `DUMMYOBJ` called `L<n>#<base part name>`, `n` in `1`..`9`, is a variant
of the part called `<base part name>` in the same file. 1,518 of them across 38 of the
563 shipped files; only levels 5 and 7 are ever authored.

Gunlok's `BuildObjectLodChain` @ 0x005b2910 builds the 10-slot chain for one part, and
matches AvP's `Projload.cpp` (`ob_name[0]=='L' && ob_name[1] && ob_name[2]=='#'`, level
`o_name[1]-'0'`, base `strcmp(&o_name[3], base_name)`). Two passes with different
meanings:

- an **`RBOBJECT`** variant supplies a *replacement shape* at level `name[1]-'0'`;
- a **`DUMMYOBJ`** variant sets a *cull cutoff* at `name[1]-'1'` — the part is not drawn
  at that level or beyond. Elint MkII culls its finger joints, neck pistons and right
  hand this way.

A level with neither entry inherits the previous level's shape. The switch distances are
the file-root `HIDEGDIS`.

Three things a reader/writer has to know:

- **The match is byte-exact**, so the four typo'd variants in `Skorn MkII.RIF`
  (`L5#Face Pipe RIght`, `L5#Face Pipe left`) are dead in-game. 32 variants name a base
  object that does not exist and 9 name one no hierarchy node binds; both are legal and
  simply never selected.
- **No `OBJCHIER` node binds a variant.** The engine reaches it through the base's name,
  so a variant has no hierarchy node and no animation of its own — it is drawn at the
  transform of the *base part's* node.
- **A variant's own `OBJHEAD1`/`DUMOBJDT` placement is dead data.** The shipped sets are
  parked beside the model (Gunlok MkII's L5 set at +1200 on X, its L7 at -1200), yet the
  vertices are already in the base part's local frame: across all 1,258 variant/base mesh
  pairs the local-frame centroid distance has a median of 35 rif units against a
  ~2,000-unit-tall character, and applying the stored placement *raises* it to a median
  of 115 (p90 1,202). That is what AvP's loader implies by keeping only the shape
  (`deg_ptr->shape = low_detail_array[i]`) and discarding the object.

`destructorfrag.RIF` is the trap: nine of its parts are genuinely *named* `L7#head`,
`L7#pelvis` and so on with no base object, and its hierarchy binds those names directly.
They are ordinary parts that happen to match the pattern, so anything acting on the
convention has to require that the base exists and the variant is itself unbound.

### Version/Name Management Chunks

These chunks perform lookups and management rather than storing simple data:

| Chunk ID   | Purpose                    | Address    | Notes |
|------------|----------------------------|------------|-------|
| `BMNAMVER` | Bitmap name version        | 0x005cf730 | Manages versioning of bitmap names. Complex deduplication logic. |
| `BMNAMEXT` | Bitmap name extension      | 0x005cfba0 | Extends bitmap name data. Creates if not exists. |
| `SHBMPNAM` | Shape bitmap names         | 0x005d0530 | Bitmap name list for a shape. Linked list of entries. |
| `OBJHIERD` | Object hierarchy data      | 0x005cb1e0 | `{int32 num_extra_data, int32[n] extra_data, name}` - the count is 0 in all 5,250 shipped nodes so the name starts at 4. **The name is the object the node animates**: AvP's `Object_Hierarchy_Data_Chunk::find_object_for_this_section` resolves it with `strcmp` against each object's name. It reaches 3,541 `RBOBJECT` and 1,488 `DUMMYOBJ`; 221 nodes match nothing, which AvP treats as a null object, so a dangling node is legal. Node names are the skeleton - `Waist`, `Thigh Right`, `Shin Right`, `Chest`, `Hand Right`, `Index Right A` |

### Texture Chunks

| Chunk ID   | Purpose                    | Address    | Body Format |
|------------|----------------------------|------------|-------------|
| `BMPNAMES` | **The file's texture table** - fully decoded, see below | 0x005cfed0 | uint32 `count \| version << 16` + per entry a 20-byte record and a padded name |
| `SHPTEXFN` | Shape texture filenames    | 0x005b94d0 | uint32 count + count x string (null-terminated filenames). **Registered but never used** - it does not appear in a single shipped file, which is why the texture index is a `BMPNAMES` lookup |
| `SHPUVCRD` | Shape UV coordinates       | 0x005b9330 | uint32 count + per entry: uint32 num_verts + num_verts x 2 x float (UV pairs), in **texels** - see below. An **indexed list keyed by a polygon's `colour >> 16`**, not one entry per polygon - 139 shapes have fewer entries than polygons, and 534 contain polygons whose index points past the end (concentrated in the untextured `_shadow` meshes). A zero-length entry and an entry of three `(0,0)` pairs are different and both occur |

#### `SHPUVCRD` units: texels, not fractions

**A stored UV is a texel coordinate**, running 0..width and 0..height rather than
0..1, so it only means anything alongside the size of the texture the polygon
names. Three independent things say so:

- **It scales with the texture.** Across 375,000 shipped UV pairs, the 99th
  percentile of `|u|` lands within 7% of the texture's own width for every size
  in the game: 1,095 for the 1024-wide textures, 510 for the 512s, 255 for the
  256s. A fraction would not know the difference.
- **AvP casts the float straight to the int its renderer wants**, which is texel
  space: `chnkload.cpp:2390` does `(int)cshp_ptr->uv_list[UVIndex].vert[j].u`,
  and the sprite path a few lines down builds the same number as
  `f->UVCoords[l][0] << 16` - an integer texel in 16.16.
- **They are whole numbers**: 374,658 of the 376,641 sampled pairs are integral.
  Not all, so a reader must keep them as floats.

**V grows downward**, i.e. `v = 0` is the *top* row of the image, as in Direct3D.
Measured on the 8,916 polygons across the shipped levels that are near-vertical,
show at least 40% of their texture's height, and are mapped axis-aligned (one
edge at constant V and level in the world): **86.3% put the low V at the top of
the wall**, and 16 of the 17 levels with a usable sample lean that way, several
overwhelmingly (`level10` 3,286 to 68, `level01` 114 to 1). The minority is
artists deliberately flipping a texture on a face, which is legal and expected.

Anything mapping these into a bottom-up UV space (Blender, OpenGL) has to divide
by the texture size *and* flip V. Both are exact in float32, because every
texture in the game is a power of two from 8 to 1024.

#### `BMPNAMES` - Global Bitmap Names (AvP's `Chunk_With_BMPs`)

The table a polygon's texture index resolves against. **527 of the 563 shipped files carry
one**, always as a child of the file-level `REBENVDT`, holding 1,601 entries between them.
Layout is AvP's `Chunk_With_BMPs` (`3dc/win95/BMPNAMES.CPP`) verbatim.

`Chunk_With_BMPs_Ctor` @ 0x005cfed0 is the parser (shared with the never-shipped
`BMPLSTST`); `RifLoad_BMPNAMES` @ 0x005d1870 is its loader and `BMP_Name_Ctor`
@ 0x005cf4b0 builds one entry.

| Offset | Size | Type   | Description                  |
|--------|------|--------|------------------------------|
| 0x00   | 4    | uint32 | entry `count` in the low 16 bits, table `version` in the high 16 |
| 0x04   | 4    | uint32 | `flags` - `BMPN_Flags` |
| 0x08   | 4    | int32  | **`index`** - what a polygon's `colour & 0xfff` matches |
| 0x0c   | 4    | int32  | `version_num` in the low 20 bits, `enum_id` in the top 12 |
| 0x10   | 4    | int32  | `priority` - palette-generation priority, PC in the low byte |
| 0x14   | 4    | uint32 | `transparency_colour_union` - see below |
| 0x18   | ...  | string | the name, NUL-terminated and padded to `strlen + (4 - strlen % 4)` |

Five things measured across all 1,601 entries rather than read off the AvP source:

- **`index` is a stable id, not a position.** Entries are stored in *descending* index
  order and the values are sparse - `Maze.RIF` holds `[10, 9, 8, 5, 4, 1]` for six
  entries. Matching a polygon's texture index against it resolves **1,518,963 of the
  1,766,071 shipped polygons**; of the remainder 22,331 are the `0xfff` untextured
  sentinel and 215,517 of the last 224,747 are in `_shadow` files, whose polygons carry
  junk texture *and* UV indices because the meshes are never textured. **No table has a
  duplicate index or a duplicate name.**
- **`flags` is `0x0100010c` in every single entry**: `ChunkBMPFlag_IFF` plus
  `PriorityAndTransparencyAreValid` and both mip-map requests. The IFF flag is the
  load-bearing one - it means the name is a path relative to the textures root and that
  everything about the image lives in the image file.
- **With that flag set, `transparency_colour_union` holds the image size**, width in the
  low 16 bits and height in the high 16 (AvP's `SHIFT_W`/`SHIFT_H`). It agrees with the
  `.RIM`'s own header in 1,580 of the 1,597 entries whose file is on disk, so it is
  authoring output that can go stale rather than a second source of truth.
- **The name is a path relative to `<game>\Graphics`**, in whatever case the artist typed:
  `Units\baddies3.RIM` and `units\baddies3.RIM` both ship and name the same file. Folded
  case-insensitively, 1,597 of the 1,601 resolve against the install; the other four name
  files Gunlok does not ship.
- **The padding after a name is not always NUL.** The writer pads out of uninitialised
  memory - `Units\baddies3.RIM` is followed by `00 f5` - so a reader that regenerates the
  padding cannot rebuild the chunk byte for byte.

`priority` is `6` in every shipped entry, and the binary agrees with the measurement
rather than merely with AvP's header: `BMP_Name_Ctor` @ 0x005cf4b0 initialises a fresh
entry to `flags = 0x10c` and `priority = 6`, which is `DEFAULT_BMPN_FLAGS` and
`DEFAULT_BMPN_PRIORITY`. The `0x0100010c` in the files is those defaults plus the IFF
flag the loader sets.

---

## Chunk Class Hierarchy

```
Chunk (base, 0x24 bytes)
  vtable @ 0x00
  error_code @ 0x04
  identifier_store[9] @ 0x08
  identifier* @ 0x14
  parent* @ 0x18
  next* @ 0x1C
  previous* @ 0x20
  |
  +-- Chunk_With_Children (adds children list @ 0x28)
  |     |
  |     +-- File_Chunk (REBINFF2) - adds filename, object_array, compression handling
  |     +-- Shape_Chunk (REBSHAPE) - adds shape-specific data @ 0x8C
  |     +-- SubShape_Chunk (SUBSHAPE)
  |     +-- Object_Chunk (RBOBJECT) - adds object_data (location, orientation)
  |     +-- (other container types)
  |
  +-- Miscellaneous_Chunk (fallback for unrecognized IDs)
  |     adds: data_size, data_store*, data*
  |
  +-- (all leaf chunk types - each adds specific fields after base)
```

## Key Functions

| Address    | Name                                     | Purpose |
|------------|------------------------------------------|---------|
| 0x005d4980 | `Chunk::Chunk`                           | Base chunk constructor |
| 0x005b5020 | `Chunk_With_Children::Chunk_With_Children`| Container chunk constructor |
| 0x005d4ae0 | `Chunk::Register`                        | Register chunk type in factory table |
| 0x005d51e0 | `Chunk_With_Children::DynCreate`         | Look up factory and create chunk from stream |
| 0x005d4cb0 | `Miscellaneous_Chunk::Miscellaneous_Chunk`| Fallback chunk for unknown IDs |

## Chunk ID Quick Reference

### Containers (24)

| ID         | Description                          |
|------------|--------------------------------------|
| `ANIMFRAM` | Animation frame                      |
| `ANIMSEQU` | Animation sequence                   |
| `ASALTTEX` | Alternate shape texture set          |
| `CUTSCUSR` | Cutscene user data                   |
| `CUTSHEAD` | Cutscene header                      |
| `CUTTRACK` | Cutscene track                       |
| `DUMMYOBJ` | Dummy object                         |
| `LIGHTSET` | Light set                            |
| `MODULEDT` | Module data                          |
| `OBANSEQC` | Object animation sequence (single)   |
| `OBANSEQS` | Object animation sequences           |
| `OBJCHIER` | Object hierarchy                     |
| `OBJPRJDT` | Object projectile data               |
| `RBOBJECT` | Rebirth object                       |
| `REBENVDT` | Environment data                     |
| `REBINFF2` | Top-level RIF file                   |
| `REBSHAPE` | Shape definition                     |
| `SHPEXTFL` | Shape external file reference        |
| `SHPFRAGS` | Shape fragments                      |
| `SHPMORPH` | Shape morphing                       |
| `SOUNDEXD` | Sound extended data                  |
| `SPECLOBJ` | Special objects                      |
| `SUBRIFFL` | Sub-RIF file reference               |
| `SUBSHAPE` | Sub-shape definition                 |

### Leaves (79)

| ID         | Category | Description                    |
|------------|----------|--------------------------------|
| `ADJMDLEP` | Complex  | Adjacent module entry points   |
| `ALTLOCAT` | Complex  | Alternate location             |
| `AMBIENCE` | Fixed    | Ambient settings (1 x int32)   |
| `ANIFRADT` | Struct   | Animation frame data           |
| `ANISEQDT` | Struct   | Animation sequence data        |
| `ANSHCEN2` | Vec3     | Anim shape center v2           |
| `BMNAMEXT` | Mgmt     | Bitmap name extension          |
| `BMNAMVER` | Mgmt     | Bitmap name version            |
| `BMPMD5ID` | Complex  | Bitmap MD5 identifier          |
| `BMPNAMES` | Struct   | **The file's texture table**   |
| `CONSHAPE` | Minimal  | Connection shape               |
| `CONSTYPE` | Fixed    | Connection type (1 x int32)    |
| `CTUSNDPR` | Fixed    | Cutscene sound props (6 x int32)|
| `CTUSRDAT` | Mixed    | Cutscene user data (str+struct)|
| `CTUSRHIE` | String   | Cutscene user hierarchy name   |
| `CTUSSPPO` | Array    | Cutscene special points        |
| `CUTEVENT` | Complex  | Cutscene event                 |
| `CUTPOINT` | Array    | Cutscene points                |
| `CUTTRFOV` | Fixed    | Cutscene track FOV (3 x float) |
| `CUTTRNAM` | String   | Cutscene track name            |
| `DUMOBJDT` | Struct   | Dummy object data              |
| `DUMOBJTX` | Complex  | Dummy object transform         |
| `ENDTHEAD` | Struct   | Environment data header        |
| `ENVACOUS` | Fixed    | Environment acoustics          |
| `ENVSDSCL` | Fixed    | Environment sound scale        |
| `EXTOBJNM` | String   | External object name           |
| `FRAGDATA` | Fixed    | Fragment data (4 x int32)      |
| `FRAGLOCN` | Vec3     | Fragment location              |
| `FRMMORPH` | Complex  | Frame morph data               |
| `HIDEGDIS` | Array    | Hierarchy degradation distances |
| `HIERBBOX` | Vec3     | Hierarchy bounding box         |
| `HSETCOLL` | Complex  | Hierarchy set collection       |
| `INDSOUND` | Mixed    | Indexed sound (index + wav)    |
| `LITSCALE` | Fixed    | Light scale (1 x float)        |
| `LTSETHDR` | Struct   | Light set header (8-char name) |
| `MATCHIMG` | Complex  | Match image data               |
| `MODACOUS` | Fixed    | Module acoustics               |
| `MODFLAGS` | Fixed    | Module flags                   |
| `OBANALLS` | Array    | OB anim all sequences          |
| `OBASEQFL` | Fixed    | OB anim seq flags (1 x int32)  |
| `OBASEQFR` | Complex  | OB anim sequence frame         |
| `OBASEQHD` | Complex  | OB anim sequence header        |
| `OBASEQSP` | Fixed    | OB anim seq speed (3 x int32)  |
| `OBASEQTM` | Fixed    | OB anim seq time (1 x int32)   |
| `OBHALTSH` | Complex  | OB hierarchy alt shape         |
| `OBHIERNM` | String   | Hierarchy node name            |
| `OBJHEAD1` | Struct   | Object header v1               |
| `OBJHIERD` | Complex  | Object hierarchy data          |
| `OBJNOTES` | Raw      | Object notes (raw bytes)       |
| `OBJTRAK2` | Array    | Object track data v2           |
| `PLOBJLIT` | Struct   | Placed object light            |
| `RANTEXID` | String   | Random texture ID              |
| `RIFFNAME` | String   | Object/asset name              |
| `RIFVERIN` | Fixed    | RIF version (1 x int32)        |
| `SHBMPNAM` | Complex  | Shape bitmap names             |
| `SHPCENTR` | Vec3     | Shape center (4 x float)       |
| `SHPEXTFN` | String   | External shape filename        |
| `SHPFLAGS` | Fixed    | Shape flags (1 x uint32)       |
| `SHPFNAME` | String   | Shape filename                 |
| `SHPFRGTP` | String   | Shape fragment type name       |
| `SHPHEAD1` | Complex  | Shape header v1                |
| `SHPMRGDT` | Array    | Shape merge data               |
| `SHPPCINF` | Complex  | Shape PC info                  |
| `SHPPNORM` | Array    | Shape polygon normals (Vec3[]) |
| `SHPPOLYS` | Array    | Shape polygons (36-byte[])     |
| `SHPPRPRO` | Array    | Shape preprocessed data        |
| `SHPRAWVT` | Array    | Shape raw vertices (Vec3[])    |
| `SHPTEXFN` | Array    | Shape texture filenames        |
| `SHPUVCRD` | Complex  | Shape UV coordinates           |
| `SHPVFLAG` | Array    | Shape vertex flags (uint32[])  |
| `SHPVNORM` | Array    | Shape vertex normals (Vec3[])  |
| `SHPVTINT` | Mixed    | Shape vertex tint              |
| `SOUNDDIR` | String   | Sound directory path           |
| `SOUNDNAM` | String   | Sound file name                |
| `SOUNDOB2` | Mixed    | Sound object v2 (struct + str) |
| `STDLIGHT` | Struct   | Standard light (~68+ bytes)    |
| `SUBSHPHD` | Struct   | Sub-shape header               |
| `TRAKSOUN` | Mixed    | Track sound (6 x int32 + str)  |
| `TRSNDCAT` | String   | Track sound category name      |
| `VMDARRAY` | Array    | VMD array data                 |

## Chunks Not in Binary

The following chunk IDs are referenced in the RIF viewer (`rifviewer/main.js`) as containers but do **not** exist in the Gunlok binary. They are from the shared Rebellion `3dc` chunk library — all five are present in the AvP source, so the earlier "general Rebellion RIF SDK used by other games" guess was right:

| Chunk ID   | AvP class                  | AvP source        |
|------------|----------------------------|-------------------|
| `SPRIHEAD` | `Sprite_Header_Chunk`      | `Sprchunk.cpp`    |
| `SPRITEPC` | `PC_Sprite_Chunk`          | `Sprchunk.cpp`    |
| `SPRITEPS` | `Playstation_Sprite_Chunk` | `Sprchunk.cpp`    |
| `SPRITESA` | `Saturn_Sprite_Chunk`      | `Sprchunk.cpp`    |
| `FRAGTYPE` | `Fragment_Type_Chunk`      | `fragchnk.hpp`    |

Gunlok dropped the sprite family (AvP's 2D sprite path) and the `FRAGTYPE` container, keeping
only `FRAGDATA`/`FRAGLOCN`.

---

## The texture images: `.RIM`

A `BMPNAMES` entry names a `.RIM` under `<game>\Graphics` (`Units\baddies3.RIM` ->
`Graphics\Units\baddies3.RIM`), 513 of which ship. **A `.RIM` is not a RIF chunk file.**
It is ordinary **IFF**: 4-character ids, **big-endian** sizes, odd bodies padded to an
even boundary, and `LIST`/`FORM`/`PROP`/`CAT ` group chunks whose body opens with a
4-character type. Every one of those four things is the opposite of the container above,
so nothing in `rif.py` reads one.

The shape is the same in all 513, and the image chunk is one of two things:

```
LIST:ILBM
  PROP:ILBM         TRAN            (shared properties; also ALPH, in one file)
  FORM:ILBM         BMHD + S3TC          <- a DXT image
                    BMHD + CMAP + BODY   <- ... or a palettized one
  LIST:MIPM
    FORM:MIPM       CONT (level count), FLAG
    LIST:ILBM       FORM:ILBM per mip level
```

**`PROP:ILBM` holds only `TRAN`** - in all 513 files, and `ALPH` beside it in
`Ground\tree_alpha.RIM`. It never holds a `BMHD`: every `BMHD` sits inside a `FORM:ILBM`
next to the image it describes. (An earlier revision of this section said
"`BMHD` (shared properties) + `TRAN`", which is wrong.)

`CONT` is the mip-level count and is present in all 513; `0` (no mip chain) ships and works,
because the mip walk in `RimOpenAndScan` is skipped when `CONT` is absent or zero. `FLAG` is
in 328 of them and its purpose has not been determined. `GRAB` is registered by the binary
but appears in no shipped file.

### Which image chunk wins: `BODY` first, `S3TC` second

`RimOpenAndScan` @ 0x005dd6b0 enumerates `ILBM`->**`BODY`** first, and only when that finds
nothing does it enumerate `ILBM`->`S3TC` and set the "is DXT" flag at `RimImage+0x81`. So a
palettized image takes priority, and a file carrying both would have its `S3TC` ignored.
**Uncompressed `.RIM` is a first-class path, not a fallback** - which is what makes a `.RIM`
writer possible without a DXT compressor.

Of the 513 shipped files, **490 are S3TC with no `BODY`, 23 are `BODY`+`CMAP` with no
`S3TC`, and none have both**. The 23 are 16 `*_fmv_*` ground textures and seven others -
`Bitmaps\MPLAY_maze`, `Ground\tree_alpha`, `Ground\tree_bark`, `Structures\EOG_cylinder`,
`Units\Command Wheel 01`, `Units\english load save`, `Units\save screen`. So this is not a
quirk of one art pipeline for the FMV ground: it is used for UI, unit and structure textures
too.

`BMHD` is the standard 20-byte ILBM header, read field for field by `BmhdChunk_Serialize`
@ 0x005e0410. **`nPlanes` means different things on the two paths**: on the `BODY` path it is
a genuine bitplane count and `nPlanes == ceil(log2(CMAP entries))` in all 77 palettized
images; on the `S3TC` path it is never read at all, which is why it holds nonsense there (a
256x256 DXT3 font declares 3, a 512x512 DXT1 declares 17). The four-character code inside
`S3TC` is what says how to decode a DXT payload.

### The IFF chunk classes the binary registers

`IffChunk_Register` @ 0x005e1d00 is the IFF analogue of `Chunk::Register` @ 0x005d4ae0, with
its own 13 static-init constructors at 0x0043c5f0..0x0043c7f0 (same
`PUSH pfnCreate / MOV ECX,id / CALL` shape as the RIF ones, so they are invisible to an xref
search until that range is disassembled).

| Id | Parent | Create | Class size | Purpose |
|----|--------|--------|-----------|---------|
| `BMHD` | `ILBM` | 0x005e0c90 | 0x28 | the 20-byte ILBM BitMapHeader |
| `CMAP` | `ILBM` | 0x005e0ce0 | 0x1c | palette, 3 bytes RGB per entry |
| `BODY` | `ILBM` | 0x005e0d30 | 0x44 | planar bitplane pixels |
| `GRAB` | `ILBM` | 0x005e0da0 | - | `{s16 x, s16 y}` hotspot; in no shipped file |
| `ALPH` | `ILBM` | 0x005e1160 | 0x4c | alpha plane |
| `S3TC` | `ILBM` | 0x005e1220 | 0x30 | DXT payload |
| `TRAN` | `ILBM` | 0x005e1110 | - | RGB colour key |
| `CONT` | `MIPM` | 0x005e11d0 | - | mip-level count |
| `FLAG` | `MIPM` | 0x005e1260 | - | not determined |
| `CAT `/`FORM`/`LIST`/`PROP` | (global) | 0x005e2a20/2960/29c0/2a80 | - | the IFF group chunks |

The load path is `AcquireRimTexture` @ 0x005a15b0 (hash + cache only, opens nothing) ->
`RimOpenAndScan` -> `RimBindImageChunks` @ 0x005ddf30 -> `RimConvertRows` @ 0x005ddbc0.
`RimLoadErrorCode` @ 0x00838b0c carries the failure: **8** for a malformed/unsupported image
and **9** for a `BODY` with no `CMAP`.

### `S3TC` - the pixels

A 22-byte header then the raw DXT payload. Measured across all **3,423** S3TC chunks in
the shipped textures: the declared size, the size implied by this header's own dimensions
and the actual body length agree in every one.

| Offset | Size | Type    | Description |
|--------|------|---------|-------------|
| 0x00   | 4    | uint32  | `flags` - zero in all 3,423 |
| 0x04   | 4    | char[4] | the DXT four-character code, **byte-reversed** (`1TXD` = DXT1) |
| 0x08   | 2    | uint16  | `redWeight` = 309 |
| 0x0a   | 2    | uint16  | `blueWeight` = 82 |
| 0x0c   | 2    | uint16  | `greenWeight` = 608 |
| 0x0e   | 2    | uint16  | width, big-endian |
| 0x10   | 2    | uint16  | height, big-endian |
| 0x12   | 4    | uint32  | payload size, big-endian |
| 0x16   | ...  | bytes   | DXT1 or DXT3 blocks, ordinary little-endian S3TC |

The three weights are the compressor's **perceptual channel weights in per-mille** - they sum
to 999, and the ordering (red, blue, green) is what makes 309 / 82 / 608 read as a luma-ish
weighting rather than as noise. They are byte-identical in all 3,423 chunks, and nothing in
the loader reads them, so a writer copies them verbatim. (This document previously recorded
them as "6 identical bytes"; `utils/rimutil/rimutil.cpp`'s `S3tcData` had the field split
right all along.)

**The DXT payload itself is little-endian**, unlike everything around it - the 5:6:5
endpoints decode as normal RGB565. That is settled by content rather than by inspection:
`lava.RIM` decodes to mean RGB `175, 109, 20` (orange, as lava should be), which a
red/blue swap would turn into blue.

3,385 payloads are DXT1 and 38 are DXT3. Of the **365 distinct textures the shipped `.rif`
files actually name**, 361 are DXT1, 3 are DXT3, 4 name files the install does not ship,
and **one is not S3TC at all**: the `*_fmv_*` ground textures store palettized `CMAP`/`BODY`
variants instead, which is 23 of the 513 files - see below.

#### Only DXT1 and DXT3 exist as far as Gunlok is concerned

**Do not write DXT2, DXT4 or DXT5.** Four independent places in the binary agree, and the
failure mode for DXT5 is silent rather than loud, which is why this is worth stating:

- `TextureFormatCandidates` @ 0x006ac348 is a 13-entry list - `R8G8B8` through `X4R4G4B4`,
  then `DXT1`, `DXT3`, terminator. `EnumerateTextureFormats` @ 0x005a4d60 probes only these
  against the device, so DXT5 is never even *asked about*, let alone used.
- `SurfaceDesc_SetCompressedFormat` @ 0x005c6820 is a two-value whitelist that **fails
  silently**: a fourcc that is not DXT1 or DXT3 leaves the field at whatever it held, with no
  error. `SurfaceDesc_IsCompressedFormat` / `_GetCompressedFormat` recognise the same two.
- `PickPreferredTextureFormat` @ 0x005761f0 considers only those two among the compressed
  formats.
- `RimOpenAndScan` tests for `DXT1` *only*, and maps everything else to "8 alpha bits" - i.e.
  it treats an unknown fourcc as DXT3.

**A DXT5 `.RIM` would not be rejected; it would be mis-rendered.** DXT5 and DXT3 share the
same 16-byte block size, so `RimBindImageChunks` derives an identical pitch and bpp from the
payload size and `RimConvertRows` memcpy's the blocks verbatim. The result is a texture whose
RGB is roughly right and whose alpha is garbage - DXT5's interpolated alpha read as DXT3's
explicit 4-bit alpha. No error is raised anywhere on that path.

So the choice is only ever DXT1 vs DXT3: **DXT1** for opaque or 1-bit-alpha (4 bpp, and what
361 of the 365 named textures use), **DXT3** for graded alpha (8 bpp, 3 textures). Note that
`D3DFormatToString` @ 0x005a44b0 *names* all five DXT variants - it is a debug log table, not
a capability list, and `rimutil`'s `case 'DXT5':` in `decompressS3tc` is likewise a decoder
convenience that no shipped file reaches.

#### The fourcc picks the surface format, through the alpha-depth rule

`ChooseSurfaceFormatForImage` @ 0x005c7880 selects the destination surface, and the whole
decision turns on the image's **alpha bits** - `RimImage+0x50`, vtable slot 4, which
`RimOpenAndScan` sets to `(fourcc == 'DXT1') ? 0 : 8`:

| alpha bits | compressed surface accepted | uncompressed fallback must satisfy |
|-----------|------------------------------|------------------------------------|
| `< 2`     | `DXT1` only                  | `alpha_bits <= format.maxAlphaBits`, so R5G6B5, X1R5G5B5, A8R8G8B8 ... all qualify |
| `>= 2`    | `DXT3` only                  | the same test, which among the 13 candidates only **A8R8G8B8** passes |

So the fourcc is *not* discarded after the DXT1 test - it survives twice over, as the 0-or-8
alpha depth that selects DXT1-vs-DXT3, and as the raw argument to
`SurfaceDesc_SetCompressedFormat`, which is where a DXT5 value is silently dropped. The whole
DXT5 failure is now traced end to end: fourcc `DXT5` -> alpha bits 8 -> a **DXT3** surface is
chosen -> `SetCompressedFormat('DXT5')` no-ops so the surface stays DXT3 -> DXT5 blocks are
memcpy'd into it.

**This is also the one respect in which DXT1 is not redundant.** Writing every texture as DXT3
is otherwise free - the RGB block encoding is identical, and an encoder only picks DXT1's
three-colour mode when it needs the 1-bit alpha - but it forces every image to `alpha_bits = 8`,
whose only uncompressed fallback is A8R8G8B8, itself gated on `Use32BitTextures`. With DXT3
compression unavailable *and* 32-bit textures off, such an image can find no format at all and
`DecodeImageToSurface` reports `RimLoadErrorCode = 3`. On hardware that has DXT3 this never
fires, which is why "DXT3 everywhere" is a reasonable choice for a modern target and not a
correctness question.

Not verified: whether the **renderer** keys blending or alpha-testing off a texture's alpha
depth. Every use of `alpha_bits` traced here is format selection, and Gunlok's polygons carry
their own `engine_type`/`flags` (see `SHPPOLYS` above), which is where translucency lives - so
the expectation is that DXT3-vs-DXT1 is a storage decision only. The load path is traced; the
draw path is not.

### `BMHD` - the header

The standard 20-byte ILBM BitMapHeader, unmodified. Offsets are into the chunk body; the
in-memory `BmhdChunk` puts the same fields at body + 0x14. All multi-byte fields are
big-endian, like every size in the container.

| Offset | Size | Type   | Field | Read by the loader? |
|--------|------|--------|-------|---------------------|
| 0x00   | 2    | uint16 | `width` | yes |
| 0x02   | 2    | uint16 | `height` | yes |
| 0x04   | 2    | int16  | `x` | no |
| 0x06   | 2    | int16  | `y` | no |
| 0x08   | 1    | uint8  | `nPlanes` | on the `BODY` path only |
| 0x09   | 1    | uint8  | `masking` | yes - **must be 0 or 2** |
| 0x0a   | 1    | uint8  | `compression` | as the `BODY` codec; see below |
| 0x0b   | 1    | uint8  | `flags` | no |
| 0x0c   | 2    | uint16 | `transparentColor` | when `masking == 2` |
| 0x0e   | 1    | uint8  | `xAspect` | no |
| 0x0f   | 1    | uint8  | `yAspect` | no |
| 0x10   | 2    | uint16 | `pageWidth` | no |
| 0x12   | 2    | uint16 | `pageHeight` | no |

**`compression` is a three-valued discriminator for the whole image, not a boolean**:
`0` = raw planar `BODY`, `1` = ByteRun1 planar `BODY`, `2` = S3TC. Every one of the 3,423 S3TC
chunks declares `2`, and the 77 palettized images declare `0` (41) or `1` (36). Which path
runs is decided by *which chunk is present*, not by this field - nothing in the loader reads it
on the S3TC path - but a writer should still set `2` for an S3TC image to match every shipped
file. `rimutil`'s `enum class Compression { None, RunLength, S3tc }` already had this right.

**`masking` must be 0 (none) or 2 (`transparentColor` is a transparent palette index).**
`RimConvertRows` dispatches on `(has ALPH, masking)` to one of four index->pixel converters
and sets `RimLoadErrorCode = 8` for anything else, so ILBM's mask plane (1) and lasso (3) are
both rejected. Shipped files use 0 everywhere except `Units\Command Wheel 01.RIM`, which uses 2.

### `CMAP` + `BODY` - the palettized path

- **`CMAP` is mandatory**, 3 bytes RGB per entry, any number of entries. A `BODY` with no
  `CMAP` fails with `RimLoadErrorCode = 9`.
- **A `FORM:ILBM` may appear several times, once per palette depth**, and
  `RimBindImageChunks` picks the one with the most `CMAP` entries that is still within the
  caller's colour cap. `Ground\city_fmv_road_1024.RIM` carries 14 planes / 14,105 entries,
  8 / 256 and 4 / 16 of the same 512x512 image; the high-depth variant is how the artists'
  tool stored a near-truecolour image without DXT. The variant set thins out at small mip
  sizes (the 32x32 level of `Command Wheel 01` has only 8- and 4-plane forms).
- **That cap is 0 - meaning no cap - on any true-colour destination**, so the largest palette
  in the file wins. `DecodeImageToSurface` @ 0x005c68c0 passes
  `(dest_is_palettized ? 1 << dest_palette_bits : 0)`, and `RimBindImageChunks` treats 0 as
  unbounded. A high-depth `CMAP` is therefore genuinely reachable rather than dead weight,
  which is what makes the point below possible.

##### `BODY` can carry true colour, at a price

Since the cap is unbounded and `IffBodyDecodeScanline` accumulates into a **uint32** index
(so `nPlanes` up to 31 works), a `BODY` whose `CMAP` holds one entry per distinct colour is an
*exactly lossless* encoding of any image. `nPlanes = ceil(log2(distinct colours))`, and the
shipped set already exercises the high end: `Ground\tree_alpha.RIM` carries 19 planes and
90,319 palette entries.

Two things make this a fallback rather than the obvious choice:

- **The cost scales with distinct colours, and the worst case beats raw RGBA.** A 1024x1024
  image with a unique colour per pixel needs 20 planes: 2.5 MB of indices plus 3 MB of palette,
  against 4 MB for uncompressed 32-bit, 1 MB for DXT3 and 0.5 MB for DXT1. A realistic
  photographic texture at ~200k distinct colours lands around 3 MB. There is **no direct-RGB
  pixel chunk** in the format - the registered vocabulary is `BMHD`/`CMAP`/`BODY`/`GRAB`/
  `ALPH`/`S3TC`/`TRAN` - so palettized is the only non-DXT option there is.
- **It is only *displayed* in true colour if a 32-bit surface is chosen.** The converters in
  `RimConvertRows` write into whatever `ChooseSurfaceFormatForImage` picked; an exact palette
  landing in R5G6B5 is still 16-bit output. The 32-bit candidates are gated on
  `Use32BitTextures`, which is a user setting the file cannot influence.

So `BODY` is the right tool for *losslessness* (and for avoiding a DXT compressor entirely),
not for cheap photographic fidelity.
- **`BODY` is planar, MSB-first** (`IffBodyDecodeScanline` @ 0x005e0a30). One scanline is
  `nPlanes` consecutive plane-rows; plane row *p* contributes bit *p* of each pixel's palette
  index, most significant pixel first within each byte:

  ```
  for y in 0..height-1:
      for p in 0..nPlanes-1:
          for x in 0..width-1:              # ceil(width/8) bytes, MSB = lowest x
              index[y][x] |= bit(x) << p
  ```

- **`compression`: 0 is raw, anything else is ByteRun1/PackBits** - `n < 0x80` is a literal
  run of `n+1` bytes, `n > 0x80` repeats the next byte `0x101-n` times, `0x80` is a no-op.
  Both values ship, and both are exercised by shipped assets. `IffBodyEncodeScanline`
  @ 0x005e0690 is the game's own writer for both, so neither is a legacy path.

**A plane row is padded to a byte, not to a word.** This is the one place Gunlok deviates
from the ILBM specification, which requires rows of an even number of bytes, and it is the
trap most likely to break a writer built from the spec. It is only observable where
`ceil(width/8)` is odd, i.e. the 8-pixel-wide mip levels: both of them consume exactly
`ceil(width/8) * nPlanes * height` bytes and neither matches word padding.

Verified across all 23 palettized files: **77 `BODY` images (palette variants and mip levels
included) decode with exact stream consumption, every index lands inside its own `CMAP`, and
`nPlanes == ceil(log2(entries))` in all 77.**

### `ALPH` - the alpha plane

A 6-byte header then a blob, and it reuses the `BODY` decoder wholesale
(`AlphChunk_BeginDecode` @ 0x005e0f70 copies its own `width`/`compression`/`bits` into the
same slots `BodyChunk_BeginDecode` fills from the `BMHD`, with `bits` playing the role of
`nPlanes`):

| Offset | Size | Type   | Field |
|--------|------|--------|-------|
| 0x00   | 2    | uint16 | `width` |
| 0x02   | 2    | uint16 | `height` |
| 0x04   | 1    | uint8  | `bits` - becomes the plane count |
| 0x05   | 1    | uint8  | `compression` - 0 raw, else ByteRun1 |
| 0x06   | ...  | bytes  | planar alpha, same layout as `BODY` |

Its presence is what routes `RimConvertRows` to the two alpha-aware converters, with
`RimImage+0x50` = `ALPH.bits`. On the `S3TC` path that field is set from the fourcc instead
(`DXT1` -> 0, otherwise 8).

**Open question: whether an `ALPH` inside a `PROP:ILBM` is found at all.** The only shipped
`ALPH` is in the `PROP` of `Ground\tree_alpha.RIM`, but `RimOpenAndScan` looks it up with
`IffChunk_FindChild` against the `ILBM`. Whether IFF `PROP` properties are merged into that
lookup scope has not been read out of the binary. It does not affect writing a file - put
`ALPH` in the `FORM` and the question is moot - but it does decide whether that one texture
gets its alpha. The same doubt applies to `TRAN`, and there it is academic: every shipped
`TRAN` body is eight zero bytes, and `RimBindImageChunks` gates the colour key on the first
byte being non-zero, so no shipped file applies one either way.

The reader is `blender/io_scene_rif/rim.py`, exercised over all 513 by
`blender/tests/test_rim.py`, and it reads the `S3TC` path only. There is still no writer there,
but the reason has changed: nothing in the addon compresses DXT, and **the palettized path needs
no compressor at all** - a `BMHD` + `CMAP` + uncompressed planar `BODY` with `CONT = 0` is a
configuration the shipped files already use. `utils/rimutil` does write it.

### Confirmed in the running game

Everything above was measured against the shipped assets; this section is the end-to-end check,
because a shared misreading of the format would satisfy every self-consistency test.

All 28 textures `level01.rif` names were re-encoded from their own decoded pixels as **raw
`BODY`** (`rimutil compress --format body --raw`) and served to Gunlok through GkPlus's PhysFS
mod overlay, so no Steam asset was modified. Result: `mods.served` reached **28 of 28**, the
level loaded with its usual 158 actors and a running simulation, and the scene rendered
correctly.

Against a baseline capture of the same camera with the stock DXT1 textures, static geometry
differs by a mean absolute error of **0.9-1.9 per channel** (max 35-119), concentrated on edges.
That is the *right* difference: the `BODY` copies are lossless, so what shows up is DXT1's block
artifacts being absent from them. Corruption - a wrong plane count, word-padded rows, a mangled
palette - would have produced a garbled image, not a sub-2 LSB edge delta.

The set exercised the interesting paths as well as the ordinary one: `Units\alpha junk.RIM` has
graded alpha and therefore round-tripped through an `ALPH` chunk, and the palettes ranged from
1,851 to 59,746 entries (11 to 16 bitplanes).

One thing this did *not* cover: a `masking = 2` transparent index, which none of level01's
textures needed. That path is covered by the synthetic cases in `utils/rimutil/tests` but has
not been seen by the engine.

---

## Upstream: the Aliens vs Predator (1999) 3dc engine

Gunlok's asset pipeline is Rebellion's `3dc` chunk library, the same one shipped in the
**published AvP Gold source release** (`source/AvP_vc/3dc/`). This is not a family resemblance —
it is the same code, and the AvP source is usable as ground truth for anything in this document.

### Evidence

- **48 of 72** chunk identifiers extracted from `3dc/win95/*.[ch]pp` appear verbatim in `gl.exe`,
  including the root id `REBINFF2` (AvP's `GodFather_Chunk`).
- AvP's `HuffmanDecompress()` (`3dc/win95/huffman.cpp`), ported unmodified, decompresses
  **563 of 563** shipped Gunlok `.rif` files. Every result begins `REBINFF2`, its root
  `chunk_size` equals the decompressed length exactly, and the chunk tree walks to the end of
  the buffer with zero slack bytes.
- `REBCRIF1` is AvP's `COMPRESSED_RIF_IDENTIFIER`, and the 316-byte compressed header
  documented above is AvP's `HuffmanPackage` struct field-for-field.
- **88 of the 105** chunk ids Gunlok registers exist in the AvP source (table below).
- Per-file string overlap is concentrated *entirely* in `3dc/win95/*CHUNK*.cpp`,
  `chnkload.cpp`, `hierchnk.cpp`, `animobs.cpp` and `avp/win95/{Projload,Objsetup}.cpp`.
  AvP's game layer (`avp/*.c` — behaviours, weapons, player) shows no meaningful overlap.

**So: the asset/chunk layer is shared; the game layer is not.** Gunlok's Roles, Actors, GLS
scripts, triggers and menus have no AvP counterpart and must still be read out of the binary.
Do not expect `STRATEGYBLOCK`, `MODULE` or AvP's behaviour blocks to describe Gunlok structures.

### Structures this confirms

These were already modelled from the binary; AvP names them and explains the layouts:

| GkPlus / notes name                         | AvP original                             | AvP source        |
|---------------------------------------------|------------------------------------------|-------------------|
| `LevelList` `{sentinel,count,cache,valid}` (0x10) | `List<T>` `{sentinel,n_entries,entry_pointers,calculated_indices}` | `list_tem.hpp` |
| `Menu::GetItemData` "cached, NO bounds check" | `List<T>::operator[]` — the bounds `fail()` is `#define fail if (0)` under `NDEBUG` | `list_tem.hpp:402` |
| roles/actors hash `{n,buckets,mask,table}`  | `_base_HashTable` `{nEntries,tableSize,tableSizeMask,chainPA}` | `Hash_tem.hpp:587` |
| `RoleList`/`ActorNode` `{data, next}`       | `HashTable<T>::Node` `{TYPE d; Node* nextP;}` (data first, then link) | `Hash_tem.hpp:557` |
| `Chunk` (0x28) / `Chunk_With_Children` (0x2c) | `class Chunk` / `class Chunk_With_Children` | `Chunk.hpp:203` |
| `Chunk::Register` @ 0x005d4ae0              | `Chunk::Register(idChunk, idParent, pfnCreate)` | `Chunk.hpp:249` |

Two deliberate divergences found so far:

- `GetRoleById`/`GetActorById` use the raw id (`mask & id`) as the bucket index. AvP's
  `HashFunction(unsigned)` scrambles (`i ^ i>>4 ^ i>>9 ^ i>>15 ^ i>>22`); Gunlok dropped it,
  which is fine for its sequential ids.
- `Chunk::Register` hashes the 8-char id as `(dword0 + dword1)` mixed through
  `h = (((((h>>7^h)>>6^h)>>5^h)>>4^h)`, not AvP's `sum of toupper(c)`.
- The roles/actors tables have **no vptr**; `RifRegEntryHashtable` does (AvP's
  `_base_HashTable` has virtual `NewNode`/`DeleteNode`). Only the chunk registry kept them.

### Chunk registration in the binary

`RegisterAllRifChunkClasses` does not exist — MSVC emits **118 separate 18-byte static-init
constructors**, one per registration, each `PUSH pfnCreate / MOV EDX,idParent (or XOR EDX,EDX) /
MOV ECX,idChunk / CALL Chunk::Register / RET`, 0x20-aligned, running 0x0043b3f0..0x0043c511 and
reached from the CRT init table at `.rdata:0x0064d9d0`. These are AvP's
`RIF_IMPLEMENT_DYNCREATE` / `..._DECLARE_PARENT` macro expansions (`Chunk.hpp:535`).

In the Ghidra DB they are named `RifRegisterCtor_<ID>[_in_<PARENT>]`, and the 118 loader
functions they install are `RifLoad_<ID>[_in_<PARENT>]`. Each loader body is the macro verbatim:
`new XxxChunk(parent, data+12, *(int*)(data+8) - 12)`.

118 registrations cover 105 distinct ids. **15 ids are registered under a required parent**
(AvP's `_DECLARE_PARENT` form), accounting for 28 of the registrations; the loader differs per
parent, so these are genuinely distinct functions:

| Chunk      | Registered under            |
|------------|-----------------------------|
| `ANIFRADT` | `ANIMFRAM`                  |
| `ANIMFRAM` | `ANIMSEQU`                  |
| `ANISEQDT` | `ANIMSEQU`                  |
| `FRMMORPH` | `SHPMORPH`                  |
| `SHPHEAD1` | `REBSHAPE`                  |
| `SHPMORPH` | `REBSHAPE`, `SUBSHAPE`      |
| `SHPMRGDT` | `REBSHAPE`, `SUBSHAPE`      |
| `SHPPNORM` | `REBSHAPE`, `SUBSHAPE`, `ANIMFRAM` |
| `SHPPOLYS` | `REBSHAPE`, `SUBSHAPE`, `CONSHAPE` |
| `SHPRAWVT` | `REBSHAPE`, `SUBSHAPE`, `ANIMFRAM` |
| `SHPTEXFN` | `REBSHAPE`, `SUBSHAPE`      |
| `SHPUVCRD` | `REBSHAPE`, `SUBSHAPE`, `CONSHAPE` |
| `SHPVFLAG` | `REBSHAPE`, `SUBSHAPE`      |
| `SHPVNORM` | `REBSHAPE`, `SUBSHAPE`      |
| `SUBSHPHD` | `SUBSHAPE`                  |

Note `CONSHAPE` is a parent id that is never itself registered.

### Gunlok-only chunks (17)

No AvP source exists for these; they must be read from the binary. Twelve are the cutscene
system that backs the `.cut` sidecar files (see `level_loading_notes.md`):

`CTUSNDPR` `CTUSRDAT` `CTUSRHIE` `CTUSSPPO` `CUTEVENT` `CUTPOINT` `CUTSCDAT` `CUTSCUSR`
`CUTSHEAD` `CUTTRACK` `CUTTRFOV` `CUTTRNAM` — plus `MODZONE`, `OBINTDT`, `SHPFLAGS`,
`SHPVFLAG`, `TRSNDCAT`.

### Chunk ID to AvP class/source map

For the other 88, the AvP class name and file give real field names and types to check the
layouts in this document against. Highlights (full list reproducible by grepping the AvP tree
for the quoted id — note the extensions are **uppercase**, so `grep --include=*.cpp` misses most
of `win95/`):

| Area                   | AvP source file                    | Chunks |
|------------------------|------------------------------------|--------|
| Shapes / meshes        | `win95/SHPCHUNK.CPP` / `.HPP`      | `REBSHAPE` `SHPHEAD1` `SHPRAWVT` `SHPVNORM` `SHPPNORM` `SHPPOLYS` `SHPUVCRD` `SHPPCINF` `SHPCENTR` `SHPMRGDT` `SHPMORPH` `FRMMORPH` `SHPFRAGS` `FRAGDATA` `FRAGLOCN` `SUBSHAPE` `SUBSHPHD` `CONSTYPE` `ASALTTEX` `ANSHCEN2` `ANIMSEQU` `ANIMFRAM` `ANIFRADT` `ANISEQDT` |
| Object hierarchies     | `win95/hierchnk.cpp`, `MISHCHNK.CPP` | `OBJCHIER` `OBJHIERD` `OBHIERNM` `HIERBBOX` `HSETCOLL` `OBHALTSH` `HIDEGDIS` |
| Object animation       | `win95/animobs.cpp` / `.hpp`       | `OBANSEQS` `OBANSEQC` `OBANALLS` `OBASEQHD` `OBASEQFR` `OBASEQFL` `OBASEQSP` `OBASEQTM` |
| Modules / objects      | `win95/OBCHUNK.CPP`                | `RBOBJECT` `OBJHEAD1` `MODULEDT` `MODFLAGS` `MODACOUS` `ADJMDLEP` `OBJNOTES` `OBJPRJDT` `OBJTRAK2` `VMDARRAY` `TRAKSOUN` |
| Environment            | `win95/ENVCHUNK.CPP`               | `REBENVDT` `ENDTHEAD` `ENVACOUS` `ENVSDSCL` `SOUNDDIR` `SPECLOBJ` |
| Bitmap names           | `win95/BMPNAMES.CPP`               | `BMPNAMES` `BMNAMEXT` `BMNAMVER` `BMPLSTST` `BMPMD5ID` `MATCHIMG` `SHBMPNAM` |
| Lighting               | `win95/LTCHUNK.CPP`                | `LIGHTSET` `LTSETHDR` `STDLIGHT` `AMBIENCE` `PLOBJLIT` `LITSCALE` `SHPVTINT` |
| Sound                  | `win95/SNDCHUNK.CPP`               | `SOUNDOB2` `SOUNDEXD` `SOUNDNAM` `INDSOUND` `ALTLOCAT` |
| Dummy objects          | `win95/DummyObjectChunk.cpp`       | `DUMMYOBJ` `DUMOBJDT` `DUMOBJTX` |
| Load-side consumers    | `avp/win95/Projload.cpp`, `Objsetup.cpp` | how the chunks are turned into runtime structures |

`Projload.cpp` and `Objsetup.cpp` are the most useful of these: they are AvP's equivalent of
Gunlok's `ToMap` (0x0047f160) and show what each chunk is *for*, not just its byte layout.
