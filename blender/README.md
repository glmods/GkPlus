# Gunlok RIF for Blender

Import and export Rebellion `.rif` assets — the chunk format Gunlok (2000) uses for all its
geometry. Pure Python, no compiled extension: the addon installs as a plain zip and needs
nothing from the GkPlus DLL.

**The scene is the whole file.** Import builds every chunk into a Blender datablock; **export reads
nothing but the scene** — no source-file parameter, and nothing stored as opaque bytes — so a
`.blend` can be moved to a machine that has never seen the original `.rif` and still produce one.
Import does look outside the file, but only for the pictures and the audio: a texture and a sound
are named by the `.rif` and stored beside it, and neither is ever written back.

```
blender/
  pyproject.toml         uv project: dev tooling and lint config only
  io_scene_rif/          the addon
    blender_manifest.toml
    __init__.py          the operators, panels and properties
    scene.py             chunk tree <-> Blender scene (the only bpy module)
    rif.py               container format: huffman, chunk tree, serialization
    schema.py            chunk bodies as typed fields
    heads.py             the record chunks: names, ids, counts, keyframe timing
    shapes.py            REBSHAPE geometry decode/encode
    bmpnames.py          the file's texture table
    sounds.py            INDSOUND: the file's indexed sound table
    rim.py               .RIM textures: IFF container, DXT1/DXT3 decode,
                         palettized CMAP/BODY both ways
  tools/build_zip.py     builds dist/io_scene_rif-<version>.zip
  tests/
    test_roundtrip.py    container format, runs without Blender
    test_schema.py       typed fields, runs without Blender
    test_shapes.py       geometry, runs without Blender
    test_heads.py        the record chunks and sequence timing, runs without Blender
    test_rim.py          textures and the texture table, runs without Blender
    test_scene.py        the scene round trip, runs inside Blender
    test_authoring.py    building a file from nothing, runs inside Blender
```

Only `scene.py` and `__init__.py` import `bpy`, which is what lets the first five tests run over
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

## Authoring a new file

Export reads the scene and nothing else, so it also reads only what the *importer* put there:
`rif_id`, `rif_index`, `rif_objhead` and `rif_absorbed`. A mesh added with *Add ▸ Mesh* carries
none of those and is silently absent from the file. The operators under *Object ▸ …* mint them.

1. ***Object ▸ New Gunlok RIF*** — an empty collection with the file-level chunks a `.rif` needs
   (`RIFVERIN`, `REBENVDT` with `ENDTHEAD`/`RIFFNAME`/`BMNAMEXT`/`BMNAMVER`), laid out as the
   smallest shipped file carries them. No texture table until a material asks for one.
2. Model as usual, then ***Object ▸ Add to Gunlok RIF*** with the objects selected. A mesh becomes
   an `RBOBJECT` with its own `REBSHAPE` and a fresh shape id; an empty becomes an `RBOBJECT` with
   no geometry; an armature becomes the file's `OBJCHIER` hierarchy; a light becomes a `STDLIGHT`
   in the file's light set. See "Lights" below.
3. For an animated model, parent each mesh to its bone, then mark each Action with
   ***Object ▸ Add Action to Gunlok RIF***. See "Editing and authoring animation" below.
4. *File ▸ Export ▸ Gunlok RIF*.

**Use a small mesh for a spawn locator, not an empty.** `level01.RIF`'s `Goodie A`…`Goodie D` are
ordinary objects carrying a 24-vertex marker mesh, and its `camhund` camera plane is a 4-vertex
quad — and across all 563 files every one of the 9,313 objects pairs with a shape, so a
geometry-less object is something the format permits and the shipped assets never do. The empty
path is offered because it is the obvious thing to reach for; it has no evidence behind it.

You can also add to a file you imported — the usual way to build a level, since the file-level
chunks and the texture table come with it.

### The two identities, in Object Properties ▸ Gunlok RIF

Both live inside chunk bodies stored as `int32` arrays, so neither is visible in Blender's own UI.

- **Name in file** — `OBJHEAD1+0x3c`, and the only name the engine sees. A map section's `name`,
  every `for "<rif object>"` spawn point and an `OBJHIERD` node binding all resolve by `strcmp`
  against it. **Renaming the object in the outliner does not change it** (and the outliner name may
  be uniquified, which is exactly why the two are separate).
- **Shape ID** — `OBJHEAD1+0x38` matched against `SHPHEAD1+0x14`; setting it here writes both
  halves at once. Duplicating an object in Blender copies the id along with everything else, and
  two objects claiming one shape is not representable: re-import pairs both with the first and the
  second mesh is orphaned. Export **refuses** that rather than writing it, and the panel offers a
  fresh id.

A duplicated *name* is fine and is left alone — `RifFilterObjectsByName` returns every match.

Materials and Speakers get a ***Gunlok RIF*** panel for the same reason — the retexturing knob is
the texture *name* and a sound's identity is its *path*, and both were previously reachable only
through Custom Properties.

## How the file maps onto the scene

The rule is: **container chunks become objects, leaf chunks become typed fields on their
parent's object.** On top of that, the things with a real Blender equivalent get one.

| RIF | Blender |
|---|---|
| `RBOBJECT` | an object, with location and orientation from `OBJHEAD1` |
| `REBSHAPE` | that object's mesh datablock |
| `OBJCHIER` | one **armature**, a bone per node, named from `OBJHIERD` (`Waist`, `Chest`, `Hand Right`) |
| `STDLIGHT` | a light — colour, energy, cutoff distance, orientation |
| `OBANSEQC` | an **Action** on that armature, named from `OBASEQHD` (`Seq_Walk`, `Seq_Die`) |
| `OBASEQFR` | a keyframe in it, plus a sound event on the Action if it triggers one |
| `SHPMRGDT` | each pair fused into one **quad**, plus a per-face `rif_merge_pair` attribute |
| `SHPVTINT` | a per-vertex `rif_light` **colour** attribute, paintable and bakeable |
| `BMPNAMES` | the texture table on the collection, plus one material per texture index |
| `INDSOUND` | a **Speaker** per entry, carrying the path, distances and volume |
| polygon `engine_type` / `flags` | per-face attributes |
| everything else | typed `int32`/`float32`/string properties |

Three properties carry the structure: `rif_id` (the chunk id), `rif_index` (position among
siblings — Blender does not preserve child order) and `rif_absorbed` (the leaves folded into a
datablock, so they re-emit in place).

**An object finds its shape by id, not by position.** `OBJHEAD1+0x38` matches `SHPHEAD1+0x14`,
which is what AvP's `Object_Chunk::assoc_with_shape_no` does. Document order disagrees with it in
86 of the 563 shipped files, so pairing positionally attaches geometry to the wrong transform in
one file in seven.

**An object's name is the trailing string at `OBJHEAD1+0x3c`, not the field at 0x04.** That field is
AvP's `lock_user`, the editor's lock holder — `Player` in 6,383 objects, junk in 2,783, non-empty in
8,136 of 9,313; the real names are `Head`, `Waist`, `Chest`, `Upper Arm Right`. `DUMOBJDT` is the
same shape at `0x34`.

**Meshes are parented to the hierarchy node that animates them.** `OBJHIERD` names the object a
node drives, resolved by `strcmp` exactly as AvP does — 3,541 objects and 1,488 dummies, with 221
nodes binding nothing (legal; AvP nulls the pointer). Each node takes the rest transform of the
object it binds, relative to its parent node, and the object sits at identity underneath. Rest pose
is unchanged, so nothing moves on import.

### Animation

**One Action per sequence, on one armature, every one starting at frame 0.** In the file a sequence
is split across the nodes — each `OBJCHIER` carries its own `OBANSEQC` for `Seq_Walk` — so the
scene has to put them back together, and it does: a bone per node, and one Action animating every
bone that takes part. Switching animation is the Action dropdown, and nothing needs an NLA track, a
timeline offset or a preview range.

(An earlier version built the hierarchy as parented *empties*, which forced one Action per node —
`Seq_Walk` then only meant anything with that strip active on all 81 of them at once, laid out end
to end on a shared timeline with a marker each. That is gone; if you find a mention of
`rif_sequence_ranges` or NLA extrapolation anywhere, it is describing the old model.)

**A sequence with no frames is normal**, not a bug: 973 of the game's 29,550 contain none, and the
root node's are usually among them — the animation lives on the child bones (`Waist`,
`Thigh Right`, …).

Export restores each object's recorded rest pose before reading transforms, because animation
evaluation *writes into* an object's transform properties and simply clearing the action does not
put them back.

**Meshes named `L5#…` are LOD variants, and they *are* put on the rig** — on the bone of the part
they replace, at that part's transform, because no `OBJCHIER` node binds a variant and nothing else
would ever animate them. They are hidden (the eye, not the monitor) so they do not double every
part in the viewport and steal its clicks.

### Editing and authoring animation

**The frame list comes from the F-curves.** Insert a keyframe and the sequence gains a frame;
delete one and it loses it. A new Action becomes a new sequence once you mark it with
***Object ▸ Add Action to Gunlok RIF*** (marked, not auto-detected — an Action is not owned by the
object it happens to be assigned to, and sweeping up every one in the file would export whatever
you were experimenting with).

**A key that came from the file keeps its exact time.** The stored `time` values are *authored*,
not computed — only 3,712 of the 27,731 non-trivial shipped sequences match `floor(k·65536/n)` and
none match `k·65536/(n−1)` — so there is no formula to re-derive them with, and recomputing on
every export would rewrite 87% of the game's animation on a round trip that changed nothing. So
imported keys are anchors, and only a key you added is placed, by interpolating between its
neighbours. A sequence authored from scratch falls back to `floor(k·65536/span)` over the **clip's**
frame extent — the clip's, not each bone's, or two bones keyed over different ranges would disagree
about where the same moment sits.

**Bones can be added and re-parented.** Nesting comes from `bone.parent`, and each node's
`OBJHIERD` — the name it drives, resolved by `strcmp` — is regenerated from the object actually
parented to that bone, so renaming that object follows into the rig instead of quietly breaking it.

Several fields are generated rather than carried — `OBJHIERD`'s binding and the whole of
`OBASEQHD` among them — and every generated value is measured rather than invented. The two worth
knowing about:

- `OBASEQHD`'s `sub_sequence_number` is a **per-file sequence id**: distinct among the sequences of
  each of the 4,270 shipped nodes, and identical across nodes for the same sequence name in all 912
  (file, name) pairs. A new Action gets a fresh one, shared by every bone's copy of it.
- `OBASEQFR`'s `flags` carries a **sound index** in bits 24–30 (AvP's
  `HierarchyFrame_SoundIndexMask`; 538 shipped frames trigger one). An imported key keeps its own;
  a new key gets 0, rather than a copy of its neighbour's — copying would duplicate that
  neighbour's sound.

### Sounds: there are two systems and they share nothing

Gunlok has exactly two ways to put a sound in the world. Not a file, a directory, a chunk parent or
a trigger is shared between them — they do not even name one `.wav` in common — so the addon models
them as two different kinds of thing:

| | `INDSOUND` | `DUMOBJTX` |
|---|---|---|
| What it is | an indexed **table of definitions** | a **placement**, one per emitter |
| Lives in | `Objects\` (47) and `Units\` (193) — never a level | `Levels\` (1,097) — never a model |
| Fired by | an animation keyframe | `LoadLevel`, once |
| Position | wherever the animating model is | the dummy's fixed world position |
| Looping | no, one shot per keyframe | yes |
| Audio | `Sound\Robots\`, `Sound\UI\` — servos, clicks, footsteps | `Sound\environ\` — wind, hum, fire, dripping |
| In Blender | a **table** on the collection | a **Speaker** in the level |

#### `INDSOUND` — the table an animation names

A keyframe names a sound by an index into a **128-entry table the file carries itself**: each
`INDSOUND` chunk — a direct child of the file root — declares its own slot and holds a path
(`Robots\GL_click08.wav`), min/max distance in mm, a volume and a pitch offset. No per-role table,
no engine-side name lookup.

This is the same shape as the texture table, and it splits the same three ways:

| | Where it lives | Written back? |
|---|---|---|
| the table | `rif_indsound` on the **collection**, kept whole and in order | yes, rebuilt on export |
| the index | `rif_sound_events` on the Action — `{bone, frame, index}` | yes, spliced into the frame's `flags` |
| the audio | a `.wav` loaded from the install for audition | no — the `.rif` stores the path, never the sound |

Edit it in **Properties ▸ Collection ▸ Gunlok RIF Sounds**, with the RIF collection active in the
outliner: click a row to select it, then edit its slot, path, distances, volume and pitch below.

- ***Object ▸ Add Gunlok RIF Sound*** takes the path the file will store and, optionally, a `.wav`
  to audition. It allocates the next free slot — never 0, which is how a frame says "no sound".
- ***Remove*** drops a row. Entries no keyframe references are otherwise kept: a slot is a stable
  id the file assigned, not a position in a list.
- ***Set Keyframe Sound*** (in the rig's panel, or `pose.rif_set_sound`) puts an index on the
  active bone's key at the current frame; index 0 removes it. The rig panel lists every event.

Two things that look like bugs and are not: a **dangling index is normal shipped data** (12 of the
52 files with sound events reference an index no `INDSOUND` declares, and the engine skips it
silently — so setting one is a warning here, not a refusal), and **`SOUNDDIR`** — despite the name —
is *not* on this path; nothing looks that chunk up by id, and it rides through untouched.

> These used to be Speakers, and it was misleading: the importer never gave them a location, so all
> of them stacked at the world origin and dragging one changed nothing in the file. They have no
> position, because they play wherever the animating model is.

#### `DUMOBJTX` — the ambient emitters, which *are* the Speakers

The retail build's only source of positional looping ambient sound, and the reason a level has wind
and machinery. Each one is a text directive on a top-level `DUMMYOBJ`:

```
Sound
GL_Wind03.wav
V40 P0 R0
```

In Blender that dummy **is** a Speaker, at its real position, where the transform,
`distance_reference` (`I`), `distance_max` (`R`) and `pitch` (`P`, in semitones) all mean something.
Edit it in **Properties ▸ Speaker Data ▸ Gunlok RIF**; ***Object ▸ Add Gunlok Ambient Emitter***
makes a new one at the 3D cursor.

Four things worth knowing, all measured:

- **Volume (`V`) does nothing.** `SoundSystem_AddAmbientEmitter` takes it and never reads that stack
  slot — the sample's own default wins. 514 shipped emitters carry a `V` and not one of them has any
  effect. It is written back for fidelity and the panel says so.
- **A max distance of 0 means the sample's own default, not silence**, which is why most shipped
  emitters import with a collapsed gizmo. The file genuinely specifies no radius.
- **Directives are uppercase-only.** A lowercase `v`/`p`/`r` is skipped by the engine in silence.
  Anything the addon writes is uppercase; line 1 is the exception, being case-insensitive, and 14 of
  the 1,097 shipped chunks say `sound`.
- **The text is carried, not regenerated.** Only a directive whose value you actually changed is
  rewritten, so an untouched level exports byte for byte — including the shipped oddities (a
  trailing CRLF, two of them, and one file whose directives are split across two lines).

The distance unit is **not** measured: the shipped `R` values run 5..60 *and* 500..5000 in the same
game, so the panel shows the number the file holds and scales nothing.

### Locators (`DUMMYOBJ`)

A dummy is a name at a position, and that is the whole of it — the engine builds strings like
`Goodie A2`, `baddie c`, `Flag_3` and `dumpresk` itself and scans for them. ***Object ▸ Add as
Gunlok RIF Locator*** turns a selected Empty into one (or a Speaker, which makes an ambient emitter).

A `for "<rif object>"` spawn point is **not** this: that clause only ever matches an `RBOBJECT`, so
***Add to Gunlok RIF*** is still what you want for one. The two are disjoint namespaces.

Four gates, enforced because the shipped data never violates them:

- **A dummy must carry a `DUMOBJDT`.** One without is an unchecked null dereference during level
  load — an access violation, not a quiet failure. Export refuses it.
- **Top level only**, and **the name must not be empty** (an empty one is stored as `NULL` and no
  consumer can find it). Export refuses both.
- **A dummy is an emitter or a marker, never both**: `ToMap` frees the record it turns into a sound,
  so an emitter's name never resolves. The panel says which one an object is.
- **Duplicate names are legal and shipped** (210 across 62 files) — a warning, not a refusal. Worth
  knowing: the console takes the *first* match where triggers take the *last*.

### Sequence settings

Three optional per-sequence chunks, in the rig's panel with the active Action selected. Each is a
**toggle**, because present and absent are different states — most sequences carry none of them:

| Field | Chunk | What it is |
|---|---|---|
| Duration | `OBASEQTM` | how long the sequence runs, in **milliseconds** |
| Speed | `OBASEQSP` | how fast it moves the model, in **mm/second** — the shipped walks and runs are 1400–3000 |
| Loop | `OBASEQFL` | `Loops` / `Once`, AvP's `SequenceFlag_Loops` and `_NoLoop` |

The file stores each on a *subset* of the bones that have the sequence, and they are **nearly**
per-sequence — for 908 of the 912 files-and-sequences carrying a duration, every bone that has one
agrees. Not all: `game_cursor.RIF`'s `DzSeq_Walk` carries 800, 600 and 1000 on three bones, and
`warflash.RIF` has one sequence flagged `Loops` on one bone and `Once` on another.

So **an untouched sequence keeps the file's own per-bone values**, and editing a field overrides it
on every copy. An untouched export also puts each chunk back on exactly the bones that had it —
without that a duration shipped on three bones of eighty would come back on all eighty.

Only the speed's first field is exposed. `angle` and `spare` are zero in all 582 shipped chunks, so
there is no UI for a heading nothing has ever set — but both are preserved. Likewise every bit of
`OBASEQFL` except the two loop bits, including the `0x80` that 181 chunks carry and nobody has
explained.

Still not authorable: **bit 31 of a frame's `flags`, which marks the sequence's origin frame.** It
is no longer a mystery — the engine scans a sequence for the first frame carrying it and rebases
every other frame onto it, subtracting its position and rotating by the conjugate of its rotation,
then skips the offset pass it would otherwise apply. So a sequence with an origin frame is stored
in that frame's local space and one without is absolute. It round-trips untouched; it is not offered
as an edit because marking the wrong frame silently re-anchors the whole animation, and nothing has
been loaded back into Gunlok to check that against.

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
| the image | a packed Blender image, wired to a Principled BSDF | only if you ask: **Textures** on the export |

**To retexture, edit the texture path** in *Material Properties ▸ Gunlok RIF* and export. Every
polygon wearing that material moves to the named texture, and if the file never mentioned it the
table gains an entry at a fresh index. Assigning a different image to the texture node does not
change what the `.rif` says — that file stores the name, and only the name. (The field writes the
`rif_bmp_name` ID property, which is what it was called when Custom Properties was the only way to
reach it.)

**To change how a texture looks, paint on the image and export with Textures set.** The images are
a *second* output beside the `.rif`, so the export has its own **Textures** dropdown and
**Textures folder**:

| Textures | What is written |
|---|---|
| **None** (default) | just the `.rif`. The names are in it either way |
| **Changed only** | the images whose pixels no longer match the `.RIM` they were imported from |
| **All** | every texture the file names |

Point the folder at a **mod**, not at the install — `<Gunlok>\gkplus\mods\<yours>\graphics` — and a
name like `Units\baddies3.RIM` lands at `graphics\Units\baddies3.RIM`, which is exactly where
GkPlus's mod loader looks for it. Pointing it at the install overwrites the real assets, which is
allowed but is your call to make; nothing here asks twice.

Written textures are **palettized, not DXT** (`CMAP` + `BODY`), which the engine reads first and
prefers. That is exactly lossless — one palette entry per distinct colour, no quantization, no DXT
block artifacts — at two to six times the size of a DXT file. **Compress textures** packs them with
ByteRun1, which 36 of the shipped textures use; turning it off writes the raw form that has been
verified end to end in the running game.

**One exception, and it is silent: a texture with graded (partial) alpha comes out opaque
in-game.** The palettized form can only carry that alpha in an `ALPH` chunk, and Gunlok ignores
the chunk — the file is correct, the engine drops it. Cut-outs are fine when every fully
transparent texel sits on the same RGB, which is the usual case. If you need a soft alpha edge,
convert that one texture with `utils\rimutil compress --format dxt3` instead; the addon cannot
write DXT.

"Changed only" re-reads the pixels and compares them against a digest taken at import, so it is
about the *image*, not about whether Blender thinks the file is dirty — paint, save the `.blend`,
reopen, and the edit is still recognised. An image the addon did not import has no digest and is
always written.

That panel also shows the **UV scale** the export will use, and warns when it is 1×1 — a material
naming a texture whose size is unknown writes every UV as a single texel, which looks like nothing
at all in-game and is otherwise invisible.

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

**Load textures also gates the audio**, which is worth knowing because the option does not say so:
with it off, the sound table still imports and its speakers are still created, they just have no
`.wav` loaded. The `Sound` folder is found the same way `Graphics` is, by searching upwards, and
has no override of its own.

`.RIM` is not a RIF chunk file. It is IFF (big-endian, `LIST`/`FORM`/`PROP` groups) carrying the
image either as DXT1/DXT3 in an `S3TC` chunk or as a palette and planar bitplanes (`CMAP` +
`BODY`), plus a mip chain the importer discards since Blender makes its own. Both forms are read —
23 of the 513 shipped textures are palettized, and the game prefers that one where a file has
both. The images are packed rather than pushed in through `pixels`, because Blender keeps a
*generated* image's settings across a `.blend` save but not its pixels.

`rim.py` also **writes** the palettized form, which is what the export's Textures option uses. The
pixels come back out through `image.pixels`: for an 8-bit image that is the stored byte over 255
with no colour management in the way, so an untouched texture reads back byte for byte identical
to the `.RIM` it was decoded from — measured on Blender 4.2 and 5.2, across a `.blend` save and
reload, alpha included.

### Lights

**Gunlok does not read them.** Editing or adding a light produces a correct `.rif` — the chunk is
written exactly as the shipped ones are — but the shipped engine ignores the whole lighting family
(`LIGHTSET`, `LTSETHDR`, `STDLIGHT`, `AMBIENCE`, `PLOBJLIT`, `LITSCALE`). Each of those chunk ids
is referenced only from its own loader and registration in gl.exe, while every chunk the engine
*does* consume — `SHPVTINT`, `INDSOUND`, `REBENVDT`, `OBJHIERD`, `SHPPOLYS` — shows a consumer
outside its own translation unit. Runtime lighting comes from `SHPVTINT`, the **baked per-vertex
intensity** the importer exposes as a `rif_light` colour attribute, plus the sun globals. The
placed lights are editor-time data that produced those baked values, so to change how a level looks
you edit the vertex attribute, not the lights. Full evidence in `rif_chunk_format.md`.

They are still worth round-tripping: they are the authored record of how a level was lit, and
exporting a file that drops them would lose that.

#### Changing how a level actually looks

That is the `rif_light` attribute, which the importer puts on the mesh and the engine does read. On
the wire it is **one packed `0x00RRGGBB` colour per vertex** — not a scalar and not 16.16; no
shipped value even falls in 0..65536. The commonest shipped values are `0xFF080808` (a dark grey)
and `0xFFFFFFFF` (white).

**It is used undecoded.** `BuildShapeVertexBuffers` copies the value straight into the vertex's
D3DCOLOR `diffuse` and ORs alpha to `0xFF`; a mesh with no `SHPVTINT` gets `0xFFFFFFFF`. So the
stored RGB *is* the vertex colour, the **top byte is ignored**, and Gunlok does **not** do the
`sqrt((r² + g² + b²) / 3)` reduction AvP's software renderer uses. The final pixel is
`texture × diffuse` — `Mat_Opaque` sets `COLOROP = MODULATE`, `ARG1 = TEXTURE`, `ARG2 = DIFFUSE`.

In the scene it is a **`BYTE_COLOR` point attribute** — the form the viewport draws, Vertex Paint
edits and a Cycles bake writes into. There is no packed mirror and no packing step: what you paint
*is* what exports.

1. *Object Properties ▸ Gunlok RIF ▸ Vertex lighting ▸* ***Enable Vertex Lighting*** — gives the
   mesh a white `rif_light` if it has none and makes it the **active** colour attribute. A mesh
   imported from a file that had a `SHPVTINT` already has one.
2. Paint it, or bake into it: *Render ▸ Bake*, **Bake Type: Diffuse**, contributions **Direct** and
   **Indirect** (uncheck Color), **Output ▸ Target: Active Color Attribute**. Light the scene with
   ordinary Blender lights — the RIF's own lights play no part in this. Because `rif_light` is the
   active attribute, that bake lands directly in the exported value.
3. Export.

Storing the paintable form is only safe because it is **lossless**, which took measuring: reading a
`BYTE_COLOR` through Blender's `color` property converts sRGB↔linear and loses a least-significant
bit on 157 of 256 values, while `color_srgb` is the stored byte and round-trips 256/256 exactly,
alpha included. The addon uses `color_srgb` throughout. Checked end to end against the shipped
files: 1,086 of 1,087 `SHPVTINT` chunks across a 25-file sample come back **byte-identical**, and
the one that does not is not a loss — `level05_shadow.RIF` ships 13,098 values for a 13,016-vertex
shape, so the tail no vertex indexes is dropped and `num_vertices` corrected. Only two objects in
all 563 files are like that (`level15_shadow.RIF` is the other).

Two rules make that storage safe to trust, and both are worth knowing before you fight them:

- **Export reads `rif_light` by name, and nothing else.** Never the *active* colour attribute —
  that is Blender-wide UI state a bake, a preview or any other feature can repoint, and the file's
  lighting is not something any of them get to decide. If a bake landed somewhere else, or you
  built a colour attribute yourself, ***Use Active Color Attribute*** folds it in deliberately.
  That is the only place a **corner-domain** attribute is averaged per vertex (the file has one
  value per vertex and cannot express a per-corner one) and the only place values above 1.0 clamp.
  A bright lamp therefore saturates to white — if a bake comes back flat white, lower the lamp's
  energy rather than assuming it failed.
- **Export refuses a `rif_light` it cannot read as one value per vertex**, rather than averaging or
  padding it into something plausible. That is what a mesh edit outrunning its lighting looks like,
  and silently writing a partial `SHPVTINT` is worse than not writing one. The panel says so, and
  the export operator refuses before it writes anything.

Having the attribute is not the same as having lighting: the *marker* is the chunk's own header,
kept on the mesh, so a `rif_light` minted purely so the preview can render an unlit mesh does not
put a chunk in the file. Deleting the attribute from a mesh that is marked does not fail — export
drops the chunk and says so as a **warning**, since a chunk that used to be in the file no longer
is.

Two more things the addon handles, both of which otherwise silently produce a file the engine
ignores or misreads:

- **The chunk is selected by name.** An object may carry one `SHPVTINT` per light set and the
  loader takes the one matching the active set's `LTSETHDR` (`strncmp(..., 8)`). Gunlok ships
  `NORMALLT` everywhere, so a mesh newly given lighting gets that name rather than zeros — a
  zero-named chunk is never found. That name lives in the marker, which is why enabling lighting
  is a deliberate act rather than a side effect of creating the attribute.
- **`num_vertices` in its header is trusted** — the engine allocates and iterates that many times,
  so a stale count reads past the chunk. Export regenerates it from the mesh, like `SHPHEAD1`'s
  counts. (Before this it was carried, so editing a mesh wrote a chunk that disagreed with itself.)

#### Seeing it the way the game will draw it

Painting `rif_light` and *looking* at the result are separate problems, and Blender's Solid mode
cannot do the second one: `View3DShading.color_type` is a single choice — vertex colour **or**
texture, never both — and the shading struct carries no blend or multiply option to combine them.
So *Object Properties ▸ Gunlok RIF ▸ Vertex lighting ▸ **Preview As In Game*** (also *Object ▸ Set
Up Gunlok Preview*) builds a material that does what the engine does, and puts the viewport in
Material Preview.

It is reversible. Every preview material records the authored material it replaced — as an ID
pointer, so renaming cannot orphan it — and the scene records the colour management the setup
changed. The **↺** button beside it, or *Object ▸ Restore Authored Materials*, puts both back. Your
own materials are never edited or deleted, and the preview material is regenerated on demand rather
than being something to maintain.

What it sets up, and why each part is not optional:

- **`texture × rif_light`, multiplied on the stored 8-bit numbers.** This is the part that is easy
  to get wrong and impossible to eyeball. The obvious material — Color Attribute × Image Texture →
  Emission — multiplies in **linear** space, which would be equivalent only if sRGB were a pure
  power law. It is not: it has a linear toe below 0.04045. Measured against the 8-bit reference the
  naive graph is wrong by up to **7.43/255**, and it is worst exactly where Gunlok's lighting lives
  (2.30 LSB at light `0x08`, 6.49 at `0x20`, 7.43 at `0x40`, and 0 only at `0xff`). Feeding it a
  pure 2.2 power law instead gives 0.0 error, which is what identifies the toe as the culprit.

  So the preview multiplies the *stored* values and converts once at the end. Both the texture and
  the colour attribute reach the multiply through an exact **linear→sRGB encode**, which recovers
  `byte / 255` from what Blender hands a shader — an sRGB image and a `BYTE_COLOR` attribute are
  both sRGB storage that gets linearised on read — and the product is decoded back to linear before
  Emission, since the view transform will encode it again on the way to the screen. Each conversion
  is the **exact piecewise** function built from Math nodes: Blender's `Gamma` node is a pure power
  law and would reintroduce the very error being avoided, and the shader node set has no
  colour-space conversion node at all.

  The image's colour space is **read, never written**. Setting it to Non-Color would get the raw
  bytes in one step, but an image is a *shared* datablock — so that would leave your own materials
  rendering a linearised texture as though it were raw once the preview is restored. An image
  already tagged Non-Color is used directly.

  Measured end to end by rendering, this reproduces `texel × light / 255` with **0.00 LSB** error
  on every channel, against 2.5–5.6 for the naive graph on the same inputs.
- **Emission, so nothing re-lights it.** That also makes the world irrelevant — verified as
  identical pixels at world strength 0 and 20 — so the setup leaves your scene lighting alone.
- **View transform Standard**, with Look, Exposure and Gamma neutralised. Filmic and AgX are tone
  mappings: they would shift the midtones and roll off the highlights instead of clamping at 1.0
  the way the game does.
- **A `_shadow` object casts and is not drawn.** `level01_shadow.rif` and its 24 siblings are a
  low-polygon stand-in used only to build shadow volumes, so the preview turns camera visibility
  off and shadow visibility on for them — which is the role the engine gives them. Untick *Shadow
  objects cast only* in the operator's redo panel if you want to look at the hull itself.

A material whose `.RIM` is not installed, or which is the `0xfff` untextured sentinel, has nothing
to sample, so it shows the lighting alone — which is what `COLORARG1` with no texture amounts to.
A mesh with no `rif_light` yet gets a white one — the engine draws an object with no `SHPVTINT`
with a white diffuse, and a shader reading an attribute that does not exist would render it black.
That white attribute is **not** lighting: the preview never sets the marker, so looking at an unlit
mesh does not give it a chunk. A mesh that already has one is left alone, since its lighting is
precisely what you are trying to look at.

This is checked numerically rather than visually: `tests/test_authoring.py` renders a known texel
against a known light and compares against `texel * light / 255`, and renders the naive linear
graph beside it and asserts that one **fails** the same tolerance — so the check cannot start
passing again if the node tree is ever simplified back.

#### Authoring a light

A light is a Blender light, and *Add to Gunlok RIF* adopts one like any other object. What makes
it different from a mesh is where it goes: **every one of the 3,794 `STDLIGHT` chunks in the game
is a child of a `LIGHTSET`, and every one of the 62 light sets is a direct child of the file-level
`REBENVDT`.** There is no such thing as a light anywhere else, so adoption puts it there — creating
the `LIGHTSET` (with the `LTSETHDR` and `AMBIENCE` leaves every shipped one carries) the first time
a file needs it, and lifting `REBENVDT` from a stored path into a real object to hold it. A second
light joins the same set. Deleting the object is how a light is removed.

The light datablock carries what it can express, and the panel shows the rest:

| Chunk field | Blender |
|---|---|
| `colour` | the light's **Color** |
| `brightness` | **Power**, as a plain multiplier — see below |
| `range` | **Custom Distance**, in metres, divided by the file's scale |
| position, orientation | the object's transform |
| `spread`, `flags`, `local_flags`, `light_id` | `rif_*` properties |

Two of those need care:

- **Power is not watts here.** The chunk holds a 16.16 multiplier and every shipped light lands
  between 0.2 and 2.0, so Blender's own 1000 W default would export a brightness a thousand times
  past anything in the game. Adoption sets it to 1.0 once — that is the only moment it can be told
  apart from an edit — and after that the number you type is the multiplier the engine gets.
- **Custom Distance must be on**, or `range` exports as 0 and the light reaches nothing. Adoption
  turns it on; the panel warns if it is later switched off. Shipped ranges run 3 m to 357 m.

`light_id` is allocated fresh per light because it is **unique within a file in all 38 that have
lights** — but not `0..n-1`, which only 10 of the 38 are, so it is an id the editor handed out
rather than a position. Duplicating a light in Blender copies the id along with everything else,
which no shipped file does; the panel says so and offers a fresh one. Unlike a shared *shape* id
this is a warning rather than a refusal, because whether the engine minds is not measured.

### Cutscenes

A level `.rif` can carry cutscenes — a camera path, a cast of participants and a timed event
list — and the addon imports them as real objects you can edit, or creates new ones. Fourteen of
the shipped levels have them; `level01.RIF` holds the four `PLAY CUTSCENE` reaches by name,
including `first contact`.

**Add Cutscene** (Object Properties ▸ Gunlok Cutscene, or the Add menu) makes a camera, an empty
for it to look at, and the event that ends it. Key the camera's **location** to build the path —
each keyframe is one control point.

Four things are worth knowing before you author one, because none is guessable from the file:

- **The camera's rotation is ignored.** The engine points the camera by looking from one track at
  another, so orientation comes from where the *target* empty is, not from how you rotate the
  camera. Give the camera a **Track To** constraint aimed at the target and animate both.
- **The path is a spline, and the viewport does not show it.** Your keyframes are Catmull-Rom
  control points, so the real path bows away from the straight line between them — on the shipped
  cutscenes by a median 5.6% of the distance between control points, and by as much as 47% (three
  and a half metres). **Preview Cutscene Path** draws the curve the engine will actually follow.
  Keys are set to Linear rather than Bezier so nothing pretends otherwise.
- **One frame is 40 ms**, the engine's tick. Cutscene paths are keyed at 25 fps so a keyframe
  number converts to a duration exactly; the timing you see is the timing you get.
- **A cutscene with no end event never ends.** Running off the end of the path leaves the camera
  parked and the player locked out — it is not a natural stop. `Add Cutscene` includes the end
  event; the panel warns if one goes missing.

**Add Cutscene Event** offers the end event and a *console command*, which is the powerful one: it
queues a console line mid-scene, so a cutscene can trigger anything the console can.

Finally, and easiest to miss: **chunks in the `.rif` are not enough to make a cutscene playable**.
`PLAY CUTSCENE` searches a list the level's script builds, so the level's `.gls` also needs

```
camera track
{
	file "levels\mylevel.rif"
	name "my cutscene"
}
```

with `name` matching exactly. From GkPlus's script layer the equivalent is
`make.camera_track({name, file})` during a level load.

What is not authored here: a participant's *animation* is chosen from the sequences its own model
ships, triggered by events, rather than keyed in Blender; and cutscene sound events (`CUTEVENT`
kind 5) are written for fidelity but never fire — the retail engine plays one Bink track per
cutscene instead.

## Semantic, not byte-exact

The bar is that the file coming out *means* the same as the file going in, which is what lets the
exporter regenerate rather than mirror:

- `SHPCENTR` is recomputed from the vertices.
- `SHPHEAD1`'s derived half — `num_verts`, `num_polys`, `radius` and the bounding box — is
  recomputed too, and its associated-object name list is rebuilt from the object that owns the
  mesh. That chunk is not just an id: AvP's loader reads all six values straight into its shape
  record, and Gunlok derives a role's collision extents from the bounds when the GLS gives
  `radius`/`height` as 0 — so carrying it through leaves it wrong the moment a vertex moves.
  Its authored half (`flags`, `lock_user`, `version_no`) is kept.
- `SHPPCINF` is discarded — nothing in Gunlok reads it. Its string has three referrers in the
  binary: its registration, its loader, and the function that *deletes* it once the shape is
  built. 681 of the 9,357 shipped shapes carry none anyway.
- `SHPMRGDT` and `SHPVTINT` are authored per-element data, so they ride as mesh attributes and
  survive an edit instead of going stale. `SHPMRGDT` names *another polygon*, so it rides as a
  **pair id** (`rif_merge_pair`) rather than the index the file stores — an index would not
  survive this addon renumbering polygons, and export validates the rebuilt table before writing
  it because the engine's merge pass has no bounds check.

Nothing is silently dropped when you edit a mesh.

### Quads

**A merge pair is a quad, and that is how it imports.** `SHPMRGDT` names pairs of coplanar
triangles the engine fuses back into four-sided polygons when it loads level geometry, so the mesh
you get is the one the level was modelled as: 99.77% of the 580,774 pairs in the shipped files come
in as quads, and level01's 29,045 triangles become 19,675 faces. Turn it off with **Merged pairs as
quads** in the import options if you would rather see the triangles.

On the way out **every face is triangulated**, whatever it is in Blender, because that is what all
1,766,071 shipped polygons are — and a quad becomes a merge pair. So the round trip is even, and
two other things follow:

- **Joining two triangles into a quad creates a merge pair**, and splitting a quad back into
  triangles keeps it (both halves inherit the pair id). Nothing has to be edited by hand.
- **An n-gon is not a pair.** The engine's merged polygon is always four-sided, so a face with five
  or more corners exports as a fan of unpaired triangles. It is still valid geometry; it is just
  more pathfinding sections.

What a pair costs or saves is **nav sections, not pixels** — see the limitations at the end.

A pair that cannot be a quad without losing something stays two triangles: a different material,
`engine_type` or `rif_flags` on the two halves, a UV seam across their shared edge, opposite
winding, or a quad Blender would tessellate along the other diagonal. All 1,311 of them across the
shipped set still round-trip through the `rif_merge_pair` attribute.

The **texture table is the exception, and is held to byte-exactness**: it is carried whole rather
than regenerated, so an import/export cycle that touches no material has no reason to disturb a
byte of it — uninitialised padding after a name included. `test_scene.py` asserts exactly that. The
sound table is held to the same bar, for the same reason.

### UVs move by a hundred-thousandth of a texel, and that is the floor

A UV survives the round trip through one inexact step. Scaling is exact — every texture in the game
is a power of two, so dividing by the width and multiplying back is bit-identical in float32 — but
the **V flip is not**: `1 - v/h` moves a fractional value to an exponent where float32 has fewer
bits left for it. Measured across the shipped files the worst drift is **3.05e-05 texels**, median
1.9e-05, i.e. about 3e-8 of the image.

That is below anything the format or the renderer can represent, so the exporter is left alone. What
it does mean is that **any comparison of UVs needs a tolerance, and rounding is not one.** The scene
test used to round both sides to a hundredth of a texel, which buys a tolerance everywhere except at
the bucket boundaries — and a value sitting within 3e-5 of a `.xx5` boundary lands on either side of
it and reads as a whole hundredth of difference. `Skeleton.RIF`'s `skull` has 11 such coordinates and
was the only shipped file to trip it; **170 of the game's 9,128,082 UV coordinates (0.0019%) sit
that close to a boundary**, so the other 562 files were lucky rather than immune. The test now keeps
full precision and reconciles the residue within `UV_TOLERANCE` (1e-3 texels, 30× the observed
worst case).

**The exporter's one-`SHPUVCRD`-entry-per-triangle is not a defect either**, though it looks like
one beside a source file whose entries several polygons may share: the entry is addressed by an
index, nothing requires the list to be minimal, and a shape's UV *values* are what round-trip. It
does make the chunk larger than it needs to be — deduplicating identical entries would be a size
optimisation, not a correctness fix, and it would have to keep the four >65,535-entry shapes below
the 20-bit index limit.

## Testing

```bash
python blender/tests/test_roundtrip.py "<Gunlok dir>"
python blender/tests/test_schema.py    "<Gunlok dir>"
python blender/tests/test_shapes.py    "<Gunlok dir>"
python blender/tests/test_heads.py     "<Gunlok dir>"
python blender/tests/test_rim.py       "<Gunlok dir>"
```

```bash
blender --background --python blender/tests/test_scene.py -- "<Gunlok dir>" all
```

```bash
blender --background --python blender/tests/test_authoring.py -- "<Gunlok dir>"
```

`test_scene.py` is the one that matters for self-containment: it builds the scene, saves a
`.blend`, **resets Blender**, reopens the `.blend`, and exports from that — the source `.rif` is
never touched during export. If the scene were not self-contained, that is the phase that fails.

`test_authoring.py` covers the other direction — scenes that were never imported — and does the
same `.blend` reset for the name and shape-id edits, which is what proves they landed in the stored
chunk body rather than in a Python-side cache. Its Gunlok directory argument is optional; without
one it runs only the from-scratch groups and skips the four that edit a real imported file.

## What is verified, and what is not

Measured across all 563 shipped files:

| Claim | Evidence |
|---|---|
| Container format is complete | 563/563 parse, re-serialize byte-identical |
| Chunk bodies survive as typed fields | `encode(decode(body)) == body` for 485,663 leaf chunks across 44 ids |
| Every polygon is a triangle | 1,766,071/1,766,071, three valid indices, `-1` in both spares |
| Object↔shape pairing | id match resolves all 9,313 objects, no shape claimed twice |
| `OBJHEAD1` layout | all 9,313 decompose into AvP's fields and rebuild byte-identically (padding aside); the 16 bytes at 0x04 are `lock_user`, non-empty in 8,136 |
| `SHPHEAD1` derived fields | recomputing from the geometry reproduces `num_verts`, `num_polys` and both bound corners for 9,357/9,357 shapes; `radius` is bit-exact for 42% and drifts a median 7e-7 relative (p90 5e-4, 4 shapes past 1%) |
| `SHPHEAD1`/`SHPCENTR` agree on `radius` | byte-identical in all 9,244 shapes carrying both — which is why both are regenerated from one formula |
| A sequence re-times to itself | `sequence_times` reproduces the stored `time` of all 27,731 non-trivial sequences, and inserting a key between two of them stays ordered and disturbs neither, in 27,731/27,731 |
| `sub_sequence_number` is a per-file sequence id | distinct within each of the 4,270 `OBANSEQS` nodes, and identical across nodes for the same sequence name in all 912 (file, name) pairs |
| `OBASEQHD`'s first field is not a count | 65536 in all 29,550 sequences — the 16.16 span a frame's `time` is a position within, despite AvP naming it `num_frames` |
| `OBASEQFR.flags` is a sound index | AvP's masks decode it: bits 0–23 zero in 323,323 of 323,334 frames, sound index 0 in 322,796 and 1–17 in the rest |
| Bit 31 marks the sequence's origin frame | outside both AvP masks, set in 9,693 frames; exactly one `TEST` on the built frame's `+0x24` in the whole binary, in `BuildSequence`, which rebases the sequence onto it |
| The sound table is in the file | 240 `INDSOUND` chunks across 52 files, all direct children of the root, `encode(decode(body)) == body` for every one |
| `OBASEQTM`/`FL`/`SP` are *nearly* per sequence | 908 of the 912 files-and-sequences carrying a duration agree across every bone that has one; four do not, and two disagree on flags — so the per-bone value is authoritative |
| `OBASEQFR` is a keyframe | unit quaternion in 100.000% of 323,334; time non-decreasing in all 28,577 sequences |
| `STDLIGHT` orientation | orthonormal 3×3 in 16.16 for 100.00% of 3,794 lights |
| Face winding | recomputed normals agree with `SHPPNORM` on 99.91% of 1.77M faces |
| Texture table rebuilds exactly | `encode(decode(body)) == body` for all 527 tables, 1,601 entries |
| A polygon's texture index is a table `index` | resolves 1,518,963 of 1,766,071 polygons; of the remaining 247,108, 22,331 are the `0xfff` untextured sentinel and 215,517 of the other 224,777 are in `_shadow` files |
| `.RIM` decoding | 490 of the 513 shipped textures decode; the other 23 are the palettized `*_fmv_*` set, which carries no S3TC image. Of the 365 a `.rif` actually names, 361 are DXT1, 3 DXT3, 1 is `*_fmv_*` and 4 are missing from the install |
| UVs are texels, not fractions | the 99th percentile of `\|u\|` is within 7% of each texture's own width, at every size in the game; 374,658 of 376,641 sampled pairs are whole numbers |
| A UV survives the round trip to 3e-05 texels | worst drift 3.05e-05 across the shipped files, median 1.9e-05 — all of it the V flip, since the scaling is bit-exact |
| V grows downward | 86.3% of the 8,916 axis-aligned wall polygons across the levels put the low V at the top of the wall; 16 of 17 levels lean that way |
| UV index survives `colour` | 1,766,071/1,766,071 re-encode exactly, including the four shapes whose table needs more than 16 bits (282,412 of their 282,454 polygons index their own position) |
| Cutscenes survive becoming objects | all 14 cutscene-bearing levels, 34 cutscenes, 2,517 chunks byte-identical through import → `.blend` → reset → reopen → export |
| A cutscene's id is its name | `MD5("Cutscene:" + name)[0:8]` with the top byte of the second dword cleared reproduces the stored value in 34 of 34 |
| A point's time is a duration in ticks | all 763 shipped point times are a multiple of 40 ms; the final point of a multi-point track is 0 in 94% against 38% elsewhere, which no cumulative timestamp could be |
| Field of view survives the camera lens | degrees → radians → degrees is exact through float32 for all 139 shipped `CUTTRFOV` values |
| `SHPMRGDT` is an exact involution | all 9,357 shipped shapes satisfy the predicate the engine's merge pass assumes, and all 9,357 reproduce their wire values byte for byte through the pair-id form the scene stores |
| A merge pair is a quad, losslessly | 579,463 of the 580,774 pairs in the shipped files fuse into one, and every one of them tessellates back into exactly the two source triangles — same corners, same UVs. The 1,311 that do not fuse stay two triangles and keep their pairing |
| Quads survive the game | `level02` imported as 6,468 quads, exported with the same 6,468 merge pairs, and loaded from a mod with 178 actors / 294 roles and no crash — identical to a stock run in the same session with the mod moved out of the tree |
| The merge fix works where it failed | across the 24 level map objects it turns 15 unwalkable tables into 0, losing 73 pairs of ~320,000 — only where the partner face was genuinely dropped. `level02` and `level11`, which crashed Gunlok on load, now load with 178/294 and 317/352 actors and roles |
| The two sound systems do not overlap | `INDSOUND` occurs in 52 files under `Objects\`/`Units\` and `DUMOBJTX` in 24 under `Levels\`, with no file and not one `.wav` name in common |
| `DUMOBJTX` rebuilds exactly | `encode(decode(body)) == body` for all 1,097, and all 1,097 are padded to `(strlen + 4) & ~3` with NULs |
| An emitter survives a `.blend` | `level01` (14), `junkyard` (87) and `level06` (the one whose directives split across two lines) come back byte-identical through import → `.blend` → reset → reopen → export, position and name included |
| Every shipped dummy satisfies the authoring gates | 6,847 of 6,847 at depth 0, 6,847 of 6,847 with a `DUMOBJDT`, 0 empty names — and 210 duplicate names across 62 files, which is why that one is a warning |
| The chunk inventory balances | every chunk id comes back with the same count it went in with, across the sampled files — the only id allowed to change is `SHPPCINF`, which is discarded on purpose. This is what caught `DUMOBJDT` being written twice per dummy (590 where `level01.RIF` has 295) and `SHPCENTR` being invented for shapes that shipped without one |
| **An exported level's dummies and emitters load in Gunlok** | `level01.RIF` exported by this build — 295 regenerated `DUMOBJDT` and 14 `DUMOBJTX` — served through the mod VFS loads to `game.state` 5 with its usual 158 actors / 259 roles, `mods.served` 6 naming the level. A malformed or missing `DUMOBJDT` is an access violation in `MapAuxObject_Ctor`, so a clean load is direct evidence the dummy chunks are well formed. **Not** evidence that the emitters are audible — nothing in the engine exposes the ambient emitter list, so that half needs ears |
| **An exported cutscene plays in Gunlok** | `level01.RIF` exported and served through the mod VFS: the level loads identically (158 actors / 259 roles) and `PLAY CUTSCENE first contact` renders at MAE **7.95** against the stock build, where two *stock* runs differ by **7.35** — i.e. inside the run-to-run noise. The control moved every camera path 3 m and rendered at MAE **24.62**, 3.3x the floor, so the test can fail. Renaming the cutscene inside the served file stops `PLAY CUTSCENE` finding it at all, which is what proves the engine reads the name and hash this addon regenerates |

Not verified:

- **Most of what is exported has still not been loaded back into Gunlok.** Four levels have been
  (see the `SHPMRGDT` limitation above, which is what that testing found), but the general case is
  checked against the format and against itself, not against the game. It matters most for a file
  built from nothing, where *every* chunk is generated — start by exporting an untouched import
  and loading that, before trusting a hand-made one. The `SHPPCINF` drop, which used to be listed
  here as reasoned-not-measured, is now measured: nothing in gl.exe reads that chunk.
- **An authored `DUMMYOBJ` has not been loaded by Gunlok.** The gates it must satisfy are measured
  and are enforced (see *Locators* above), and an exported `level01` full of *regenerated* ones does
  load (above) — but a dummy that never existed in a shipped file has not been through the engine.
- **A `.blend` saved before the polygon-merge fix exports without `SHPMRGDT`.** That chunk pairs
  coplanar triangles so the engine can draw them as one quad, and the old build stored the pairing
  as a raw polygon index — which broke the moment a face was dropped, and crashed Gunlok on load
  for 15 of the 24 shipped levels. It is now a **pair id** on `rif_merge_pair`, and an old
  `.blend` has no attribute under that name.

  The consequence is safe and almost invisible, though not quite "cosmetic": the merge feeds the
  **navmesh**, not the renderer — the engine builds its draw buffers from the unmerged triangles
  and never rebuilds them — so an unmerged pair is one extra pathfinding section, not one extra
  drawn triangle. Walkability is unchanged (a pair only merges if both halves already share their
  flags, and the quad inherits one of the two planes). **Re-import the `.rif` to get the pairing
  back** — the old values are not converted, because they are indices into a numbering the
  importer had already changed and converting them would produce a plausible, wrong pairing.
- **An ambient emitter's distance unit is unknown.** `I` and `R` reach the sound system as
  distances, and the shipped `R` values run 5, 10, 15, 20 … 60 *and* 500 and 5000 in the same game,
  which no single unit explains. The addon shows the number the file holds and scales nothing, so a
  round trip is exact either way — but a value you *author* is a guess about which convention that
  level's designer used.
- **A generated sequence's timing is a convention, like the import scale.** Where an imported key
  sits is carried exactly, but a *new* key's `time` comes from `floor(k·65536/span)` — the dominant
  shipped generator, not a rule read out of the engine. A from-scratch sequence also carries no
  `OBASEQTM` unless you add one, and whether the engine wants a duration is untested.
- **A frame's origin bit is preserved, never set.** What it does is recovered (see above), but
  choosing the wrong frame silently re-anchors a whole animation and the effect happens inside the
  engine, where nothing here can see it.
- **A from-scratch cutscene has not been played in Gunlok.** An *exported* one has (see below), so
  the chunks the addon writes are known to drive the engine — but every cutscene tested so far
  began life in a shipped file. The pieces most worth doubting when you author one are the ones the
  format cannot settle: whether a from-scratch participant needs a `CTUSRHIE` (every shipped one
  that names no live actor has one), what a sensible `CUTSCDAT` position is (gl.exe never reads it,
  so this writes zeroes), and the event *payload* fields beyond the kind and its first word.
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
- **21 chunk ids still use the generic typed fallback** rather than named fields — they round-trip
  exactly, but their contents read as a `data` int array rather than meaningful names. `OBJHEAD1`'s
  tail, `OBASEQHD`, `DUMOBJDT`, `SHPHEAD1`, `OBJNOTES` and `AVPSTRAT` are the notable ones.
  (`BMPNAMES` and `INDSOUND` are in that count too, but only because they are decoded by their own
  modules rather than by `schema`.)
