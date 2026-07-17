# RIF Chunk Format Reference

Reverse-engineered from the Gunlok (2000) game binary via Ghidra static analysis.

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
| `OBASEQFL` | OB anim sequence flags     | 0x005cd8c0 | 1 x int32 (flags) |
| `OBASEQTM` | OB anim sequence time      | 0x005cd7e0 | 1 x int32 (time) |
| `OBASEQSP` | OB anim sequence speed     | 0x005cd840 | 3 x int32 (12 bytes) |
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
| `SHPCENTR` | Shape center point         | 0x005b90d0 | 4 x float (16 bytes: Vec3 center + 1 extra) |
| `FRAGDATA` | Fragment data              | 0x005bad50 | 4 x int32 (16 bytes) |
| `FRAGLOCN` | Fragment location          | 0x005badd0 | Vec3 position + Vec3 orientation + 1 int32 (28 bytes) |
| `HIERBBOX` | Hierarchy bounding box     | 0x005cc3b0 | 2 x Vec3 (24 bytes: min + max) |
| `ANSHCEN2` | Anim shape center v2       | 0x005b6b90 | 5 x int32 (20 bytes) |

### Array Chunks

Body starts with an element count or size, followed by an array of fixed-size entries.

| Chunk ID   | Purpose                  | Address    | Body Format |
|------------|--------------------------|------------|-------------|
| `SHPRAWVT` | Shape raw vertices       | 0x005b8250 | Array of Vec3 (count = body_size / 12) |
| `SHPPNORM` | Shape polygon normals    | 0x005b8650 | Array of Vec3 (count = body_size / 12) |
| `SHPVNORM` | Shape vertex normals     | 0x005b8430 | Array of Vec3 (count = body_size / 12) |
| `SHPVFLAG` | Shape vertex flags       | 0x005b8360 | Array of uint32 (count = body_size / 4) |
| `SHPPOLYS` | Shape polygons           | 0x005b8930 | Array of polygon structs (count = body_size / 36). See below. |
| `SHPMRGDT` | Shape merge data         | 0x005b96c0 | Array of int32 (count from parameter) |
| `HIDEGDIS` | Hierarchy edge display   | 0x005cc1c0 | uint32 count + count x uint32 |
| `SHPPRPRO` | Shape preprocessed data  | 0x005bae70 | uint32 count + count x int32 (+ additional context) |
| `CUTPOINT` | Cutscene points          | 0x005d91a0 | uint32 count + count x 16 bytes (4 x float) |
| `CTUSSPPO` | Cutscene special points  | 0x005d8a80 | uint32 count + count x 36 bytes (9 x float) |
| `OBANALLS` | OB anim all sequences    | 0x005cd960 | uint32 count + count x 20 bytes (5 x int32) |
| `OBJTRAK2` | Object track data v2     | 0x005b3aa0 | uint32 count + count x 76 bytes (0x4C) |
| `VMDARRAY` | VMD array data           | 0x005b32a0 | uint32 count + count x 16 bytes (0x10) |

#### SHPPOLYS Polygon Entry (36 bytes / 0x24)

| Offset | Size | Type   | Description              |
|--------|------|--------|--------------------------|
| 0x00   | 4    | int32  | Field 0                  |
| 0x04   | 4    | int32  | Texture/material index   |
| 0x08   | 4    | int32  | Field 2                  |
| 0x0C   | 4    | int32  | Field 3                  |
| 0x10   | 4    | int32  | Vertex index / -1 sentinel|
| 0x14   | 20   | ...    | Additional vertex/face data |

### Struct + String Chunks

Body contains a fixed struct followed by (or interspersed with) null-terminated strings.

| Chunk ID   | Purpose                    | Address    | Body Format |
|------------|----------------------------|------------|-------------|
| `LTSETHDR` | Light set header           | 0x005d2ab0 | 8-char name (strncpy, not null-term) |
| `INDSOUND` | Indexed sound              | 0x005cecf0 | int32 index + string wav_name + additional data |
| `TRAKSOUN` | Track sound                | 0x005b3d70 | 6 x int32 (24 bytes) + string name |
| `SOUNDOB2` | Sound object v2            | 0x005ce360 | 3 x int32 (12 bytes) + string name (or empty if null) |
| `SHPVTINT` | Shape vertex tint          | 0x005d2df0 | 8-char name + int32 count + count x int32 |
| `CTUSRDAT` | Cutscene user data         | 0x005d8500 | string name + (padded to 4-byte boundary) + additional bytes |

### Complex Struct Chunks

These chunks have multi-field bodies that don't fit simple patterns.

| Chunk ID   | Purpose                    | Address    | Body Format |
|------------|----------------------------|------------|-------------|
| `ENDTHEAD` | Environment data header    | 0x005ca340 | int32 + 16-char name + int32 (24 bytes) |
| `ANIFRADT` | Anim frame data            | 0x005b7860 | 1 x int32 + 4 x int32 (at offsets 5-8 from source) |
| `ANISEQDT` | Anim sequence data         | 0x005b7500 | 8 x int32 (32 bytes) |
| `OBJNOTES` | Object notes               | 0x005b30a0 | Raw byte data (text), size = body_size |
| `CONSHAPE` | Connection shape           | 0x005b7c40 | Minimal data (container-like structure with no actual data) |
| `SUBSHPHD` | Sub-shape header           | 0x005b6860 | References parent shape info. Default index = -1. |
| `DUMOBJTX` | Dummy object transform     | 0x005d2540 | Delegated to internal function FUN_005d2700 |
| `MATCHIMG` | Match image data           | 0x005d11f0 | Linked list + byte flag + additional fields |
| `RANTEXID` | Random texture ID          | 0x005ca750 | String + linked list structure |

### Large Fixed Structs

These chunks copy many consecutive fields from the stream.

#### `OBJHEAD1` - Object Header (0x005b2cd0)

Contains object metadata, position, orientation, and properties.

| Offset | Size  | Type    | Description                  |
|--------|-------|---------|------------------------------|
| 0x00   | 4     | int32   | Lock flags                   |
| 0x04   | 16    | char[]  | Object name (strncpy, 16)    |
| 0x14   | 4     | int32   | (padding/extra)              |
| 0x18   | 12    | Vec3    | Location (x, y, z)           |
| 0x24   | 12    | Vec3    | Orientation / angles          |
| 0x30   | 4     | int32   | Module vis object flags       |
| 0x34+  | ...   | ...     | Additional properties         |

#### `DUMOBJDT` - Dummy Object Data (0x005d22c0)

| Offset | Size  | Type    | Description                  |
|--------|-------|---------|------------------------------|
| 0x00   | 12    | Vec3    | Position                     |
| 0x0C   | 12    | Vec3    | Orientation                  |
| 0x18   | 12    | Vec3    | Scale / extra vectors        |
| 0x24   | 16    | int32[4]| Additional properties        |

#### `STDLIGHT` - Standard Light (0x005d2b70)

Large struct (~68+ bytes):

| Offset | Size  | Type    | Description                  |
|--------|-------|---------|------------------------------|
| 0x00   | 4     | int32   | Light type                   |
| 0x04   | 8     | float[2]| Intensity / range            |
| 0x0C   | 4     | int32   | Field                        |
| 0x10   | 4     | int32   | Field                        |
| 0x14   | 12    | float[3]| Color / direction            |
| 0x20   | 16    | float[4]| Position / orientation       |
| 0x30   | 4     | int32   | Additional flags             |
| 0x34   | 12    | float[3]| More color/direction data    |
| 0x40   | 4     | int32   | Field                        |
| 0x44+  | ...   | ...     | Extended light properties    |

#### `PLOBJLIT` - Placed Object Light (0x005d2c80)

Large struct (~60+ bytes) with position, color, and attenuation data.

#### `OBASEQFR` - OB Anim Sequence Frame (0x005cd470)

| Offset | Size  | Type    | Description                  |
|--------|-------|---------|------------------------------|
| 0x00   | 36    | int32[9]| Frame header (9 fields)      |
| 0x24   | 4     | int32   | Field 9 (parent/chunk_size)  |
| 0x28   | 4     | int32   | Field 10 (num sub-frames)    |
| 0x2C   | n*4   | int32[] | Sub-frame data array         |

#### `OBASEQHD` - OB Anim Sequence Header (0x005cd610)

| Offset | Size  | Type    | Description                  |
|--------|-------|---------|------------------------------|
| 0x00   | 12    | int32[3]| Header fields                |
| 0x0C   | 4     | int32   | Count of sub-entries         |
| 0x10   | n*4   | int32[] | Sub-entry array              |
| ...    | ...   | string  | Name string (after array)    |

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
| `SHPHEAD1` | Shape header v1            | 0x005b8b30 |
| `SHPPCINF` | Shape PC info              | 0x005ba7b0 |
| `BMPMD5ID` | Bitmap MD5 ID              | 0x005d15c0 |

### Version/Name Management Chunks

These chunks perform lookups and management rather than storing simple data:

| Chunk ID   | Purpose                    | Address    | Notes |
|------------|----------------------------|------------|-------|
| `BMNAMVER` | Bitmap name version        | 0x005cf730 | Manages versioning of bitmap names. Complex deduplication logic. |
| `BMNAMEXT` | Bitmap name extension      | 0x005cfba0 | Extends bitmap name data. Creates if not exists. |
| `SHBMPNAM` | Shape bitmap names         | 0x005d0530 | Bitmap name list for a shape. Linked list of entries. |
| `OBJHIERD` | Object hierarchy data      | 0x005cb1e0 | String name + hierarchy position lookup. |

### Texture Chunks

| Chunk ID   | Purpose                    | Address    | Body Format |
|------------|----------------------------|------------|-------------|
| `SHPTEXFN` | Shape texture filenames    | 0x005b94d0 | uint32 count + count x string (null-terminated filenames) |
| `SHPUVCRD` | Shape UV coordinates       | 0x005b9330 | uint32 num_polys + per-poly: uint32 num_verts + num_verts x 2 x float (UV pairs) |

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
| `HIDEGDIS` | Array    | Hierarchy edge display flags   |
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

The following chunk IDs are referenced in the RIF viewer (`rifviewer/main.js`) as containers but do **not** exist in the Gunlok binary. They may be from the general Rebellion RIF SDK used by other games:

- `SPRIHEAD` - Sprite header
- `SPRITEPC` - Sprite PC
- `SPRITEPS` - Sprite PS
- `SPRITESA` - Sprite Saturn
- `FRAGTYPE` - Fragment type
