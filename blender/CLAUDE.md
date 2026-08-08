# GkPlus Blender Addon (`io_scene_rif`)

Import/export of Gunlok `.rif` geometry for Blender. This file is the design and gotcha
record for `blender/`; it loads on top of the repo-root `CLAUDE.md`, which covers the C++
`d3d8.dll` side and the reverse-engineering reference.

**Paths and file references below are relative to the repository root**, not to this
directory — `rif_chunk_format.md`, `game_defects_notes.md` and `level_loading_notes.md` are
all at the root. Test invocations are in the root `CLAUDE.md` under "Running the test suites".

## The addon

Import/export of `.rif` geometry, in **pure Python** — no compiled extension, so it installs as a
plain zip and shares nothing with `d3d8.dll`. `blender/README.md` is the user-facing half.

`blender/pyproject.toml` is a **uv project for the tooling only** — `uv run tools/build_zip.py`
builds `dist/io_scene_rif-<version>.zip`, `uv run --group dev ruff check .` lints. **The addon
itself must stay dependency-free**: Blender ships its own interpreter and an extension cannot pull
wheels at install time, so nothing in `[dependency-groups]` may ever be imported from
`io_scene_rif/`. `blender_manifest.toml` owns the version (Blender reads it) and the build fails if
`pyproject.toml` disagrees, which is the only thing stopping the two from drifting. `build_zip.py`
lists its payload explicitly rather than globbing, so a stray file cannot ship.

The split is the point: `io_scene_rif/rif.py` (container: huffman, chunk tree, serialization),
`io_scene_rif/shapes.py` (REBSHAPE geometry), `io_scene_rif/heads.py` (the record chunks —
`OBJHEAD1`, `SHPHEAD1`, `OBJHIERD`, `OBASEQHD`, `OBASEQFR` — plus the keyframe-timing rule),
`io_scene_rif/bmpnames.py` (the texture table),
`io_scene_rif/sounds.py` (the `INDSOUND` sound table),
`io_scene_rif/emitters.py` (the `DUMOBJTX` ambient-emitter grammar — a different
system entirely, see below) and
`io_scene_rif/rim.py` (`.RIM` textures: IFF container, DXT1/DXT3 decode, palettized `BODY` both
ways) import **no `bpy`**, so
`blender/tests/` exercises them over all 563 shipped files with Blender absent — the opposite of
`src/`, where nothing runs outside Gunlok. `io_scene_rif/scene.py` and
`io_scene_rif/__init__.py` are the only files that touch Blender.

These pin the design, in roughly decreasing order of how much everything else rests on them
(the list grows; deliberately not numbered, because the count kept going stale):

- **The scene is the whole file: export reads nothing but the scene.** Every chunk becomes a
  Blender datablock — objects, mesh datablocks, lights, an armature, Actions, Speakers, mesh
  attributes, and typed `int32`/`float32`/string properties for the rest. **Export** takes no
  source-file parameter and there is **no opaque byte storage anywhere**, so a `.blend` moved to a
  machine that has never seen the original `.rif` still produces one. *Import* does read outside
  the file, but only for the pictures and the audio — a `.RIM` and a `.wav` are named by the `.rif`
  and stored beside it, and neither is ever written back. `tests/test_scene.py` enforces exactly that: build the scene, save a
  `.blend`, *reset Blender*, reopen, export. The rule is container chunks → objects, leaf chunks →
  typed fields on the parent, with `rif_id` / `rif_index` / `rif_absorbed` carrying the structure.
- **The bar is semantic equivalence, not byte equivalence**, and that is what lets the exporter
  regenerate instead of mirror. `SHPCENTR` is recomputed; `SHPPCINF` is discarded (**inert** —
  the string has three referrers in gl.exe, its registration, its loader and
  `StripUnusedShapeChunks` @ 0x005b5df0 which deletes it after the shape is built, and nothing ever
  reads the fields the loader fills; this was inferred from AvP's null-guards and is now measured
  against Gunlok itself); `SHPMRGDT` and `SHPVTINT` are
  *authored* per-element data so they become mesh attributes and survive an edit rather than going
  stale. Nothing is silently dropped when you edit a mesh. The byte-exact round-trip test still
  guards the **container** layer (`tests/test_roundtrip.py`, 563/563) and the **typed field** layer
  (`tests/test_schema.py`, `encode(decode(body)) == body` for 485,663 leaf chunks across 44 ids) —
  those two are what make the scene able to stop referencing the file.
- **`SHPHEAD1` is regenerated, not carried, and finding that out is what `heads.py` is for.** It
  looks like an id chunk and is not: after `flags`/`lock_user`/`file_id_num` it holds `num_verts`,
  `num_polys`, `radius` and the min/max bounds, all of which AvP's from-buffer constructor reads
  straight into its shape record — and `ToRole` derives a character's collision extents from a
  shape's bounds whenever the GLS gives `radius`/`height` as 0. Carried through `rif_absorbed` like
  any other chunk, as it was, it goes stale the instant a vertex moves. Recomputing is safe:
  `num_verts`, `num_polys` and both corners reproduce the stored values in **9,357/9,357** shipped
  shapes, so an unedited file is unchanged but for `radius`. That one is bit-exact in 42% and
  drifts a median 7e-7 relative (p90 5e-4, four shapes past 1%) — and it is regenerated **anyway**,
  because it is the same quantity `SHPCENTR` carries, the two are byte-identical in all 9,244
  shapes that have both, and `SHPCENTR` was already being recomputed. Carrying one and recomputing
  the other would make every export disagree with itself in 58% of shapes.
- **The 16 bytes at `OBJHEAD1+0x04` are AvP's `lock_user`**, the editor's lock owner — not a name
  and not junk. Earlier revisions of this file called them "a separate (often uninitialised) tag",
  which was right that they are not the name and wrong that they carry nothing: 8,136 of the 9,313
  shipped objects have something there. They are authored, so they are carried; zeroing them was
  what made a rebuilt header fail to reproduce 87% of the shipped ones, which is how the field got
  identified.
- **An object finds its shape by id, never by position.** `OBJHEAD1+0x38` (`shape_id_no`) matches
  `SHPHEAD1+0x14` (`file_id_num`), which is what AvP's `Object_Chunk::assoc_with_shape_no` does.
  That resolves all 9,313 objects in the shipped files with no shape claimed twice, and it
  **differs from document order in 86 of the 563 files (15.3%)** — pairing the two lists
  positionally, which an earlier version did on the strength of "the counts match", attaches
  geometry to the wrong transform in one file in seven. Matching counts do not imply matching
  order.
- **A level-of-detail variant is moved onto its base part's bone, and its placement is frozen.**
  `L<n>#<part>` is a naming convention, not a chunk (full description in `rif_chunk_format.md`):
  the engine swaps that mesh onto the hierarchy node driving `<part>`, so no `OBJCHIER` node binds
  a variant and nothing would animate it. `_build_armature` therefore parents it to the base's
  bone **at the base's transform**, and `_object_chunk`/`_dumobj_chunk` skip the usual
  `matrix_world` write-back for anything carrying `rif_lod_base`. Both halves are needed: the
  shipped sets are parked beside the model (Gunlok MkII's L5 at +1200 on X) while their vertices
  are already in the base's local frame, so the stored placement is dead data that would be
  destroyed by recomputing it — and preserving it keeps the round-trip byte-exact whether or not
  that reading is right. The guard is "base exists and is bound, variant is not":
  `destructorfrag.RIF` has nine parts genuinely *named* `L7#head` etc. with no base, bound
  directly. 1,477 of 1,518 variants qualify; the rest stay loose, as they are in-game.
- **Moving a mesh onto a bone changes what a click selects, and that broke animation playback.**
  Once the variants coincided with the parts they replace, clicking the chest selected `L5#Chest`
  instead — and the Action editor targets the *active object*, so choosing a sequence assigned it
  to a mesh with no bone channels while the armature went on playing the one bound at import. It
  presents as "every action plays the first action", nowhere near the animation code, and it is
  invisible to any test that assigns actions to the armature directly, which is what every probe
  did before the user's `animation_data.action_slot` came back `<<NONE>>` on a mesh. Variants are
  therefore hidden with **`hide_set`** (the eye) — not `hide_viewport` (the monitor), which drops
  the object from the depsgraph and freezes `matrix_world`, making the placement unverifiable and
  reporting 1,309 phantom drifts — and the importer leaves the rig selected as well as active.
  When a change adds objects to the scene, check selection, not just geometry and transforms.
- **A texture is a name, and the name is all the `.rif` has.** A polygon carries an *index*
  (`colour & 0xfff`) into one file-level table, `BMPNAMES` under `REBENVDT` — decoded in
  `bmpnames.py`, layout in `rif_chunk_format.md`. So the three parts live apart: the **table** on
  the collection (`rif_bmpnames`, kept whole and in order, so entries no polygon references
  survive), the **index** and the **name** on a per-import material (`rif_texture_index` /
  `rif_bmp_name`), and the **image** as a packed Blender image that is never written back.
  Retexturing is editing `rif_bmp_name`; a name the table does not list appends an entry at a
  fresh index. Four things this rests on, each measured over all 563 files:
  - **The table's `index` is a stable id, not a list position** — entries are stored in
    *descending* index order and are sparse (`Maze.RIF` has `[10, 9, 8, 5, 4, 1]`). It resolves
    1,518,963 of 1,766,071 polygons; of the rest 22,331 are the `0xfff` untextured sentinel and
    215,517 of the remaining 224,777 are in `_shadow` files, whose polygons carry junk texture
    *and* UV indices. No table has a duplicate index or name.
  - **The table is the one thing held to byte-exactness**, because it is carried rather than
    regenerated — including the *uninitialised* padding after each name (`Units\baddies3.RIM` is
    followed by `00 f5`, not by NULs). `encode(decode(body)) == body` for all 527 tables.
  - **Its chunk position belongs to whichever datablock absorbed the container it sits in**, and
    that is *not* always the collection: `REBENVDT` holds a `LIGHTSET` in **62** of the 563 files
    (every file has exactly one `REBENVDT`), which makes it an object rather than data folded onto
    the collection. Assuming the collection loses every material name in exactly those files
    (`Maze`, `S3 Level`, `city ruins`, `level01`…) — which is what `_note_table_owner` and
    `_table_location` exist for, and what `tests/test_scene.py` caught.
  - **Materials are per-import, not shared by name**, because an index only means anything inside
    its own file: the same `.RIM` is entry 11 in one level and entry 4 in the next. Images *are*
    shared, which is where the cost is.
  - **A Blender image filled through `pixels` does not survive a `.blend` round trip** — it is a
    *generated* image and comes back blank, which no import-then-export test catches unless it
    reopens the file and reads a pixel. So a decoded texture is written out as a PNG (`zlib` is
    stdlib), loaded, packed, and the temp file removed; that also hands the sRGB tagging to
    Blender instead of reimplementing a transfer function.
- **The UV index is 20 bits in the four shapes that need it, and 16 everywhere else.** AvP's
  `ChunkPoly::GetUVIndex` folds `colour` bits 12-15 in whenever they are set; Gunlok does that
  only in `city ruins`, `level07`, `level15` and `level12`, whose UV tables hold 65,663..77,669
  entries. Reading those with the nibble reproduces `uv_index == polygon index` for 282,412 of
  their 282,454 polygons; without it every index past 65,535 **wraps into range** and quietly
  wears another polygon's UVs — which is what the exporter did until the scene test grew a UV
  check. Everywhere else the nibble is not an index at all (it takes all 15 values, and 99.05% of
  the polygons carrying it have no usable UV entry either way), so the rule is a property of the
  *shape*: `shapes.Shape.extended_uv`, `decode_uv_index`, `encode_colour`.
- **A `SHPUVCRD` UV is a texel coordinate, not a fraction**, so it is divided by the size of the
  texture its polygon names and multiplied back on export (`scene.uv_scale` / `uv_to_blender`).
  Feeding the raw value to Blender tiles a 1024×1024 texture 1024 times across every face, which
  is what the first version did. Three things pin it, all in `rif_chunk_format.md`: the 99th
  percentile of `|u|` lands within 7% of each texture's own width for every size in the game;
  AvP casts the float straight to the int its renderer wants in texel space
  (`chnkload.cpp:2390`); and 374,658 of 376,641 sampled pairs are whole numbers — *not all*, so
  nothing may round. **V grows downward** (Direct3D convention), measured at 86.3% of the 8,916
  axis-aligned wall polygons in the shipped levels, so it is flipped as well as scaled. Both
  directions are bit-exact in float32 because every texture is a power of two. The scale is
  re-derived at export from the material's *current* texture, not replayed, so retexturing onto a
  different size rescales the UVs — with `rif_uv_scale` on the material as the fallback for a
  `.blend` opened where the textures are not installed.
- **`.RIM` is IFF, not a RIF chunk file** — big-endian sizes, 4-character ids, `LIST`/`FORM`/`PROP`
  groups, odd bodies padded — carrying DXT1/DXT3 in an `S3TC` chunk whose 22-byte header is
  documented in `rif_chunk_format.md`. The **payload is little-endian** even though the container
  is not, which is settled by content rather than inspection: `lava.RIM` decodes orange, and a
  red/blue swap would make it blue. 490 of the 513 shipped textures are S3TC; the other 23 are
  palettized `CMAP`/`BODY` with no S3TC — 16 `*_fmv_*` ground textures and seven UI/unit/
  structure ones, so this is not one art pipeline's quirk. `rim.py` reads both.
- **The palettized path is the engine's *preferred* one, and it is the one `rim.py` writes.**
  `RimOpenAndScan` @ 0x005dd6b0 enumerates `ILBM`→`BODY` first and only falls back to
  `ILBM`→`S3TC`, so a file carrying both would have its DXT ignored — and among several
  palette-depth variants of one picture `RimBindImageChunks` takes the largest `CMAP`, its colour
  cap being unbounded on a true-colour destination. `decode` follows both rules, so it shows what
  the player would see rather than whichever chunk comes first. `BODY` is planar ILBM —
  `CMAP` of 3-byte RGB entries (**mandatory**; absent is error 9), `nPlanes` MSB-first bitplane
  rows per scanline, each `ceil(width/8)` bytes, optionally ByteRun1-compressed — and `BMHD.masking`
  must be 0 or 2, never ILBM's mask plane or lasso. **A plane row is padded to a byte, not to the
  word boundary the ILBM spec requires**, which is only visible at the 8-pixel mip levels and is the
  trap that would break a writer built from the spec. Verified over all 77 palettized images in the
  shipped set: exact stream consumption, every index inside its `CMAP`,
  `nPlanes == ceil(log2(entries))`. Full layout, the 13-entry IFF chunk registry and the engine
  addresses are in `rif_chunk_format.md`.
- **Writing needs no compressor, which is the only reason the addon can do it at all.** One
  palette entry per distinct colour is exactly lossless, so `rim.encode` quantizes nothing;
  `nPlanes` follows from the count (up to 31, since the scanline decoder accumulates into a
  uint32), and alpha goes to a transparent palette index when every transparent texel shares one
  RGB and to an `ALPH` chunk otherwise — **that choice is not cosmetic**, because the RGB under a
  transparent texel is what bilinear filtering blends into its opaque neighbours. **The `ALPH`
  goes in `PROP:ILBM`, not in the `FORM`** — measured in the running game, a `FORM`-placed one is
  never found and renders bit-for-bit identically to a file with no `ALPH` at all, while the same
  chunk in the `PROP` renders correct graded alpha. Both writers put it in the `FORM` until
  2026-08-08, which is why this file used to claim the engine ignored `ALPH` outright; it does not.
  See `rif_chunk_format.md`, "An `ALPH` must be a child of `PROP:ILBM`".
  The one real limit is a **256-colour cap when an `ALPH` is emitted**: the engine's alpha
  converter faults above that (measured, 0x005df14a), and neither writer has a quantizer, so both
  refuse rather than emit a file that crashes. `Ground\tree_alpha.RIM` is the only shipped texture
  in that shape — it needs an `ALPH` and palettizes to 90,319 colours — so it is the one image the
  `body` path cannot round-trip. DXT stays in
  `utils/rimutil`, where libsquish is. The two writers emit **identical bytes** for the same
  image, raw and ByteRun1 alike, which is what makes them cross-checkable rather than separately
  believed. Cost: 2–6× a DXT payload, and about a second per 1024² image in pure Python.
  All 513 shipped textures decode, re-encode and come back byte-identical (`test_rim.py`, ~20
  min); the partial group at the end of a plane row is **not** reachable from the shipped set —
  every width in the game divides by eight — so it is covered by synthetic cases instead.
- **`image.pixels` is how pixels come back out of Blender, and it is exact.** For an 8-bit image
  it is the stored byte over 255 with no colour management in the way: an untouched texture reads
  back identical to the `.RIM` it was decoded from, on 4.2 and 5.2, through a `.blend` save and
  reload, alpha included. `Image.save()` to a PNG or a raw TGA reproduces the same bytes and is
  50× faster than the PNG route — but both go through a temp file and `Image.file_format`, which
  is shared datablock state, and one probe caught Blender writing the **packed PNG verbatim** when
  asked for a TGA. `pixels` has no format to disagree about. Two traps around it: **do not touch
  `colorspace_settings` to read** — changing it re-reads a packed image from its packed bytes and
  silently discards unsaved paint — and row 0 is the *bottom* in Blender and the *top* in a `.RIM`,
  the same flip the UVs get. (`image.pixels` is not the `color` vs `color_srgb` hazard from the
  vertex-lighting work; that one is about attributes.)
- **"Has this texture been edited?" is a digest taken at import, not `is_dirty`.** `is_dirty` means
  *unsaved* edits and clears on a `.blend` save, so paint-save-reopen would read as untouched;
  `rif_rim_crc` is stamped in `_image_for` and never updated, so the test is "do these pixels still
  match the file they came from". It is stored as a **hex string**, because an ID property is a
  signed C int and `zlib.crc32` uses all 32 bits — storing the number raises `OverflowError` mid
  import on half of all images. That got through a three-file test run and failed on the fourth
  file of a ten-file one, which is the argument for widening the sample rather than trusting a
  green run.
- **The textures are a second output, so they have their own destination.** `write_textures` is
  called by the export operator, not by `rebuild_tree` — the model goes to the file the user
  picked and the images go to a mod tree mirroring `Graphics`, and the default is to write
  **nothing**. "Changed only" is the useful setting: a re-export of an untouched level would
  otherwise turn 28 DXT textures into 28 larger `BODY` files pixel-identical to what the player
  already has.
- **Verified in the running game.** All 28 textures `level01.rif` names were re-encoded as raw
  `BODY` with `rimutil` and served to Gunlok through GkPlus's PhysFS mod overlay (`gkplus\mods\`,
  so no Steam asset was touched). `mods.served` reached 28/28, the level loaded with its usual 158
  actors, and the scene rendered correctly — the difference against the stock DXT1 build is
  0.9–1.9 mean absolute error per channel on static geometry, confined to edges, which is DXT1's
  block artifacts being *absent* from the lossless copy rather than any fault in it. So the format
  description above is confirmed end to end, not just self-consistent — **for opaque textures**.
  This measurement looked at static geometry and never at an alpha blend, which is exactly why it
  missed the `ALPH` placement bug above: `Units\alpha junk.RIM` was in the set, its alpha was being
  dropped throughout, and the MAE number is identical either way because the quads that use it in
  a level are additive over black RGB. The instrument that did catch it was the **front-end menu
  lozenge**, drawn `SRCALPHA`/`INVSRCALPHA` on the main menu, where alpha is directly visible in a
  screenshot — worth reaching for before any wider comparison.
- **Files are written uncompressed.** 150 of the 563 shipped files already are, and the game reads
  them, so the Huffman *compressor* is never needed. `huffman/huffman_compress.cpp` still exists for
  byte-parity work.
- **Porting `HuffmanDecode` needs two things the C hides.** Shift counts are masked to 5 bits on
  x86, so `x << 32` is `x << 0` and not zero — Python shifts by the full count and silently corrupts
  the bit window. And it reads up to a word past the end of the compressed block; without padding
  the input, the 64 largest files stop exactly 4 bytes short.
- **`rif_chunk_format.md` was wrong about most of the chunks that matter** and is now corrected
  there: `SHPRAWVT` is int32 triples (not floats), `SHPPOLYS` is
  `{engine_type, normal_index, flags, colour, vert_ind[5]}` and always triangles, `SHPUVCRD` is an
  indexed list rather than one entry per polygon, `SHPCENTR` is int32 centre + float radius about
  the **origin**, `OBASEQFR` is a fixed 44-byte keyframe (quaternion + int32 position + normalized
  16.16 time + frame index) rather than a variable sub-frame array, `STDLIGHT` is 84 bytes of pure
  integers whose 0x10 field is an orthonormal 3×3 in 16.16 — and is **AvP's `Light_Data` field for
  field** (21 int32, `win95/LTCHUNK.HPP`), which is what named `spread` @ 0x38, `local_flags` @ 0x48
  and the `pad` pair @ 0x4c; they were `field_0x38`/`field_0x48`/`field_0x4c` in `schema.py` and in
  the stored `rif_*` properties while only their offsets were known. `SHPMRGDT` is one int32 per polygon,
  and `SHPVTINT` is a 16-byte header plus one int32 per vertex that hangs off `RBOBJECT`, not off a
  shape. AvP's `ChunkPoly` is *not* Gunlok's — it has an explicit `num_verts` and `vert_ind[4]`.
  Every one of those was measured across all 563 files; the old entries read like plausible
  guesses, which is what they were.
- **A decoded chunk body has to be *flat*, and that is what shapes the cutscene codecs.**
  `schema.py` can express a fixed field list and nothing else, so the eight variable-length
  cutscene chunks — a padded string, a counted array, a tagged record stream — get hand-written
  decode/encode pairs in `schema.CODECS`, consulted before `STRING_CHUNKS` and `SCHEMA`. The
  representation is constrained by storage, not by the format: `_unpack_absorbed` converts each
  stored value with a **single `list(val)`**, so a value may be a scalar, a string, or a flat
  homogeneous list — never nested, and never mixing ints with floats. That is why `CUTEVENT`'s
  records come back as parallel arrays (`kinds` / `headers` / `payload` / `payload_counts`, plus
  the per-record strings joined by `\n`) rather than as the list of dicts the format describes.
  The trap is that a nested list round-trips perfectly in pure Python and only fails once a
  `.blend` is involved, so `test_schema.py` cannot see it — `tests/test_cutscene.py` checks the
  storable shape directly, and the `.blend` round trip was confirmed on `level01.RIF` and on
  `city ruins.RIF`, whose 0-record `CUTEVENT` is the empty-array edge case.
  Two things are carried rather than regenerated for a reason: a point's packed time keeps its
  **top byte**, which the engine masks off but 125 of 763 shipped points have set, and
  `CUTTRNAM`/`CTUSRHIE` left `STRING_CHUNKS` because they are *not* bare strings — the trailing
  int32 they carry were being stored as `padding`.
- **A cutscene is real datablocks, and the camera is two objects rather than one.**
  `cutscene.py` (no `bpy`) turns the absorbed `REBENVDT/SPECLOBJ` subtree into nested objects and
  back; `scene.py` builds a Camera, an Empty it looks at, and an Empty per participant, with the
  path in **location F-curves** — no second copy of it anywhere, the same rule `rif_light` follows.
  Four things make that work, each of which fails silently if you get it wrong:
  - **The camera's rotation is never exported.** The engine derives orientation as a look-at
    between two pseudo-participants — `is_camera == 0` supplies the position, `flags` bit 0 the
    target — so a Blender camera's rotation means nothing and the quaternions in the `CUTPOINT`
    trailer orient *actors*. Author with a Track To constraint.
  - **One frame is one tick.** The engine's interval is 40 ms and all 763 shipped durations are a
    multiple of it, so keying at `cutscene.FPS` (25) makes a keyframe number a lossless duration
    and removes any need for a parallel array of times.
  - **A stored duration of 0 on a non-final point means one *second*, not zero.** 129 of 341
    shipped non-final points store it. Building frames from the raw value puts two control points
    on one keyframe, Blender keeps one, and the path silently shortens — which is exactly what the
    first version did. `effective_durations` substitutes; `zero_flags` remembers which encoding to
    write back. That flag is safe to carry beside the keyframes precisely because both encodings
    play identically, so a desynced flag can only choose the other spelling of the same duration.
  - **`_suspend_animation` must skip these objects.** It clears every action to read the rest pose,
    which for a cutscene track deletes the data — the exporter then reads one posed location and
    writes a one-point path. The symptom is a path that loses every point but the last.
  Blender's F-curve is **not** the engine's curve (Bezier vs uniform Catmull-Rom), so keys are
  drawn as LINEAR and `preview_cutscene_path` builds the real spline as a poly curve carrying no
  `rif_` id, so export skips it like a Speaker. On the shipped paths the spline departs from the
  straight line between its own control points by a median 5.6% of segment length and up to 47%
  (3.5 m), so this is not a cosmetic preview. The loader synthesises the two phantom end control
  points itself — **never write them**.
  Verified: all 14 cutscene-bearing levels, 34 cutscenes, 2,517 chunks byte-identical through
  import → `.blend` → reset → reopen → export, plus `tests/test_cutscene_authoring.py` building one
  from nothing and reading it back off the wire.
- **And verified in the running game, which took three false results to get right.** `level01.RIF`
  exported and served through the mod VFS renders `PLAY CUTSCENE first contact` at MAE 7.95 against
  stock, where two *stock* runs differ by 7.35 — inside the noise. The control (every camera path
  moved 3 m) renders at 24.62. Three things made earlier versions of this test lie, all worth
  knowing before running another:
  - **Renaming a mod directory does not disable it.** PhysFS mounts every entry under
    `<Gunlok>\gkplus\mods`, so `cutscene-test.disabled` is still mounted and still serves. The
    "baseline" ran the mod for several rounds; `mods.served` read 3 with the folder renamed and 0
    only once it was moved out of the tree entirely. Move it, don't rename it.
  - **`mods.served` is 0 until something has been asked of the VFS**, so querying it before
    `levels.start` reports 0 whether or not the mod is mounted — which makes any comparison built
    on it vacuous. Query after the level load, and read `mods.recent` for the actual paths.
  - **`camera.position` does not follow the cutscene camera.** It reads `CameraCoords`
    @ 0x007b4e0c, which during `first contact` moved 5 units in X and 0.27 in Y while the authored
    path spans 8 units in Y. A trace of it looks like a plausible cutscene and is not one; the
    frame buffer is what settles this, via `PrintWindow(hwnd, dc, 3)`.
- **A cutscene needs an end event or it never ends**, and nothing in the file makes that obvious:
  running off the end of a path leaves the camera parked and the player locked out. Only a control
  event (kind 3, payload 0) ends one, so `new_cutscene` emits it and `cutscene_problems_for`
  reports its absence. The other authoring gate is outside the `.rif` entirely — a cutscene is
  unreachable until the level's `.gls` declares `camera track { file … name … }`, or a script calls
  `make.camera_track`.
- **`_build` absorbs a chunk's children *after* every branch has decided what it took**, and
  getting that order wrong is invisible to every semantic test. `skip` is what stops a chunk being
  carried in `rif_absorbed` as well as regenerated on export; `RBOBJECT` adds to it in the first
  if-chain, before the absorb, but `DUMMYOBJ`'s `skip.add(DUMOBJDT)` used to run in the *second*
  chain, after the absorb had already copied it. So every dummy exported its `DUMOBJDT` twice —
  6,847 of them across the shipped set, 590 where `level01.RIF` has 295 — for as long as the addon
  has existed. Nothing caught it because the duplicate carried the same *meaning*, and every check
  in `test_scene.py` compares meaning: the objects a file describes, their transforms, their
  geometry. **The chunk inventory is the check that sees this class of bug**, it is now in
  `compare_inventory`, and `INVENTORY_EXPECTED` is the short list of ids allowed to change (only
  `SHPPCINF`, which is discarded on purpose). Re-introducing the ordering bug makes it fail on
  three of eight sampled files.
  Two things this turned up that are worth keeping:
  - **The engine did not care.** The in-game cutscene test ran against an exported `level01.rif`
    carrying the duplicates and the level loaded normally (158 actors / 259 roles) and rendered
    within the run-to-run noise — `lookup_single_child` takes the first, which was the carried
    original. So this was structural waste, not a visible fault, which is exactly why it survived.
  - **Regenerating a chunk is not the same as carrying it, and the difference shows up when the
    duplicate goes.** With both emitted, the *carried* one won every lookup; with the fix, the
    regenerated one is all there is. For `DUMOBJDT` that is safe — location and name are
    byte-exact and the orientation differs by at most **0.03°** — but the raw bodies are not
    identical, and 128 of 295 differ only by the quaternion double cover (`q` vs `-q`, the same
    rotation). Comparing quaternion *components* reads that as a drift of 1.83 and is the wrong
    metric; compare the angle.
- **`SHPCENTR` is recomputed, so absence has to be recorded or export invents one.** The inventory
  check found this immediately after the `DUMOBJDT` fix: `Battler Turret.RIF` ships 4 shapes of
  which 2 carry a `SHPCENTR`, and the export gave all 4 one. `NO_CENTRE_PROP` (`rif_no_centre`) is
  the marker, set in `_mesh_for_shape` when the source shape had none — the same "presence is a
  separate question from the values" rule `rif_vtint_header` follows. A shape authored from scratch
  carries no marker and so still gets a `SHPCENTR`, which is what a new shape wants.
- **`SHPMRGDT` is a *pairing*, not a per-face value, and carrying it through a face drop crashed
  Gunlok on 15 of the 24 shipped levels.** Fixed — the scene now stores a **pair id** and export
  validates before writing — but the mechanism is worth keeping, because it is the sharpest example
  in this addon of an index into a list the addon renumbers.
  - **The consumer, and it is not the renderer.** `MergePolygonsInChunkShape` @ 0x005d7900 (AvP's
    `merge_polygons_in_chunkshape`, `chnkload.cpp:3130`) fuses coplanar triangle *pairs* named by
    `merge_data` into quads. It is reached only from `BuildShapeVertexBuffers` @ 0x005ab300, whose
    merge block runs **only when its `flags & 1`** — and of the eight call sites of
    `RifFindObjectByName` @ 0x005aa5c0 (which is `__fastcall` with **five** parameters, `RET 0xc`,
    not the three the DB had), only two pass 1: `ToMap`'s cold geometry path @ 0x0047f926 and
    `GetShape` @ 0x004ae6c4. So **only the map object and the shadow object are ever merged**,
    which is why a prop with broken merge data is harmless and the map is not.
    **The D3D buffers are built before the call and never rebuilt**, so the merge cannot remove a
    drawn primitive: its only output is the `LevelMeshHeader` that becomes the level's nav
    sections. `SHPMRGDT` is pathfinding data wearing a rendering name — see `rif_chunk_format.md`,
    "Merging polygons into quads", and `level_loading_notes.md` §5.5 on `NavQuad`.
  - **The invariant, and there is no bounds check anywhere in it.** The loop is bounded by
    `shape->num_polys` — the `SHPPOLYS` count, *never* by `SHPMRGDT`'s own `num_polys`, which
    `ShapeMergeDataChunk_FromData` @ 0x005b97d0 computes as `size >> 2` and nothing compares — and
    it indexes `poly_list[merge_data[i]]` unchecked, into an output array of exactly
    `malloc(num_polys * 0x24)` with no bound on the write cursor. So the data must be a proper
    involution: `m[i] == -1`, or `m[i] == j` with `j != i`, `j < num_polys` and `m[j] == i`. All
    24 shipped levels satisfy it; a shipped file can never reach the fault.
  - **What the addon does wrong.** `rif_merge_group` stores the partner's **raw source polygon
    index**, and export writes it back verbatim — while import drops faces Blender cannot hold
    (two on the same three vertices) and export drops triangles that weld degenerate. Dropping a
    single face renumbers every face after it, so one dropped face does not corrupt one pair, it
    corrupts the whole table: `level02` loses 14 faces and ends with 14 out-of-range entries,
    1,349 self-referential and 6,677 non-involutive. Measured across the shipped levels, **15 of
    24 map objects lose at least one face and would crash; the 9 that lose none load.**
  - **Confirmed in the game, 4 for 4 against that prediction**: `level01` and `level06` (no faces
    dropped) load with their usual actor and role counts; `level02` and `level11` (14 and 26
    dropped) fault at `gl+0x1d79a5` and `gl+0x1d7abe` — the first a *write* past the output array
    when a broken pairing emits one polygon twice, the second a *read* of `poly_list[j]` with
    `j == num_polys`. And an exported `level02` with only the stock `land` shape spliced back in
    loads, which is what isolates the shape from everything else the export changes.
  - **The fix: the scene stores a pair id, and export validates.** `rif_merge_group` is gone;
    `rif_merge_pair` (`MERGE_PAIR_ATTR`) gives both partners of a pair the same number and
    everything else `-1`, which is stable under reordering and degrades correctly under dropping —
    a survivor whose partner went simply holds an id nothing else shares and writes `-1`.
    `shapes.merge_pairs_from_wire` / `merge_wire_from_pairs` are the two halves, and they reproduce
    the wire values exactly for **all 9,357** shipped shapes. Across the 24 map objects the fix
    turns 15 unwalkable tables into 0, and costs 73 merge pairs out of 325,598 — only where the
    partner face was genuinely dropped.
    **What a lost pair costs is one nav section, not one quad on screen.** The merged list never
    reaches the renderer, and because both mergers refuse unless `a->flags == b->flags` and the
    quad inherits the lower-indexed triangle's plane, walkability is identical either way. So a
    lost pair is one extra `NavPolygon` instead of one `NavQuad`, one extra A\* node against the
    200-500 node budget, and a different `.map` sidecar. 73 extra sections across all 24 levels,
    against level01's own 29,045 → 19,675, is a rounding error — but "same pixels" was the right
    answer for the wrong reason, and the honest statement is "same pixels, one more section".
    - **The rename is the migration.** A `.blend` from the old build has no attribute under the new
      name, so its shapes export with **no `SHPMRGDT` at all** — legal, guarded, and costing only
      the quad merge. The old values are deliberately *not* converted: they are indices into a
      numbering the importer had already changed, so a conversion would produce a plausible and
      wrong pairing.
    - **`shapes.merge_problems` is the guard, and it runs at the point of writing**, not as a
      pre-flight — a validated-then-modified table is exactly the shape of bug this had. Anything
      it cannot prove is dropped rather than written (`stats["merge_dropped"]`, reported as a
      warning), because omitting is always safe and writing is not. `tests/test_shapes.py` asserts
      the predicate over every shipped shape *and* carries four synthetic breakages as a control;
      `tests/test_scene.py` asserts it on both the source and the exported tree of every sampled
      file.
    - **Verified in the running game**: `level02` and `level11`, which faulted before, now load
      with 178/294 and 317/352 actors and roles.
- **Blender cannot hold two faces on the same three vertices**, and the shipped assets contain
  them (doubled or reverse-wound triangles): 775 across 193 shapes, and 28 of Maskelyn MkII's 44
  polygons. **Drop them deliberately, before `from_pydata`** — letting `validate()` remove them
  renumbers `me.polygons` out from under the source list, so every face after the first duplicate
  takes the *previous* face's texture, UVs, flags and merge group. That was invisible for as long
  as the tests compared per-face data as unordered counters, which a permutation satisfies.
  Such a shape loses one face on import and is therefore
  flagged as edited on export even when untouched. `engine_type`, `flags` and "did this polygon
  have a UV entry" have no Blender equivalent either and ride as **face attributes**; the last one
  cannot be inferred from the values, because a zero-length UV entry and one holding three `(0,0)`
  pairs are different on the wire and both occur. A new UV layer is also **not zero-initialised** —
  Blender seeds it with a per-face box mapping, so an untextured polygon silently acquires
  `(0,0)/(1,0)/(1,1)` and reads back as an edit unless the importer overwrites it.
- **Export drops a triangle whose corners coincide once quantized, and that is a crash fix, not
  tidiness.** `SHPRAWVT` is integer, so `to_rif` can collapse two vertices Blender considers
  distinct; the engine welds vertex records by position at load, and such a triangle then carries
  a *repeated* vertex pointer. Gunlok's polygon adjacency predicate (slot 0x50,
  `PolygonAdjacencyTest` @ 0x0048ecf0) collects shared vertices into a fixed `Vec3[3]` with **no
  bound check**, so two of them meeting in one section-grid cell reach 4+ matches and the fourth
  write lands exactly on the function's `/GS` cookie — `0xc0000409` inside
  `LoadOrBuildSectionAdjacency`, naming neither the file nor the polygon, and **invisible under a
  debugger** because it suppresses the WER dump. `shapes.weld_map` / `welds_degenerate` is the
  test, `_shape_chunk_from_mesh` drops before appending anything parallel (so `polys`/`uv_lists`/
  `merge` stay in step and `uv_index` stays contiguous), and the count is reported as a
  **warning**, never INFO. **The shipped set does not license skipping this**: 14 files do carry
  such triangles (`corps building.RIF` 22, `gastowerfrag.RIF` 14) but every one is under
  `RIF\Objects` or `RIF\Units` and **none under `RIF\Levels`** — only level geometry reaches
  section adjacency, so a prop's are merely invisible. `test_scene.py` counts this loss beside the
  duplicate-face one, disjointly and in pipeline order (import drops duplicates, export drops
  degenerates from the survivors). Full write-up in `game_defects_notes.md` §5.
- **The navmesh is the level's own polygons, and the addon can show you which ones.**
  Gunlok authors, loads and generates no separate nav data: `BuildNavPolygons` @ 0x004888d0
  builds one 0x40-byte record per map polygon and `BuildPolygonAdjacencyGrid` links the
  walkable ones, which is precisely `(flags & 0x100) == 0 && normal.y < 0` — RIF is Y-down,
  so a **negative** Y faces up. Bit 0x100 is set by the loader for anything steeper than
  **45°** (`ny² + 1e-5 < nx² + nz²`, the epsilon putting the limit exactly on 45) and
  *also* survives the `& 0x3fffc1` mask, so it doubles as an authored blocker — level01
  marks 1,914 faces that way. `shapes.is_walkable` / `nav_islands` mirror it and
  **Preview Navmesh** (`object.rif_navmesh_preview`) writes `rif_walkable` and
  `rif_nav_island` FACE attributes and leaves the walkable faces selected. It deliberately
  writes **no colour attribute**: export is name-gated on `rif_light` so one left here could
  never *become* the lighting on its own, but `adopt_color_attribute` reads whichever one is
  *active*, so leaving one puts a navmesh preview one click from adoption. **The metric that
  matters is the largest island's share, not the island count**: shipped levels run
  32–45% walkable across 420–1,014 islands with 58–67% in the largest, so a mesh at 33%
  walkable but 2% in the largest is broken — that is what 21.7% open edges (against
  level01's 6.8%) does, and it leaves the walkable *fraction* looking fine. Full
  derivation in `level_loading_notes.md` §5.5.
  One thing the preview deliberately does not model: the engine's sections are **not** one per
  triangle. `SHPMRGDT` fuses 74.4% of map polygons into quads first, so level01's 29,045
  triangles are 19,675 nav sections in game. That changes none of the metrics above — merging
  two coplanar edge-sharing triangles alters neither the walkable surface nor the connectivity,
  and a pair only merges when both halves already share their flags — so the *island* numbers are
  the same either way. It does change the A\* node count, which is what the merge is for.
- **RIF is Y-down, and the swizzle has to carry the orientation too, not just the position.** A
  biped's parts all sit at negative Y — feet nearest the origin (~-100), top of the head furthest
  (~-1990) — so the body extends in -Y from the ground and **-Y is up**; assembled in Blender a
  character spans Z = 0.000 to 2.589 m with its feet exactly on the ground plane. The map is
  `(x, y, z) -> (x, z, -y)`, a **-90°** rotation about X, and a placed object's quaternion needs
  the same change of basis (`m q m⁻¹`) or it lands in the right spot facing the wrong way. The
  determinant is +1 so it does not mirror, which is correct because RIF is right-handed —
  `shapes.face_normal` takes an ordinary right-handed cross product of raw RIF coordinates and
  matches the shipped `SHPPNORM` on 99.91% of 1.77M faces. Single-shape vertex extents do **not**
  settle this (characters are articulated per-part meshes, roughly symmetric about their own
  origins); the part *placements* do.
- **`obj.matrix_world` is stale until `bpy.context.view_layer.update()`**, which makes an assembled
  model look collapsed — the first attempt at this measurement reported a 0.765 m character
  because every part was still at the identity. `OBJHEAD1` is
  `{int flags, char[16] lock_user, int32[3] location, float[4] quaternion (x,y,z,w), int index_num,
  int version_no, int shape_id_no, char[] name}` (`heads.py`, from AvP's
  `Object_Header_Chunk::fill_data_block`); a routine export rewrites only the location and
  orientation and leaves the rest alone, and the name and shape id move only when the UI is used to
  move them.

## Authoring a file, rather than editing an imported one

Export reads exactly the properties **import** minted — `rif_id`, `rif_index`, `rif_objhead`,
`rif_absorbed` — so a datablock created any other way is skipped in silence. `scene.py`'s
authoring section and twelve operators in `__init__.py` are the way in (`scene.rif_new`,
`object.rif_add`, `object.rif_add_dummy`, `object.rif_add_sequence`, `object.rif_new_shape_id`,
`object.rif_new_light_id`, `scene.rif_add_sound`, `scene.rif_remove_sound`,
`scene.rif_select_sound`, `scene.rif_add_emitter`,
`pose.rif_set_sound`, `action.rif_toggle_setting`, beside the import and export pair), with four
panels over them — Object, Collection (the sound table), Speaker data and Material.
`scene.rif_new` builds a `REBINFF2`
collection carrying the file-level chunks (`RIFVERIN`, `REBENVDT` +
`ENDTHEAD`/`RIFFNAME`/`BMNAMEXT`/`BMNAMVER`) laid out as `SQUARE.RIF` — the smallest shipped file,
1,004 bytes — carries them, and `object.rif_add` gives a selected mesh an `RBOBJECT`/`REBSHAPE`
pair with a fresh shape id. `tests/test_authoring.py` drives all of it inside Blender.

Five things there are worth keeping:

- **No `BMPNAMES` is created up front.** `_new_table_location` already knows how to append one
  after `REBENVDT`'s children the first time a material names a texture, which is the path the 36
  shipped files without a table would take. Seeding an empty one would only add a second way to
  get there. `BMNAMEXT` is 52 bytes in **all 316 shipped chunks** regardless of how many entries
  the table holds — it is a fixed record, not a parallel array — so emitting it zeroed is not a
  guess about its size scaling. (247 files have none at all, which is why it is a chunk count and
  not a file count.)
- **A RIF name is not `obj.name`.** The engine resolves by `strcmp` against `OBJHEAD1+0x3c` — the
  map's `name`, every `for "<rif object>"` spawn point, `OBJHIERD`'s node binding — and the
  exporter never regenerates that from the outliner, which is uniquified anyway. `rif_object_name`
  (an RNA property with get/set over the stored body) is the setter; `DUMMYOBJ` is the exception
  and keeps using `rif_name`, because `_dumobj_chunk` re-appends from there.
- **Duplicating an object duplicates its shape id, and that one is fatal.** Re-import pairs both
  objects with the first shape and orphans the second mesh, so export **refuses** a collection with
  a shared id rather than writing it. A duplicated *name* is left alone: it is legal, and
  `RifFilterObjectsByName` returning several matches is how a level spawns more than one actor
  from one clause. `rif_shape_id` writes both halves of the pair at once, which is the whole point
  of having it — setting only `OBJHEAD1`'s would leave the object naming a shape nothing claims.
- **The game does not read lights at all**, so authoring one is a fidelity feature, not a way to
  light a level. Each of `LIGHTSET`/`LTSETHDR`/`STDLIGHT`/`AMBIENCE`/`PLOBJLIT`/`LITSCALE` occurs
  once in gl.exe's image and is referenced only from its own class ctors and `RifRegisterCtor_`
  thunk — all inside the 0x005d29xx-0x005d35xx LTCHUNK run — while the rif consumers look chunks up
  by **id string** (`BuildRifFileObject` @ 0x005a9b50: `lookup_single_child(this, "REBENVDT")`,
  `lookup_child(..., "INDSOUND", ...)`) and MSVC pools identical literals. The control is what makes
  that conclusive: `SHPVTINT`, `INDSOUND`, `REBENVDT`, `OBJHIERD`, `OBASEQFR`, `SHPPOLYS` and
  `OBJHEAD1` all show referrers outside their own TU. Runtime lighting is **`SHPVTINT`** (baked
  per-vertex intensity, read by `BuildHierarchyNode` and `RifFindObjectByName`) plus the sun
  globals; the `.rif`'s lights are the editor-time input that baked it. `AMBIENCE` is likewise not
  a light: AvP's `Objsetup.cpp` puts it in `GlobalAmbience` and the renderer clamps every channel
  **up** to it (`if (redI < GlobalAmbience) redI = GlobalAmbience`) — a `max()`, scalar and
  colourless, 16.16 with `ONE_FIXED` 65536, so Gunlok's 2048 is a 3.1% black floor.
- **`SHPVTINT` is therefore where lighting is authored, and two of its four header words are not
  opaque.** `[0:2]` is the light set *name* and is the **selector** — an object may carry one chunk
  per light set and the loader takes the one matching the active `LTSETHDR`
  (`strncmp(svic->light_set_name, ::light_set_name, 8)`, `Projload.cpp:1424`), so a chunk written
  with a zeroed name is never found; Gunlok ships `NORMALLT` on all 4,668 chunks and all 62
  headers. `[3]` is `num_vertices`, equal to the array length in 4,668 of 4,668, and the engine
  **trusts** it — it allocates `12 * num_vertices` and iterates that many times, so a stale count
  reads past the body. `_vtint_chunk` regenerates it instead of carrying it, the same rule
  `SHPHEAD1`'s counts follow; carrying it meant any mesh edit that changed the vertex count wrote a
  self-inconsistent chunk. The per-vertex value is a **packed `0x00RRGGBB`**, not a scalar and not
  16.16 — no shipped value lands in 0..65536 at all.
- **The scene stores the paintable form and nothing else: `rif_light`, a `BYTE_COLOR` POINT
  attribute, *is* the exported value.** That rests entirely on `color_srgb`, never `color` —
  reading a `BYTE_COLOR` through Blender's `color` property converts sRGB↔linear and loses a
  least-significant bit on **157 of 256** values per channel, while `color_srgb` is the stored
  byte: **256/256 exact** through a `.blend` save and reload, alpha included. Confirmed end to end
  rather than in the abstract — **1,086 of 1,087** shipped `SHPVTINT` chunks over a 25-file sample
  survive import→export byte-identical. The sole exception is not a loss: `level05_shadow.RIF`
  ships 13,098 values for a **13,016-vertex shape**, and the export truncates to the mesh. That is
  a different quantity from the `num_vertices` agreement above — the header matches its *own* array
  in 4,668 of 4,668, while the array matches the **id-paired shape's** vertex count in only 4,666
  (`level15_shadow.RIF` is the other, and `rif_chunk_format.md` records the same 4,666). The tail
  no vertex indexes is unread by the engine, and the packed-int design truncated it identically,
  so this is neither new nor lossy. There was a packed-int mirror
  (`rif_vertex_intensity`) with two operators converting between them; it is gone, because two
  attributes that can desync is a failure mode and "I painted and forgot to pack" is another.
- **What replaced it is a name gate, and the gate is the whole design.** Export reads `rif_light`
  **by name** and never `color_attributes.active_color`, which is Blender-wide UI state a bake, a
  preview or any other feature repoints freely — so nothing can become the file's lighting by
  accident. `adopt_color_attribute` is the deliberate way in, and is therefore the **only** place
  the two lossy reductions happen: a **corner-domain** attribute averaged per vertex (Blender's
  default domain and a common bake target; the file cannot express per-corner) and the clamp at
  1.0, which makes a bright lamp saturate to white — indistinguishable from a bake that never ran,
  and it cost a test failure before the energy was dropped to 20 W. Export **raises** on a
  `rif_light` it cannot read as one value per vertex rather than averaging or padding, with
  `lighting_problems` as the pre-flight check beside the shared-shape-id one.
- **Presence is a separate question from the attribute, and conflating them adds chunks.** The
  marker is `rif_vtint_header` — the chunk's own header, which exists exactly when the object
  carries a chunk — so the preview can mint a white `rif_light` for an unlit mesh (the engine draws
  no-`SHPVTINT` as white diffuse, and a `ShaderNodeAttribute` naming a missing attribute reads
  black) without every mesh you *look at* acquiring lighting. Marker with the attribute deleted is
  neither a refusal nor a silent loss: the chunk is dropped and counted into
  `stats["lighting_dropped"]`, which export reports as a warning.
- **Previewing that lighting has to multiply in gamma space, and the naive material is wrong by
  7.43/255.** The engine's shading is `texel × diffuse` on the stored bytes — `BuildShapeVertexBuffers`
  @ 0x005ab300 assigns `diffuse = shpvtint->values[vert] | 0xFF000000` (undecoded, top byte ignored,
  `0xFFFFFFFF` with no `SHPVTINT`) and `InitBuiltinMaterials` @ 0x005757b2 gives `Mat_Opaque`
  `COLOROP = MODULATE, ARG1 = TEXTURE, ARG2 = DIFFUSE`. That is D3D8 fixed function, so the multiply
  is on the **gamma-encoded** numbers. Doing it in linear space instead — which is what
  Color Attribute × Image Texture → Emission does — would only be equivalent if sRGB were a pure
  power law; the piecewise toe below 0.04045 makes `(aᵍ·bᵍ)^(1/g) = a·b` fail by up to **7.43 LSB**,
  worst in the dark midrange the game actually uses (2.30 at light `0x08`, 6.49 at `0x20`, 7.43 at
  `0x40`, 0 at `0xff`). A pure 2.2 power law gives **exactly 0.0**, which is what identifies the toe
  rather than any imprecision. So `scene.rif_preview_setup` runs *both* operands through an exact
  linear→sRGB encode to recover `byte / 255` (an sRGB image and a `BYTE_COLOR` attribute are alike
  sRGB storage that Blender linearises on read), multiplies, and decodes the product back before
  Emission — every conversion **exact piecewise, built from Math nodes**, because `ShaderNodeGamma`
  is a pure power law and the shader node set has no colour-space conversion node at all. Encoding
  the texture rather than **setting the image to Non-Color** is the one non-obvious choice, and it
  is not fastidiousness: an image is a *shared* datablock, so writing its colour space would leave
  every authored material rendering a linearised texture as raw once the preview was restored.
  Rendered end to end this
  reproduces `texel * light / 255` at **0.00 LSB**. Three things fall out: Solid mode cannot do this
  *at all* (`View3DShading.color_type` is VERTEX **or** TEXTURE with no blend property, which is why
  it needs a material), the view transform must be **Standard** with Look/Exposure/Gamma neutralised
  (Filmic and AgX are tone mappings and roll off instead of clamping at 1.0), and Emission makes the
  world irrelevant — identical pixels at world strength 0 and 20 — so scene lighting is left alone.
  The test renders the naive graph beside the real one and asserts it **fails**, since a tolerance
  nothing violates would prove nothing.
- **A light is not in the object list at all, and that is what makes adopting one different.**
  All 3,794 shipped `STDLIGHT` chunks are children of a `LIGHTSET`, and all 62 `LIGHTSET`s are
  a direct child of the file-level `REBENVDT` — one per file, never more, and 24 of the 62 hold no
  lights, so an empty set is ordinary data. `adopt_object` therefore routes a `LIGHT` to
  `_adopt_light`, which takes none of the shape/`OBJHEAD1` bookkeeping and instead calls
  `lightset_for`. The work is in `_promote_rebenvdt`: in the 502 files without lights `REBENVDT`
  is *absorbed* onto the collection as a path prefix (`_is_data_only`), and a `LIGHTSET` object
  needs a parent **object**, so the first light lifts that container into one — moving its
  absorbed children and, if it owned it, `rif_bmpnames_path`. Getting that wrong loses the texture
  table, which is why `_table_location` scans objects as well as the collection.
  Two smaller ones: Blender's 1000 W default `energy` would export a 16.16 `brightness` a thousand
  times past the shipped 0.2..2.0, so adoption normalises it to 1.0 (adoption is the one moment
  that cannot be confused with an edit); and `light_id` is allocated past the highest in the file
  because it is unique per file in all 38 that have lights but is **not** `0..n-1` (only 10 of the
  38), i.e. an editor-assigned id rather than a position.
- **A `DUMMYOBJ` is authorable, and it is a different namespace from a spawn point.**
  `RifFilterObjectsByName` -> `RifCollectObjectChunks` @ 0x005b0900 keeps a child only if its id is
  literally `RBOBJECT`, so a `for "<rif object>"` clause can **never** resolve to a dummy — which
  is why `object.rif_add` still makes only `RBOBJECT`s and `object.rif_add_dummy` is a separate
  gesture. A dummy is the *locator* system: ambient sound, console and trigger positions, MP and
  enemy spawns, CTF points. Four gates, all measured over the 6,847 shipped dummies, and the first
  is a crash rather than a nuisance:
  - **A `DUMOBJDT` is mandatory.** `DummyObjectChunk_GetDataChunk` @ 0x005d21d0 returns NULL when
    it is missing and `MapAuxObject_Ctor` dereferences that unchecked at 0x005a971a — an access
    violation during level load. 6,847 of 6,847 have one, so the game's own data never exercises
    it and nothing in the engine is hardened against it. `adopt_dummy` creates one and
    `dummy_problems` **refuses the export** if it was lost, beside the shared-shape-id check.
  - **Top level only** (`RifCollectDummyChunks` walks the root's direct children and never
    recurses; all 6,847 are at depth 0), and **a non-empty name** — an empty one is stored as
    `NULL`, not `""`, and every name-matching consumer skips the record. 0 of 6,847 ship empty.
  - **A dummy is an emitter or a marker, never both.** `ToMap` unlinks and frees the record it
    turns into a sound, so an emitter's name never reaches a single one of the seven name-matching
    consumers. The panel says which of the two an object is rather than offering both.
  - **Duplicate names are legal and shipped** — 210 of them across 62 files — so they are a
    warning. The catch worth warning about is that resolution differs: `ConsoleParsePosition` takes
    the *first* match where the other six consumers take the *last*.
  The extents at `DUMOBJDT+0x0c`/`+0x18` are read by **nothing** in the engine, so a new dummy
  leaves them zero rather than deriving a bounding box no consumer wants.
- **A spawn locator is an ordinary object with geometry, not an empty and not a dummy.**
  `level01.RIF`'s `Goodie A`..`Goodie D` are `RBOBJECT`s carrying a 24-vertex marker mesh and its
  `camhund` camera plane is a 4-vertex quad — and the id pairing resolves **all 9,313 objects
  across the 563 files**, so no shipped object is geometry-less at all. `object.rif_add` will make
  one from an empty because that is the obvious gesture, but nothing in the assets attests it;
  a small mesh is the choice with evidence behind it. (An earlier draft of this section had it
  backwards, claiming a geometry-less object was "what the shipped files' unpaired objects are" —
  there are none.)

**A rig is authorable too, and the animation half is where the measuring was.** An armature adopts
as the file's `OBJCHIER` tree, nesting comes from `bone.parent` rather than the `rif_path` strings
an import recorded, and each node's `OBJHIERD` is regenerated from the object actually parented to
that bone — so renaming that object (which the UI now offers) follows into the binding instead of
silently breaking the rig. The findings that pin the sequence half:

- **The frame list comes from the F-curves.** It used to be the stored list, merely *sampled* — so
  inserting a keyframe changed what a pose looked like but never how many frames a sequence had,
  and an Action made from scratch produced nothing at all.
- **But a frame's `time` is authored, not computed, so imported keys are anchors.** Only 3,712 of
  the 27,731 non-trivial shipped sequences match `floor(k*65536/n)` and none match
  `k*65536/(n-1)`, so recomputing on every export would rewrite 87% of the game's animation on a
  round trip that changed nothing. A key from the file keeps its exact time; only a new key is
  placed, by interpolating between neighbours. `heads.sequence_times` reproduces all 27,731 exactly
  and takes a clean insertion in all 27,731. The from-scratch fallback scales by the **clip's**
  frame extent, not each bone's — otherwise two bones keyed over different ranges disagree about
  where one moment sits, which is the bug the test caught.
- **`OBASEQHD`'s first field is not `num_frames`** despite AvP's name for it: it is **65536 in all
  29,550** shipped sequences, i.e. the 16.16 span a frame's `time` is a position within.
  `sub_sequence_number` *is* recovered — a **per-file sequence id**, distinct within each of the
  4,270 `OBANSEQS` nodes and identical across nodes for the same name in all 912 (file, name)
  pairs — so a new Action allocates one and every bone's copy shares it.
- **`OBASEQFR.flags` is a sound index**, not opaque: AvP's `HierarchyFrame_SoundIndexMask` decodes
  Gunlok cleanly (bits 0-23 zero in 323,323 of 323,334 frames; index 0 in 322,796 and 1..17 in the
  538 that trigger a sound). So a new key gets **0**, not a copy of its neighbour's — copying would
  duplicate that neighbour's sound.
- **And the index resolves entirely inside the `.rif`** — traced in Ghidra, full write-up in
  `rif_chunk_format.md`. `BuildRifFileObject` @ 0x005a9b50 zeroes a **128-slot pointer array at
  rif+0x10** (matching the 0x7f mask exactly) and installs every `INDSOUND` chunk at
  `rif->sounds[chunk->index]` — the chunk declares its own slot. They are **direct children of the
  file root**, not of `REBENVDT`: the `lookup_child` is on `rif+0xc`, the root File_Chunk, and all
  240 shipped chunks sit at depth 1 under `REBINFF2`. (An earlier revision of this file said
  `REBENVDT`, inferred from an unrelated lookup a few instructions earlier in the same function —
  the measurement, not the neighbouring code, is what settles it.) `MakeHierarchyFrame`
  @ 0x005ae510 does `rif->sounds[idx]` and reads a **path string** off the entry
  (`Robots\GL_click08.wav`) plus min/max distance in mm, volume and a pitch offset. The rif is
  threaded through `GetHierarchy` → `BuildHierarchy` → `BuildHierarchyNode` →
  `BuildSequenceList` → `BuildSequence` for no other purpose than this lookup. So a sound is
  authorable with no external table: emit an `INDSOUND` and reference its index.
  **A dangling index is normal shipped data** — 12 of the 52 files with sound events use an index
  no `INDSOUND` declares, and the engine's null-slot check makes that silent. `SOUNDDIR` is *not*
  on this path; nothing looks that chunk up by id.
- **Bit 31 of `OBASEQFR.flags` marks the sequence's origin frame** — recovered, and *not* ignored,
  which was the standing suspicion. Set in 9,693 frames. `MakeHierarchyFrame` @ 0x005ae74f puts `4`
  into the built frame's own flags (+0x24); `BuildSequence` @ 0x005ae037 scans for the **first**
  frame carrying it, sets flag `0x4` on the sequence (+0x14), and **rebases every frame onto it** —
  subtracting its position, then rotating by the conjugate of its rotation
  (`XOR 0x80000000` on `w`). `OffsetSequenceFrames` @ 0x005945b0 then skips any sequence so
  flagged, where it would otherwise offset every frame. So a sequence with an origin frame is
  stored in that frame's local space; one without is absolute and gets offset at use. Marking the
  wrong frame silently re-anchors the whole animation, which is why the addon preserves it and does
  not yet offer to set it.
- **There are two sound systems, they share nothing, and the addon models them as two different
  kinds of thing.** `INDSOUND` is an indexed **table of definitions** at the rif root that an
  animation keyframe selects from by number, and it plays wherever the animating model is; it
  occurs only in `Objects\` (47) and `Units\` (193). `DUMOBJTX` is a **placement** — a text
  directive on a `DUMMYOBJ` that becomes one looping emitter at that dummy's fixed world position,
  started once from `LoadLevel`; it occurs only in `Levels\` (1,097). The two name **not one file
  in common** (`rif_chunk_format.md`, "`DUMOBJTX` vs `INDSOUND`").
  So the **Speakers are the emitters**, and the table is a table:
  - **`INDSOUND` follows `BMPNAMES` exactly**, because it is structurally the same thing — a
    file-level indexed table whose index is a stable, sparse id meaningful only inside its own
    file, with a payload loaded from the install for preview and never written back. The **table**
    is `rif_indsound` on the collection, kept whole and in order so an entry no keyframe references
    survives; the **index** is `rif_sound_events` on the Action (`{bone, frame, index}`, because a
    sound belongs to a sequence at a time); the **audio** is a `bpy.types.Sound` per entry, given a
    fake user so it survives a `.blend` save, and found again by the `rif_sound_path` stamped on it
    — the same job `rif_rim_path` does for a texture's image. The index is still **split out of
    `flags` on import and spliced back on export**, so the residual bits nobody understands are
    preserved untouched.
  - **It was Speakers, and that was wrong.** `_build_speakers` never set a location, so all of them
    stacked at the world origin (16 of them in `cyberbay.RIF`) and the transform meant nothing — a
    user could drag one anywhere and no byte changed. Tolerable only while Speakers were the sole
    sound thing in the scene; the moment `DUMOBJTX` emitters exist, two object types that look
    identical in the outliner would carry opposite semantics. That is the ambiguity `rif_light`'s
    name gate exists to design out.
  - **The cost is real and was accepted**: no distance slider, no sound file browser, and no
    `template_list`. The last one is measured, not a preference — the table is a plain ID property
    (which is what lets every test drive `scene.py` with the addon unregistered), and
    `template_list` resolves the bracket form `["rif_indsound"]` and then **crashes Blender 5.2** in
    `RNA_property_collection_length`, a null dereference through `rna_property_rna_or_id_get`. A
    background test cannot see that, because nothing there ever draws; it took running a real UI
    Blender under `redraw_timer`. So the panel draws its rows itself and selection is an operator.
    A registered `CollectionProperty` would satisfy `template_list` and would mean a second place
    the table lives.
- **An emitter's storage is its raw text, and the Speaker is a view spliced back into it.** The
  1,097 shipped texts are far more irregular than the three-line grammar suggests: 362 have no
  third line at all, 221 end in a trailing CRLF that leaves an empty one, 29 end in two, and
  `level06.RIF` has one whose directives are split across two lines with a leading space
  (`Sound\r\nloudcreak01.wav\r\nV30\r\n P0 R60`). Reformatting from parsed values would rewrite
  every one of them, so `emitters.retext` replaces **only the argument of a directive whose value
  actually changed**, appends one the text did not carry, and otherwise returns its input. "Changed"
  is decided on the *formatted* argument, not the float, which is what absorbs the float32 round
  trip through a Blender property — a `P2` stored as `2**(2/12)` comes back as 1.9999998 and still
  spells `P2`. Three things fall out:
  - **The directive letters are uppercase-only.** The dispatch @ 0x00481c10 does `ADD -0x49` /
    `CMP 0xd` / `JA`, so a lowercase `v`/`p`/`r` is skipped in silence. All 1,540 shipped ones are
    uppercase, so nothing in the game exercises it — which makes it purely a trap for a generator.
    Line 1 *is* case-insensitive (`lstrcmpiA`, and 14 ship as `sound`), which is what makes it easy
    to get backwards.
  - **`V` is parsed and then discarded** (`game_defects_notes.md` §10): the emitter takes the
    sample's own default. 514 shipped emitters carry one and not one does anything, so the panel
    shows it as inert and writes it for fidelity — the same standing `CUTEVENT` kind 5 has.
  - **A zero distance means the sample's own default, not silence**, so 735 of the 1,097 import with
    a collapsed gizmo. That is the file speaking rather than a loss, and absent-versus-zero stays
    distinguishable because a value equal to the engine's own default for an absent directive is
    never written.
  - **The distance unit is not measured**, and the shipped data does not settle it: `R` takes 5, 10,
    15, 20 *and* 500 and 5000 in the same game. So the number is shown as the number the file holds
    and nothing is scaled.
- **A dummy that is an emitter *is* the Speaker, rather than carrying one.** A dummy is a name at a
  position and nothing else, so there is no second transform for a child object to add and no way
  for one object to drift out of step with itself.
- **Verified in the running game, and the control is what made the result readable.** `level01.RIF`
  exported by this build — 295 regenerated `DUMOBJDT` and 14 `DUMOBJTX`, none of it carried — served
  through the mod VFS loads to `game.state` 5 with its usual 158 actors / 259 roles and
  `mods.served` 6. Since a missing or malformed `DUMOBJDT` is an unchecked null dereference in
  `MapAuxObject_Ctor`, a clean load is direct evidence the dummy chunks are well formed. What it is
  **not** is evidence that the emitters are audible: nothing in the engine exposes the ambient
  emitter list, so there is no assertion to make and that half needs ears.
  The same run on **`level02` crashes, and it is not this change** — see the `SHPMRGDT` entry
  below, which is the diagnosis. Three controls were needed to get even that far, and each one
  would have produced a confident wrong answer if skipped: the **stock** control (without it the
  crash reads as this change's fault), the **pre-change addon** control (without it, as the
  exporter being fine on level01 only by luck), and deleting the **sidecar caches** — `.cut`/`.map`
  are keyed on the `.rif`'s FILETIME, which a VFS-served file need not report, so a stale cache was
  the obvious suspect and was not the cause.
- **A sequence's three optional chunks are *nearly* per-sequence, and the "nearly" is the whole
  design.** `OBASEQTM` (milliseconds), `OBASEQSP` (`{speed mm/s, angle, spare}`) and `OBASEQFL`
  (`Loops` 0x04 / `NoLoop` 0x08, plus an unexplained 0x80 on 181 chunks) are all AvP's and all
  optional — 590, 722 and 582 of 29,550 sequences carry one, each on a *subset* of that sequence's
  bones. For 908 of the 912 (file, sequence) pairs with an `OBASEQTM` every bone agrees, but four
  do not (`game_cursor.RIF`'s `DzSeq_Walk` has 800, 600 and 1000 on three bones) and two disagree
  on flags. **So the file's per-bone body is authoritative and is left untouched**; the Action-level
  property is an *override* applied only to settings the user actually edited (`rif_seq_edited`).
  Two guards, both load-bearing and both proven by deliberately removing them: `rif_seq_edited`
  (without it the divergent files collapse to one value) and `rif_seq_had` (without it a duration
  shipped on three bones of eighty comes back on all eighty — breaks four files).
  Present and absent are distinct states, so these are ID properties that exist or do not, and the
  UI toggles them rather than offering a sentinel.
  **The first design got this wrong**, on a measurement that counted "the bones disagree" and "at
  least one bone lacks it" and found the same number — which does not imply the *present* values
  agree. The full scene round-trip caught it on `Binary Laser MkI.RIF`. Counting two conditions
  that coincide is not the same as testing the one you care about.

Not verified against the game: the import scale (a convention — the engine's factor comes from map
data at level load, not a constant), the Y-up→Z-up assumption, and a *generated* sequence's timing,
which follows the dominant shipped convention rather than a rule read out of the engine. Nothing
exported has been loaded back into Gunlok yet, which matters most for a file built from scratch,
where every chunk is generated rather than carried. A frame's **origin bit is preserved
but never set** — what it does is recovered, but choosing the wrong frame re-anchors a whole
animation inside the engine, where nothing here can see the result. And the **unit** an ambient
emitter's `I`/`R` distances are read in is not measured: the shipped values run 5..60 and 500..5000
in the same game, so the addon shows the number the file holds and scales nothing.
