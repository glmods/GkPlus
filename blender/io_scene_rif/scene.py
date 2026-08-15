"""Chunk tree <-> Blender scene. The only module that needs ``bpy``.

The scene is the whole file: every chunk lands in a Blender datablock, and export
reads nothing but the scene. There are no opaque byte properties anywhere -- a
chunk body becomes typed fields via :mod:`schema`, geometry becomes a mesh,
lights become lights, and animation becomes an Action.

**The scene does not mirror the chunk tree** -- it only has to be able to rebuild
one. A chunk earns a Blender datablock when it *is* something a 3D tool works
with; everything else is data on whichever datablock owns it, however deeply the
file nests it. ``OBINTDT`` was 9,313 empties across the game holding nothing but a
note string, and the cutscene containers several hundred more.

Three properties carry the structure:

``rif_id``
    the 8-character chunk id this datablock stands for.
``rif_index``
    its position among its parent's children. Blender does not preserve child
    order, so this is not optional.
``rif_absorbed``
    everything folded into this datablock, as ``{path, fields}`` pairs. A path is
    ``id:index`` segments joined by ``/``, so an arbitrarily nested run of
    data-only containers stays flat enough for Blender to store -- nested lists of
    dicts are not reliable -- while still recording exactly where each chunk sat.

Chunks that become something native rather than an object:

- ``REBSHAPE`` -> the mesh datablock of the ``RBOBJECT`` it pairs with by
  document order. Its geometry children become mesh data; the rest ride in
  ``rif_absorbed`` on the mesh.
- ``STDLIGHT`` -> a Blender light. Position, orientation, colour and range are
  real light/object settings.
- ``OBANSEQS`` -> **not an object at all**. An animation is a property of the node
  it animates, so its ``OBANSEQC`` sequences become Actions on the ``OBJCHIER``
  object, held in NLA tracks the way Blender models several takes for one object.
  Giving each sequence its own empty produced 2,916 of them for Elint MkII.
- ``BMPNAMES`` -> **materials, plus a table on the collection**. A polygon names
  its texture by an index into one file-level table (:mod:`bmpnames`), so the
  table is file data and lands on the collection, while each index used by
  geometry gets a Blender material carrying the name it resolved to. The image
  itself is not in the ``.rif`` at all -- it is a ``.RIM`` beside the install
  (:mod:`rim`) -- so it is loaded, packed into the ``.blend`` and wired to a
  Principled BSDF for display only. **The name is what the file stores and the
  name is what export writes back**, which is why editing ``rif_bmp_name`` on a
  material retextures a model and swapping the image does not.
"""

import contextlib
import json
import math
import os
import re
import array
import struct
import tempfile
import zlib

import bpy
import mathutils

from . import bmpnames
from . import cutscene
from . import emitters
from . import heads
from . import rif
from . import rim
from . import schema
from . import shapes as shp
from . import sounds as snd

#: RIF integer units to Blender metres. The engine's own factor is per-rif data
#: read at level load (see gk::RifUnitScale), not a constant this addon can know,
#: so this is a convention: a character shape spans about +-1900 units against a
#: roughly two-metre character.
DEFAULT_SCALE = 0.001

FIXED_ONE = 65536.0  # 16.16

#: **RIF is Y-down**: a biped's parts all sit at negative Y, feet nearest the
#: origin (about -100) and the top of the head furthest (about -1990, i.e. ~1.9 m
#: at the default scale). So the body extends in -Y from the ground, which makes
#: -Y up. `(x, y, z) -> (x, z, -y)` is a -90 degree rotation about X, and
#: orientations need the same change of basis or a placed object faces wrong.
#:
#: The mapping's determinant is +1, so it does not mirror. That is the right
#: choice because RIF is right-handed: `shapes.face_normal` takes an ordinary
#: right-handed cross product of the raw RIF coordinates and agrees with the
#: shipped SHPPNORM on 99.91% of 1.77M faces, which it could not do in a
#: left-handed space.
_BASIS = mathutils.Quaternion((1.0, 0.0, 0.0), math.radians(-90.0))

#: REBSHAPE children that become mesh data rather than properties. Every one of
#: these is either the geometry itself or regenerable from it, so none is carried
#: as a field: `SHPCENTR` is recomputed on export (its integer centre reproduces
#: the shipped value in all 9,244 shapes that have one) and the normals follow
#: from the winding.
GEOMETRY_CHUNKS = frozenset(
    (b"SHPRAWVT", b"SHPPOLYS", b"SHPUVCRD", b"SHPVNORM", b"SHPPNORM", b"SHPCENTR")
)

#: Marks a mesh whose shape shipped *without* a `SHPCENTR`, so export does not
#: invent one. See `_mesh_for_shape`.
NO_CENTRE_PROP = "rif_no_centre"

#: Per-polygon and per-vertex data that is *authored*, not derived, so it becomes a
#: Blender attribute and rides along with the mesh through an edit:
#:
#: - ``SHPMRGDT`` is exactly one int32 per polygon in all 9,357 shipped shapes
#:   (AvP's ``{int *merge_data; int num_polys}``) -> a face attribute, and where
#:   the pair can be one, **a quad**: the pairing is the quad, and export reads
#:   it back off the tessellation rather than out of the attribute
#:   (``shapes.plan_faces`` / ``fuse_quad``). The attribute stays for the pairs
#:   no quad can hold and for a quad somebody splits.
#:
#:   Its wire value is the *index of the polygon this one pairs with*, so it is a
#:   reference into a list whose numbering this addon changes -- import drops
#:   faces Blender cannot hold, export drops ones that weld degenerate, and
#:   dropping a single face renumbers everything after it. Carrying the index
#:   made 15 of the 24 shipped level map objects unwalkable and crashed the game
#:   on load, because ``MergePolygonsInChunkShape`` @ 0x005d7900 has **no bounds
#:   check of any kind**. So the attribute holds a **pair id** instead: see
#:   :data:`MERGE_PAIR_ATTR` and ``shapes.merge_pairs_from_wire``.
#: - ``SHPVTINT`` is per-vertex lighting and a child of ``RBOBJECT``, never of a
#:   shape, in all 4,668 cases -> a **colour** attribute on the object's mesh.
#:   It is the one entry here that is not stored as the int32 the wire holds:
#:   see the "Baked vertex lighting" section for why the paintable form is the
#:   stored one, and :data:`VTINT_HEADER_PROP` for what says it exists at all.
#: The merge pairing, as a shared id per pair. **Renamed** from
#: ``rif_merge_group``, which held the raw wire value and was wrong: a ``.blend``
#: from that build has no attribute under this name, so its shapes export with
#: no ``SHPMRGDT`` at all -- which is legal, costs only the quad merge, and is
#: the right direction to fail in. The old values are not converted, because
#: they are indices into a numbering the importer had already changed and a
#: conversion would produce a plausible, wrong pairing.
MERGE_PAIR_ATTR = "rif_merge_pair"

ATTRIBUTE_CHUNKS = {
    b"SHPMRGDT": (MERGE_PAIR_ATTR, "INT", "FACE"),
    b"SHPVTINT": ("rif_light", "BYTE_COLOR", "POINT"),
}

#: Preprocessed render data. **Discarded on load and omitted on export**, and
#: that is now measured against Gunlok rather than inferred from AvP: the
#: ``SHPPCINF`` string has three referrers in gl.exe -- its registration, its
#: loader, and ``StripUnusedShapeChunks`` @ 0x005b5df0, which *deletes* it once
#: the shape is built -- so nothing ever reads the fields the loader fills.
#: 681 of the 9,357 shipped shapes carry none. Byte-exact round-tripping is not
#: a goal; semantic equivalence is.
DISCARDED_CHUNKS = frozenset((b"SHPPCINF",))


# --------------------------------------------------------------------------
# coordinates
# --------------------------------------------------------------------------

def to_blender(v, scale, y_down):
    """RIF (X right, Y **down**, Z) -> Blender (X right, Y forward, Z up)."""
    if y_down:
        return (v[0] * scale, v[2] * scale, -v[1] * scale)
    return (v[0] * scale, v[1] * scale, v[2] * scale)


def to_rif(v, scale, y_down):
    if y_down:
        return (int(round(v[0] / scale)), int(round(-v[2] / scale)), int(round(v[1] / scale)))
    return (int(round(v[0] / scale)), int(round(v[1] / scale)), int(round(v[2] / scale)))


def quat_to_blender(q, y_down):
    """RIF (x, y, z, w) -> Blender Quaternion (w, x, y, z)."""
    quat = mathutils.Quaternion((q[3], q[0], q[1], q[2]))
    return (_BASIS @ quat @ _BASIS.inverted()) if y_down else quat


def quat_to_rif(quat, y_down):
    if y_down:
        quat = _BASIS.inverted() @ quat @ _BASIS
    return (quat.x, quat.y, quat.z, quat.w)


def matrix_to_blender(m16, y_down):
    """A 16.16 row-major 3x3 (STDLIGHT) -> Blender quaternion."""
    rows = [[m16[r * 3 + c] / FIXED_ONE for c in range(3)] for r in range(3)]
    quat = mathutils.Matrix(rows).to_quaternion()
    return (_BASIS @ quat @ _BASIS.inverted()) if y_down else quat


def matrix_to_rif(quat, y_down):
    if y_down:
        quat = _BASIS.inverted() @ quat @ _BASIS
    m = quat.to_matrix()
    return [int(round(m[r][c] * FIXED_ONE)) for r in range(3) for c in range(3)]


# --------------------------------------------------------------------------
# property plumbing
# --------------------------------------------------------------------------

#: ID property integers are int32. Nothing in the shipped assets needs more (the
#: widest is STDLIGHT's colour at 0xFFFFFF), but a uint32 field could overflow, so
#: values are wrapped into signed range on the way in and back on the way out.
def _to_signed32(v):
    return v - 0x100000000 if isinstance(v, int) and v >= 0x80000000 else v


def _to_unsigned32(v):
    return v + 0x100000000 if isinstance(v, int) and v < 0 else v


def _set_fields(datablock, props):
    """Write decoded chunk fields onto a datablock as typed ID properties."""
    for key, val in props.items():
        val = [_to_signed32(x) for x in val] if isinstance(val, list) else _to_signed32(val)
        datablock["rif_" + key] = val


def _get_fields(datablock, chunk_id):
    """Read the chunk fields back off a datablock."""
    unsigned = {name for name, kind, _ in schema.SCHEMA.get(chunk_id, ()) if kind == schema.U32}
    out = {}
    for key in datablock.keys():  # noqa: SIM118 - IDPropertyGroup, not a dict
        if not key.startswith("rif_") or key in _STRUCTURAL:
            continue
        name = key[4:]
        val = datablock[key]
        if hasattr(val, "__len__") and not isinstance(val, str):
            val = list(val)
            if name in unsigned:
                val = [_to_unsigned32(x) for x in val]
        elif name in unsigned:
            val = _to_unsigned32(val)
        out[name] = val
    return out


#: Properties the addon keeps for its own bookkeeping. These are *not* chunk
#: fields and must never be written back into a chunk body.
_STRUCTURAL = frozenset(("rif_seq_duration_ms", "rif_seq_flags", "rif_seq_speed",
                         "rif_seq_had", "rif_seq_edited",
                         "rif_indsound", "rif_indsound_active", "rif_sound_path",
                         "rif_sound_events", "rif_sound_dir",
                         "rif_emitter_text", "rif_emitter_index",
                         "rif_id", "rif_index", "rif_absorbed", "rif_scale", "rif_y_down",
                         "rif_shape_index", "rif_texture_index", "rif_pair",
                         "rif_name", "rif_bound", "rif_objhead", "rif_dumobjdt",
                         "rif_rest", "rif_rig_parented", "rif_lod_base",
                         "rif_anim_index",
                         "rif_anim_absorbed", "rif_vtint_header", "rif_fps",
                         "rif_bmp_name", "rif_bmpnames", "rif_bmpnames_path",
                         "rif_bmpnames_version", "rif_uv_scale",
                         "rif_no_centre"))


#: Chunks that earn their own Blender datablock. Everything else is data, however
#: deeply the file nests it.
OBJECT_CHUNKS = frozenset(
    (b"RBOBJECT", b"REBSHAPE", b"SUBSHAPE", b"OBJCHIER", b"DUMMYOBJ",
     b"STDLIGHT", b"LIGHTSET")
)

#: Handled by their own machinery, never absorbed.
SPECIAL_CHUNKS = frozenset((b"OBANSEQS", b"OBANSEQC", b"OBASEQFR"))


def _is_data_only(chunk):
    """True when nothing in this subtree needs a Blender object of its own."""
    for kid in chunk.children or ():
        if kid.id in OBJECT_CHUNKS or kid.id in SPECIAL_CHUNKS:
            return False
        if kid.children is not None and not _is_data_only(kid):
            return False
    return True


def _absorb(chunk, skip, prefix=""):
    """Fold a chunk's data-only descendants into ``[(path, props)]``.

    The .blend does not have to mirror the file's chunk tree -- it only has to be
    able to rebuild one. So a container that holds no objects does not become an
    empty; it becomes part of a path string on whatever datablock owns it.
    ``OBINTDT`` alone was 9,313 empties across the game whose entire content is a
    note string.

    A path is ``id:index`` segments joined by ``/``, which keeps the structure
    flat enough for Blender to store (nested lists of dicts are not reliable)
    while still recording exactly where each chunk sat.
    """
    out = []
    for i, kid in enumerate(chunk.children or ()):
        if kid.id in skip or kid.id in SPECIAL_CHUNKS:
            continue
        path = "%s%s:%d" % (prefix, kid.name, i)
        if kid.children is None:
            out.append((path, schema.decode(kid.id, kid.body)))
        elif kid.id not in OBJECT_CHUNKS and _is_data_only(kid):
            nested = _absorb(kid, skip, path + "/")
            # An empty data-only container still has to come back, so record the
            # path with no fields rather than dropping it.
            out.extend(nested if nested else [(path, {})])
    return out


def _pack_absorbed(entries):
    """[(path, props)] -> a list of dicts, which is what Blender can store.

    A ragged list like ``[["a", 1], ["b", 2]]`` is rejected outright (TypeError);
    a list of dicts becomes an IDP_IDPARRAY and survives save/load intact. Field
    order does not have to be carried because :func:`schema.encode` walks its own
    table and looks each field up by name.
    """
    return [{"path": path, "fields": dict(props)} for path, props in entries]


def _unpack_absorbed(raw):
    for entry in raw or ():
        fields = entry["fields"]
        props = {}
        for key in fields.keys():  # noqa: SIM118 - IDPropertyGroup, not a dict
            val = fields[key]
            props[key] = (list(val) if hasattr(val, "__len__") and not isinstance(val, str)
                          else val)
        yield entry["path"], props


def _emit_absorbed(datablock):
    """Rebuild the absorbed subtree: ``[(index, Chunk)]`` for the owner's children."""
    return _emit_from(datablock.get("rif_absorbed"))


def _emit_from(raw, extra=()):
    """Same, from a raw ``rif_absorbed``-shaped list.

    ``extra`` is ``[(path, Chunk)]`` in that same ``id:index`` form, for the
    chunks the scene rebuilds itself instead of storing as fields -- the texture
    table is the only one. Children are ordered by the index in their path, not
    by the order they arrive in, so an injected chunk goes back exactly where it
    sat among its siblings.
    """
    roots = []
    made = {}
    pending = {}

    def place(path, chunk):
        head, _, last = path.rpartition("/")
        index = int(last.rpartition(":")[2] or 0)
        if head:
            container_for(head)
            pending[head].append((index, chunk))
        else:
            roots.append((index, chunk))

    def container_for(path):
        """The Chunk for a path prefix, creating it (and its parents) if needed."""
        if path in made:
            return made[path]
        cid = path.rpartition("/")[2].rpartition(":")[0]
        chunk = rif.Chunk(cid.encode("latin-1"), b"", [])
        made[path] = chunk
        pending[path] = []
        place(path, chunk)
        return chunk

    for path, props in _unpack_absorbed(raw):
        cid = path.rpartition("/")[2].rpartition(":")[0].encode("latin-1")
        if cid in rif.CONTAINERS and not props:
            container_for(path)  # an empty container
            continue
        place(path, rif.Chunk(cid, schema.encode(cid, props)))

    for path, chunk in extra:
        place(path, chunk)

    for path, kids in pending.items():
        made[path].children = [c for _, c in sorted(kids, key=lambda t: t[0])]
    return roots






# --------------------------------------------------------------------------
# textures
# --------------------------------------------------------------------------
#
# A polygon carries a texture *index*; the file-level BMPNAMES table turns that
# into a path like `Units\baddies3.RIM`, relative to the install's `Graphics`.
# So there are three separate things and each lives where it belongs:
#
# - the **table** is file data -> a property on the collection, kept whole, so
#   entries no polygon references survive and export can rebuild the chunk;
# - the **index** is what geometry stores -> `rif_texture_index` on a material,
#   which is what makes an unresolved index (the 0xfff sentinel, or the junk the
#   `_shadow` meshes carry) round-trip without a name to call it by;
# - the **image** is not in the file at all -> a packed Blender image, loaded
#   for display and never written back.


#: Lifted out of the absorbed tree because the scene models it properly, and
#: rebuilt from the scene on the way out.
_TABLE_CHUNKS = frozenset((b"BMPNAMES",))


def _find_chunk_path(chunk, cid, prefix=""):
    """``(path, chunk)`` for the first ``cid`` in the tree, in ``_absorb``'s form."""
    for i, kid in enumerate(chunk.children or ()):
        path = "%s%s:%d" % (prefix, kid.name, i)
        if kid.id == cid:
            return path, kid
        if kid.children is not None:
            got = _find_chunk_path(kid, cid, path + "/")
            if got is not None:
                return got
    return None


def _read_texture_table(root, collection):
    """Lift ``BMPNAMES`` out of the tree and onto the collection.

    Returns ``{index: entry}`` for the material pass. A file without a table --
    36 of the 563 ship that way, none of them with textured geometry -- leaves
    the collection with no table entries at all, and every material falls back
    to its raw index. **An empty table is not the same thing**: 8 shipped files
    carry a ``BMPNAMES`` with no entries, and that chunk has to come back.

    The table is file data, so the *entries* always land on the collection. The
    *path* is recorded separately, by :func:`_note_table_owner`, on whichever
    datablock ends up absorbing the container the chunk sits in -- which is not
    always the collection. All 527 tables are under the file-level ``REBENVDT``,
    but in 34 files that container also holds a ``LIGHTSET``, which makes it an
    object rather than data folded onto the collection.
    """
    found = _find_chunk_path(root, b"BMPNAMES")
    if found is None:
        return {}
    _path, chunk = found
    try:
        version, entries = bmpnames.decode(chunk.body)
    except bmpnames.BmpNamesError:
        # A table this build cannot read stays where it is, as typed fields, and
        # the materials fall back to raw indices -- the file still round-trips.
        return {}
    _CTX["table_chunk"] = chunk
    collection["rif_bmpnames_version"] = version
    collection["rif_bmpnames"] = [_pack_entry(e) for e in entries]
    return {e["index"]: e for e in entries}


def _note_table_owner(chunk, datablock, skip, prefix=""):
    """Claim the lifted ``BMPNAMES`` for the datablock absorbing ``chunk``.

    Adds it to ``skip`` so it is not also stored as typed fields, and records
    the path export has to put it back at -- relative to whatever this
    datablock's ``rif_absorbed`` is relative to, which is why the caller passes
    the prefix rather than this recomputing one from the root.
    """
    table = _CTX.get("table_chunk")
    if table is None:  # no table, or it has already been claimed
        return skip
    found = _find_chunk_path(chunk, b"BMPNAMES", prefix)
    if found is None or found[1] is not table:
        return skip
    datablock["rif_bmpnames_path"] = found[0]
    _CTX["table_chunk"] = None
    return set(skip) | _TABLE_CHUNKS


def _pack_entry(entry):
    """A table entry as ID properties: ``flags`` and the size word are uint32."""
    out = dict(entry)
    out["flags"] = _to_signed32(entry["flags"])
    out["transparency"] = _to_signed32(entry["transparency"])
    return out


def _unpack_entry(raw):
    """The reverse: one stored entry back into the dict :mod:`bmpnames` writes."""
    return {
        "name": raw["name"],
        "flags": _to_unsigned32(raw["flags"]),
        "index": int(raw["index"]),
        "data1": int(raw["data1"]),
        "priority": int(raw["priority"]),
        "transparency": _to_unsigned32(raw["transparency"]),
        "padding": list(raw.get("padding") or ()),
    }


# --------------------------------------------------------------------------
# the INDSOUND table
# --------------------------------------------------------------------------
#
# **This is not the ambient sound system.** Gunlok has two, they share nothing,
# and modelling both as Speakers would put two object types in the outliner that
# look identical and mean opposite things. `INDSOUND` is a *table of definitions*
# an animation keyframe selects from by number, and it plays wherever the
# animating model is -- so it has no position, and the transform of a datablock
# standing for one would be meaningless. `DUMOBJTX` (below, and :mod:`emitters`)
# is the *placement* system, and that is what the Speakers are now.
#
# So this follows `BMPNAMES` exactly, because it is structurally the same thing:
# a file-level indexed table whose index is a stable, sparse id meaningful only
# inside its own file, with a payload loaded from the install for preview and
# never written back.
#
# - the **table** -> `rif_indsound` on the collection, kept whole and in order,
#   so an entry no keyframe references survives;
# - the **index** -> `rif_sound_events` on the Action, because a sound is a
#   property of a sequence at a time;
# - the **audio** -> a `bpy.types.Sound` per entry, loaded from the install for
#   audition and never written back;
# - the **editing surface** -> a UIList panel, the way a material's texture name
#   is edited.
#
# The cost, accepted deliberately: Blender's Speaker widgets (a distance slider,
# a sound file browser) are gone and the panel has to approximate them.

#: The chunk lifted out of the absorbed tree because the scene models it properly.
_SOUND_CHUNKS = frozenset((b"INDSOUND",))

#: The table itself, on the collection. Named for its chunk, the way
#: `rif_bmpnames` is, and *not* `rif_sounds` -- which is what it was called when
#: it also stood for the ambient emitters. A .blend from that build is still read
#: (see :func:`sound_table`), so an upgrade does not silently drop the table.
SOUND_TABLE_PROP = "rif_indsound"

#: The active row of the UIList, which is UI state and nothing else.
SOUND_ACTIVE_PROP = "rif_indsound_active"

#: Stamped on a loaded `bpy.types.Sound` so an entry can find its audio again.
#: The same job `rif_rim_path` does for a texture's image.
SOUND_PATH_PROP = "rif_sound_path"


def _read_sound_table(root, collection):
    """Lift every ``INDSOUND`` onto the collection. Returns ``{index: entry}``.

    Unlike ``BMPNAMES`` there is no container to locate: all 240 shipped chunks
    are direct children of the file root, so they absorb onto the collection like
    any other root-level leaf and their own child index is the path.
    """
    entries = []
    for i, kid in enumerate(root.children or ()):
        if kid.id != b"INDSOUND":
            continue
        try:
            entry = snd.decode(kid.body)
        except snd.SoundError:
            # A table this build cannot read stays where it is, as typed fields,
            # and the file still round-trips.
            return {}
        entry["chunk_index"] = i
        entries.append(entry)
    collection[SOUND_TABLE_PROP] = [_pack_sound(e) for e in entries]
    collection[SOUND_ACTIVE_PROP] = 0
    return {int(e["index"]): e for e in entries}


def _pack_sound(entry):
    out = dict(entry)
    out["padding"] = list(entry.get("padding") or ())
    out["extra"] = list(entry.get("extra") or ())
    return out


def _unpack_sound(raw):
    out = {"path": raw["path"], "index": int(raw["index"]),
           "padding": list(raw.get("padding") or ()),
           "extra": list(raw.get("extra") or ()),
           "chunk_index": int(raw.get("chunk_index", -1))}
    for name in snd.TRAILING:
        out[name] = int(raw.get(name, 0))
    return out


def _sound_root(source_path, override):
    """The install's ``Sound`` directory, found the way textures find ``Graphics``."""
    if override:
        return override if os.path.isdir(override) else None
    if not source_path:
        return None
    cur = os.path.dirname(os.path.abspath(source_path))
    while True:
        for name in os.listdir(cur) if os.path.isdir(cur) else ():
            if name.lower() == "sound" and os.path.isdir(os.path.join(cur, name)):
                return os.path.join(cur, name)
        parent = os.path.dirname(cur)
        if parent == cur:
            return None
        cur = parent


def _resolve_sound(entry, root_dir):
    """The ``.wav`` on disk for an entry, or None.

    The stored path is relative to the install's ``Sound`` folder and uses
    backslashes; a path with no folder part is looked up directly under it. The
    match is case-insensitive because the shipped entries are not consistent --
    ``Robots`` and ``robots`` both appear for the same directory.
    """
    if not root_dir:
        return None
    parts = [p for p in entry["path"].replace("\\", "/").split("/") if p]
    cur = root_dir
    for i, part in enumerate(parts):
        if not os.path.isdir(cur):
            return None
        match = next((n for n in os.listdir(cur) if n.lower() == part.lower()), None)
        if match is None:
            return None
        cur = os.path.join(cur, match)
        if i == len(parts) - 1:
            return cur if os.path.isfile(cur) else None
    return None


def _load_sound_audio(table, root_dir):
    """One ``bpy.types.Sound`` per entry whose ``.wav`` is on disk.

    For audition only -- export writes the *path*, never the wave, exactly as it
    writes a texture's name and never the image. Each one is stamped with the
    path it stands for and given a fake user, because a Sound nothing points at
    is dropped on the next ``.blend`` save and the entry would lose its audio
    across a reopen.
    """
    loaded = 0
    for index in sorted(table):
        entry = table[index]
        path = _resolve_sound(entry, root_dir)
        if path is None:
            continue
        try:
            sound = bpy.data.sounds.load(path, check_existing=True)
        except Exception:  # noqa: BLE001 - a missing sound never fails an import
            continue
        sound[SOUND_PATH_PROP] = entry["path"]
        sound.use_fake_user = True
        loaded += 1
    return loaded


def sound_audio(entry):
    """The loaded ``bpy.types.Sound`` for a table entry, or None.

    Matched on the stored path rather than held as a pointer: the table is a
    plain ID property, and a datablock reference inside one of those is not
    something an array of entries can carry.
    """
    want = (entry.get("path") or "").lower()
    if not want:
        return None
    for sound in bpy.data.sounds:
        if (sound.get(SOUND_PATH_PROP, "") or "").lower() == want:
            return sound
    return None


def sound_table(collection):
    """The table this export will write, whole and in order.

    Kept exactly as ``rif_bmpnames`` is: entries no keyframe references survive,
    because an index is a stable id and dropping unused rows would renumber
    nothing but would lose data the file carried.

    ``rif_sounds`` is read as a fallback for a ``.blend`` saved by the build
    where this table lived on Speaker objects. It is read, never written -- there
    is one storage location, and the old one is only a way in.
    """
    raw = collection.get(SOUND_TABLE_PROP) if collection is not None else None
    if raw is None and collection is not None:
        raw = collection.get("rif_sounds")
    entries = [_unpack_sound(r) for r in (raw or ())]
    entries.sort(key=lambda e: (e.get("chunk_index", -1), e["index"]))
    return entries


def set_sound_table(collection, entries):
    """Write the table back, keeping the active row inside it."""
    collection[SOUND_TABLE_PROP] = [_pack_sound(e) for e in entries]
    active = int(collection.get(SOUND_ACTIVE_PROP, 0))
    collection[SOUND_ACTIVE_PROP] = max(0, min(active, len(entries) - 1))


def active_sound(collection):
    """``(index, entry)`` for the row the panel is editing, or ``(-1, None)``."""
    entries = sound_table(collection) if collection is not None else []
    if not entries:
        return -1, None
    at = max(0, min(int(collection.get(SOUND_ACTIVE_PROP, 0)), len(entries) - 1))
    return at, entries[at]


def set_sound_field(collection, name, value):
    """Edit one field of the active entry. Returns False when there is none."""
    entries = sound_table(collection)
    at, entry = active_sound(collection)
    if entry is None:
        return False
    entries[at][name] = value
    set_sound_table(collection, entries)
    return True


def remove_sound(collection, at=None):
    """Drop one entry. This is how a sound is removed now that there is no object."""
    entries = sound_table(collection)
    at = active_sound(collection)[0] if at is None else at
    if not 0 <= at < len(entries):
        return False
    del entries[at]
    set_sound_table(collection, entries)
    return True


def next_sound_index(collection):
    used = {e["index"] for e in sound_table(collection)}
    for i in range(1, snd.TABLE_SLOTS):
        if i not in used:
            return i
    return 0


# --------------------------------------------------------------------------
# ambient sound emitters: the DUMOBJTX half, and the Speakers
# --------------------------------------------------------------------------
#
# **These are the Speakers**, and the reason is that they are the only sound in
# the game that has a position. A `DUMOBJTX` is a placement, not a definition: a
# text directive on a top-level `DUMMYOBJ` that `ToMap` turns into one looping
# emitter at that dummy's fixed world coordinates, started once from `LoadLevel`.
# So `distance_reference`, `distance_max`, `pitch` and the object's transform all
# mean something here, which is exactly what they could never mean for an
# `INDSOUND` entry -- and having both wear the same datablock type would put two
# indistinguishable things in the outliner with opposite semantics.
#
# The dummy *is* the Speaker rather than carrying one. A dummy is a name at a
# position and nothing else, so there is no second transform for a child object
# to add, and one object cannot drift out of step with itself.
#
# **The text is the storage; the Speaker is a view of it.** The shipped texts are
# too irregular to reproduce by reformatting (see :mod:`emitters`), so the raw
# string is carried and `emitters.retext` splices back only the directives whose
# value actually changed -- which is what keeps an untouched level byte-exact
# through import, a .blend round trip and export.

#: The `DUMOBJTX` text, verbatim, on the dummy's object. The truth; the Speaker's
#: own settings are derived from it and compared back against it on export.
EMITTER_TEXT_PROP = "rif_emitter_text"

#: Where the `DUMOBJTX` sat among its dummy's children, so it goes back there.
EMITTER_INDEX_PROP = "rif_emitter_index"

#: Where the ambient `.wav` files live, and what indexes them. Not `SOUNDDIR`,
#: which is inert -- the sound system resolves these against its own directory
#: list. All 1,097 shipped names are bare files with no folder part.
EMITTER_DIR = "environ"


def _dumobjtx_text(chunk):
    """The text out of a ``DUMOBJTX`` chunk, or ``None`` if it is unreadable."""
    if chunk is None:
        return None
    try:
        return schema.decode(b"DUMOBJTX", chunk.body)["text"]
    except (schema.SchemaError, KeyError, UnicodeDecodeError):
        return None


def resolve_emitter_wav(name, root_dir):
    """The ambient ``.wav`` on disk, or None.

    Looks in ``Sound\\environ`` first because that is where all 44 of the
    shipped emitter sounds live, then falls back to a case-insensitive scan of
    the other ``Sound`` subdirectories -- the engine resolves against a directory
    *list*, so a name found elsewhere is not wrong.
    """
    if not root_dir or not name:
        return None
    name = name.replace("\\", "/").split("/")[-1]

    def find_in(folder):
        if not os.path.isdir(folder):
            return None
        match = next((n for n in os.listdir(folder) if n.lower() == name.lower()), None)
        return os.path.join(folder, match) if match else None

    environ = next((os.path.join(root_dir, n) for n in os.listdir(root_dir)
                    if n.lower() == EMITTER_DIR and os.path.isdir(os.path.join(root_dir, n))),
                   None)
    got = find_in(environ) if environ else None
    if got:
        return got
    for entry in sorted(os.listdir(root_dir)):
        sub = os.path.join(root_dir, entry)
        if os.path.isdir(sub):
            got = find_in(sub)
            if got:
                return got
    return find_in(root_dir)


def _apply_emitter(obj, text, root_dir):
    """Text -> real Speaker settings, so the falloff and pitch are visible.

    Absent directives take the engine's own defaults (``V`` 100, everything else
    0), and a zero distance means *the sample's own*, not silence -- which is why
    735 of the 1,097 shipped emitters import with a collapsed gizmo. That is the
    file speaking, not a loss: it specifies no radius.
    """
    spk = obj.data
    obj[EMITTER_TEXT_PROP] = text
    vals = emitters.effective(text)
    spk.distance_reference = vals["I"]
    spk.distance_max = vals["R"]
    spk.pitch = emitters.pitch_to_factor(vals["P"])
    spk.volume = emitters.volume_to_fraction(vals["V"])
    path = resolve_emitter_wav(emitters.wav(text), root_dir)
    if path is not None:
        with contextlib.suppress(Exception):  # a missing wav never fails an import
            spk.sound = bpy.data.sounds.load(path, check_existing=True)
    return spk


def emitter_objects(collection):
    """Every dummy in the collection that is an ambient emitter."""
    return [o for o in (collection.objects if collection else ())
            if o.get("rif_id") == "DUMMYOBJ" and EMITTER_TEXT_PROP in o]


def emitter_text(obj):
    return obj.get(EMITTER_TEXT_PROP, "") or ""


def emitter_values(obj):
    """What this emitter's text says, absent directives filled in by the engine."""
    return emitters.effective(emitter_text(obj))


def set_emitter_wav(obj, wav_name):
    """Point the emitter at a different ``.wav``, keeping every other byte."""
    obj[EMITTER_TEXT_PROP] = emitters.retext(emitter_text(obj), wav_name=wav_name)
    return True


def emitter_text_from_speaker(obj):
    """The text this emitter would export: its own, with the Speaker spliced in.

    Only what differs is rewritten, and "differs" is decided on the *formatted*
    argument rather than the float -- so a pitch that made the round trip through
    a float32 Blender property as 1.9999998 still spells ``P2`` and the chunk
    comes back byte for byte.
    """
    text = emitter_text(obj)
    spk = obj.data
    if spk is None or obj.type != "SPEAKER":
        return text
    return emitters.retext(text, values_by_letter={
        "I": spk.distance_reference,
        "R": spk.distance_max,
        "P": emitters.factor_to_pitch(spk.pitch),
        "V": emitters.fraction_to_volume(spk.volume),
    })


def _emitter_chunk(obj):
    """The ``DUMOBJTX`` for one emitter, or None if it is not one."""
    text = emitter_text_from_speaker(obj)
    if not text:
        return None
    return rif.Chunk(b"DUMOBJTX", schema.dumobjtx_body(text))


def _sound_chunks(collection):
    """``[(path, Chunk)]`` for the table, in ``_emit_from``'s injection form."""
    out = []
    used = set()
    for e in sound_table(collection):
        i = e.get("chunk_index", -1)
        if i < 0 or i in used:
            i = _next_free_root_index(collection, used)
        used.add(i)
        out.append(("INDSOUND:%d" % i, rif.Chunk(b"INDSOUND", snd.encode(e))))
    return out


def _next_free_root_index(collection, used):
    taken = set(used) | {_path_index(p) for p, _ in _absorbed_entries(collection)
                         if "/" not in p}
    for obj in collection.objects:
        if "rif_id" in obj:
            taken.add(int(obj.get("rif_index", 0)))
            if obj.data is not None and obj.data.get("rif_id") in ("REBSHAPE", "SUBSHAPE"):
                taken.add(int(obj.data.get("rif_index", 0)))
    i = 0
    while i in taken:
        i += 1
    return i


def _texture_root(source_path, override):
    if override:
        return override if os.path.isdir(override) else None
    return rim.find_texture_root(source_path) if source_path else None


def _material_name(entry, index):
    if entry is None:
        return "rif_tex_%d" % index
    stem = os.path.splitext(os.path.basename(entry["name"].replace("\\", "/")))[0]
    return stem or ("rif_tex_%d" % index)


def _material_for(index):
    """The material for one texture index, made once per imported file.

    Materials are per-import rather than shared by name, because the index that
    identifies a texture is only meaningful inside its own file: the same
    ``.RIM`` is entry 11 in one level and entry 4 in the next. Images *are*
    shared, which is where the cost is.
    """
    cache = _CTX["materials"]
    mat = cache.get(index)
    if mat is not None:
        return mat

    entry = _CTX["textures"].get(index)
    mat = bpy.data.materials.new(_material_name(entry, index))
    mat["rif_texture_index"] = index
    if entry is not None:
        mat["rif_bmp_name"] = entry["name"]
        image = _image_for(entry["name"])
        if image is not None:
            _wire_texture(mat, image)
    mat["rif_uv_scale"] = list(uv_scale(mat, entry))
    cache[index] = mat
    return mat


#: **A stored UV is a texel coordinate, not a fraction of the image.** Values run
#: 0..width and 0..height -- measured across 375k shipped UV pairs, the 99th
#: percentile of ``|u|`` lands within 7% of the texture's own width for every
#: size in the game (1024, 512, 256, 128) -- and AvP's loader agrees, casting the
#: float straight to the int its renderer wants in texel space
#: (``chnkload.cpp:2390``, where the sprite path builds the same number as
#: ``UVCoords << 16``). Blender wants a fraction, so a UV is divided by the size
#: of the texture the polygon wears, and multiplied back on the way out.
#:
#: Getting this wrong is not subtle: a 1024x1024 texture tiles 1024 times across
#: every face.
#:
#: Both directions are **exact**, which is why nothing rounds here: every texture
#: in the game is a power of two from 8 to 1024, so dividing and multiplying back
#: are bit-identical in float32. (99.5% of shipped UVs are whole texels, but not
#: all of them, so rounding would corrupt the rest.)
def uv_scale(mat, entry=None):
    """Texels per unit UV for a material: what its normalized UVs multiply by.

    The image's own size wins when it is loaded, because the engine samples the
    real texture and the table's declared size is stale in 17 of the 1,597
    shipped entries that resolve to a file. Failing that the table is used, and
    failing *that* whatever the import recorded -- which is what keeps a
    ``.blend`` exporting the same UVs on a machine with no textures installed.
    """
    width, height = _image_size(mat)
    if not (width and height) and entry is not None:
        declared = bmpnames.size(entry)
        if declared:
            width, height = declared
    if not (width and height):
        stored = mat.get("rif_uv_scale")
        if stored is not None and len(stored) == 2:
            width, height = int(stored[0]), int(stored[1])
    return float(width or 1), float(height or 1)


def uv_to_blender(uv, scale):
    """One (u, v) texel pair -> Blender's fraction, V flipped.

    Row 0 of the image is its **top** both in the file and in the PNG handed to
    Blender, and a texel V of 0 means that row -- but Blender's V origin is the
    **bottom** of the image, so the axis is flipped rather than only scaled.
    """
    return (uv[0] / scale[0], 1.0 - uv[1] / scale[1])


def uv_to_rif(uv, scale):
    return (uv[0] * scale[0], (1.0 - uv[1]) * scale[1])


def _image_for(name):
    """Load a ``.RIM`` into a packed Blender image, once per install path."""
    index = _CTX.get("texture_index")
    if index is None:
        return None
    path = index.resolve(name)
    if path is None:
        _CTX["missing_textures"].add(name)
        return None

    key = os.path.normcase(os.path.abspath(path))
    cached = _CTX["images"].get(key)
    if cached is not None:
        return cached
    for image in bpy.data.images:
        if image.get("rif_rim_path") == key:
            _CTX["images"][key] = image
            return image

    try:
        texture = rim.load(path)
    except rim.RimError:
        texture = None
    if texture is None:
        _CTX["undecodable_textures"].add(name)
        return None

    image = _load_png(rim.to_png(texture), os.path.splitext(os.path.basename(path))[0])
    if image is None:
        return None
    image["rif_rim_path"] = key
    image["rif_bmp_name"] = name
    # Whether a texture is cut out is a property of its pixels, not of its
    # format: DXT1 only carries 1-bit alpha in blocks whose endpoints are
    # ordered `c0 <= c1`, and every image here is written out as RGBA either
    # way, so the decoder's answer is recorded rather than re-derived.
    image["rif_has_alpha"] = texture.has_alpha
    # What "unchanged since it was imported" means at export time. A digest
    # rather than a flag, because `is_dirty` is about unsaved edits and resets
    # on a `.blend` save -- paint, save, reopen, and the edit would look like no
    # edit at all.
    image["rif_rim_crc"] = _digest(texture.rgba)
    _CTX["images"][key] = image
    return image


def _load_png(data, name):
    """PNG bytes -> a packed image datablock.

    Blender only keeps pixels across a ``.blend`` save for images that are
    packed or file-backed; an image filled through ``pixels`` is *generated* and
    comes back blank. So the decoded texture is written out as a PNG, loaded the
    ordinary way (which also gets the sRGB handling right), packed, and the
    temporary file removed -- after which the ``.blend`` carries the pixels and
    the original path is only a label.
    """
    handle, temp = tempfile.mkstemp(prefix="rif_", suffix=".png")
    try:
        with os.fdopen(handle, "wb") as fh:
            fh.write(data)
        image = bpy.data.images.load(temp)
        image.name = name
        image.pixels[0]  # force the load while the file is still there
        image.pack()
    except Exception:  # noqa: BLE001 - a texture is never worth failing an import
        return None
    finally:
        with contextlib.suppress(OSError):
            os.remove(temp)
    image.filepath_raw = ""
    return image


def _wire_texture(mat, image):
    """Principled BSDF with the texture in base colour, alpha included."""
    mat.use_nodes = True
    tree = mat.node_tree
    bsdf = next((n for n in tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
    if bsdf is None:
        return
    node = tree.nodes.new("ShaderNodeTexImage")
    node.image = image
    node.location = (bsdf.location.x - 320, bsdf.location.y)
    # Nearest, because these are 8..1024 pixel textures meant for a 2000-era
    # rasterizer and Blender's default filtering makes them mush at that size.
    node.interpolation = "Closest"
    tree.links.new(bsdf.inputs["Base Color"], node.outputs["Color"])
    if _has_alpha(image):
        tree.links.new(bsdf.inputs["Alpha"], node.outputs["Alpha"])
        # Blender 4.2 replaced `blend_method` with a render-method enum; set
        # whichever this build has rather than picking one and requiring it.
        if hasattr(mat, "surface_render_method"):
            mat.surface_render_method = "DITHERED"
        elif hasattr(mat, "blend_method"):
            mat.blend_method = "CLIP"


def _has_alpha(image):
    return bool(image.get("rif_has_alpha", False))


# --------------------------------------------------------------------------
# writing the textures back out
# --------------------------------------------------------------------------
#
# The `.rif` stores a name; the image lives beside the install as a `.RIM`, and
# until now this addon only ever read one. Writing them back is what makes a
# retexture more than a rename -- and it is a *separate* output from the `.rif`,
# because the two go to different places: the model into the file the user
# picked, the textures into a mod tree mirroring the game's `Graphics` folder.
#
# Three things decide the shape:
#
# - **`image.pixels` is the way back out, and it is exact.** For an 8-bit image
#   Blender hands back the stored byte over 255 with no colour management in the
#   way -- measured on Blender 4.2 and 5.2, across a `.blend` save and reload:
#   every byte of every channel survives, alpha included. `Image.save()` to a
#   PNG or a raw TGA reproduces the same bytes, but both go through a temporary
#   file and `Image.file_format`, which is shared state on the datablock, and
#   one probe caught Blender writing the packed PNG verbatim when asked for a
#   TGA. `pixels` has no format to disagree about.
# - **Row 0 is the bottom in Blender and the top in a `.RIM`**, the same flip the
#   UVs get.
# - **Unchanged textures are not written**, because the alternative is a mod that
#   ships megabytes of `BODY` re-encodings of textures the player already has,
#   pixel for pixel. `rif_rim_crc` is stamped at import and never updated, so the
#   test is "do these pixels still match the file they came from" rather than
#   anything about Blender's dirty flag, which a `.blend` save clears.

#: Blender's own sRGB encode, for the rare image whose buffer is float. An 8-bit
#: buffer needs none of this -- ``pixels`` is already the stored byte over 255 --
#: but a float one holds scene-linear values, and this is the transfer function
#: Blender itself would apply on the way to an 8-bit file.
def _linear_to_srgb(v):
    if v <= 0.0031308:
        return v * 12.92
    return 1.055 * (v ** (1.0 / 2.4)) - 0.055


def _digest(rgba):
    """A texture's pixels as a short string.

    Hex rather than the number ``zlib.crc32`` returns, because an ID property is
    a **signed** C int and a checksum uses the whole 32 bits -- storing one
    raises ``OverflowError`` on the 50% of images whose digest happens to have
    the top bit set, which is a crash in the middle of an import.
    """
    return "%08x" % zlib.crc32(rgba)


def image_rgba(image):
    """An image datablock -> ``(width, height, RGBA bytes)``, row 0 at the top."""
    width, height = image.size
    if not width or not height:
        raise ValueError("%r is %dx%d" % (image.name, width, height))
    if image.channels != 4:
        raise ValueError("%r has %d channels, not RGBA" % (image.name, image.channels))
    buf = array.array("f", bytes(4 * width * height * 4))
    image.pixels.foreach_get(buf)
    if image.is_float:
        buf = array.array("f", map(_linear_to_srgb, buf))
    flat = bytes(0 if v <= 0.0 else 255 if v >= 1.0 else int(v * 255.0 + 0.5)
                 for v in buf)
    stride = width * 4
    return width, height, b"".join(
        flat[y * stride:(y + 1) * stride] for y in range(height - 1, -1, -1))


def texture_targets(collection):
    """``[(material, image or None, name)]`` for the textures this file names."""
    out = []
    for mat in _materials_in(collection):
        name = (mat.get("rif_bmp_name", "") or "").strip()
        if not name:
            continue
        image = next((node.image for node in
                      (mat.node_tree.nodes if mat.use_nodes and mat.node_tree else ())
                      if node.type == "TEX_IMAGE" and node.image is not None), None)
        out.append((mat, image, name))
    return out


def _texture_destination(root, name):
    """Where a ``BMPNAMES`` name lands under ``root``, or ``None`` if it escapes it."""
    relative = name.replace("\\", "/").lstrip("/")
    full = os.path.normpath(os.path.join(root, relative))
    if os.path.commonpath([os.path.abspath(root), os.path.abspath(full)]) \
            != os.path.abspath(root):
        return None
    return full


def write_textures(collection, root, changed_only=True, compress=True):
    """Write each named texture as a ``.RIM`` under ``root``. -> stats.

    ``changed_only`` skips an image whose pixels still match the ``.RIM`` it was
    imported from. Anything the addon did not import -- a texture painted from
    scratch, or one imported before this was recorded -- has no digest and is
    always written, which is the safe way round.
    """
    stats = {"written": 0, "unchanged": 0, "no_image": [], "failed": [], "bytes": 0}
    for _mat, image, name in texture_targets(collection):
        if image is None:
            stats["no_image"].append(name)
            continue
        destination = _texture_destination(root, name)
        if destination is None:
            stats["failed"].append((name, "the name points outside the textures folder"))
            continue
        try:
            width, height, rgba = image_rgba(image)
        except Exception as exc:  # noqa: BLE001
            stats["failed"].append((name, str(exc)))
            continue
        if changed_only and image.get("rif_rim_crc") == _digest(rgba):
            stats["unchanged"] += 1
            continue
        try:
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            blob = rim.encode(width, height, rgba, compress)
            with open(destination, "wb") as fh:
                fh.write(blob)
        except (OSError, rim.RimError) as exc:
            stats["failed"].append((name, str(exc)))
            continue
        stats["written"] += 1
        stats["bytes"] += len(blob)
    return stats


# --------------------------------------------------------------------------
# chunk tree -> scene
# --------------------------------------------------------------------------

#: An object names its shape by id, not by position. Both records are laid out
#: in :mod:`heads`; these are the two fields the pairing needs.
OBJHEAD1_SHAPE_ID = heads.OBJHEAD1_SHAPE_ID
SHPHEAD1_FILE_ID = heads.SHPHEAD1_FILE_ID


def _shape_pairs(root):
    """Map each RBOBJECT to its REBSHAPE, by matching ids.

    **Not by document order.** AvP's ``Object_Chunk::assoc_with_shape_no`` walks
    the file's shapes comparing ``hdptr->shape_id_no == shphd->file_id_num``, and
    the same two fields carry it here: ``OBJHEAD1+0x38`` against
    ``SHPHEAD1+0x14``. That resolves all 9,313 objects in the 563 shipped files
    with no shape claimed twice, and it **differs from document order in 86 of
    them** -- 15.3% -- so pairing positionally silently misplaces geometry.

    Confirmed independently: ``SHPVTINT`` is a 16-byte header followed by one
    int32 per vertex, and its length matches the *id-paired* shape's vertex count
    in 4,666 of 4,668 cases.

    Returns {rbobject_child_index: rebshape_child_index}.
    """
    kids = list(root.children or ())
    shape_by_id = {}
    for i, c in enumerate(kids):
        if c.id != b"REBSHAPE":
            continue
        head = c.find(b"SHPHEAD1")
        if head is not None and len(head.body) >= SHPHEAD1_FILE_ID + 4:
            file_id = struct.unpack_from("<i", head.body, SHPHEAD1_FILE_ID)[0]
            shape_by_id.setdefault(file_id, i)

    pairs = {}
    for i, c in enumerate(kids):
        if c.id != b"RBOBJECT":
            continue
        head = c.find(b"OBJHEAD1")
        if head is None or len(head.body) < OBJHEAD1_SHAPE_ID + 4:
            continue
        shape_id = struct.unpack_from("<i", head.body, OBJHEAD1_SHAPE_ID)[0]
        j = shape_by_id.get(shape_id)
        if j is not None:
            pairs[i] = j
    return pairs




#: What a fresh ``LIGHTSET`` holds, and it is measured rather than chosen. All
#: 563 shipped files were scanned: **62 have a ``LIGHTSET``, always exactly one,
#: always a direct child of the file-level ``REBENVDT``**, and every one of the
#: 3,794 ``STDLIGHT`` chunks in the game is a child of one -- there is no such
#: thing as a light anywhere else. The set holds ``LTSETHDR`` first, then the
#: lights, then ``AMBIENCE``; both of those leaves are byte-identical in all 62,
#: which is why they can be constants instead of parameters. 24 of the 62 carry
#: no lights at all, so an empty set is normal shipped data rather than a state
#: to avoid.
LIGHTSET_HEADER = b"NORMALLT\0\0\0\0"   # AvP's char[8] light_set_name + int pad
LIGHTSET_AMBIENCE = 2048

#: A new light's ``spread``: the mode of the shipped values (2,955 of 3,794).
LIGHT_SPREAD = 1000

#: A new ``SHPVTINT``'s header, as the four int32 it is stored as: the light set
#: name (which selects the chunk -- see :func:`_vtint_chunk`), the word that is 0
#: in all 4,668 shipped chunks, and a placeholder count that export regenerates.
VTINT_HEADER = list(struct.unpack("<3i", LIGHTSET_HEADER[:8] + b"\0\0\0\0")) + [0]

#: A new light's brightness, as Blender energy. ``_light_chunk`` writes
#: ``energy * 65536`` because the chunk's field is 16.16, and every shipped light
#: lands in 0.2 .. 2.0 -- so Blender's own 1000 W default would export a
#: brightness three orders of magnitude past anything in the game. Adoption
#: converts it once, which is the only moment that can be told from an edit.
LIGHT_ENERGY = 1.0


def _apply_light(obj, light_chunk, index, scale, y_down):
    props = schema.decode(b"STDLIGHT", light_chunk.body)
    light = obj.data
    colour = props["colour"]
    light.color = (((colour >> 16) & 0xFF) / 255.0,
                   ((colour >> 8) & 0xFF) / 255.0,
                   (colour & 0xFF) / 255.0)
    light.energy = props["brightness"] / FIXED_ONE
    # `range` is in rif units; Blender's custom distance is metres.
    light.use_custom_distance = True
    light.cutoff_distance = props["range"] * scale
    obj.location = to_blender(props["position"], scale, y_down)
    obj.rotation_mode = "QUATERNION"
    obj.rotation_quaternion = matrix_to_blender(props["orientation"], y_down)
    # Everything the light datablock cannot express stays as typed fields.
    for key in ("light_id", "spread", "flags", "local_flags", "pad"):
        obj["rif_" + key] = _to_signed32(props[key]) if not isinstance(props[key], list) \
            else [_to_signed32(x) for x in props[key]]
    obj["rif_id"] = "STDLIGHT"
    obj["rif_index"] = index


# --------------------------------------------------------------------------
# OBJHEAD1: the words that become a Blender transform
# --------------------------------------------------------------------------
#
# The body has no named schema (the tail is 64..96 bytes and undecoded), so it
# rides as a typed int array and only the words that mean something here are
# read back out. Indices are in dwords.
OH_NAME = slice(1, 5)      # 0x04, char[16]
OH_LOCATION = slice(5, 8)  # 0x14, int32[3]
OH_ORIENT = slice(8, 12)   # 0x20, float[4] living in an int array
OH_SHAPE_ID = 14           # 0x38, shape_id_no


def _bits_to_float(i):
    return struct.unpack("<f", struct.pack("<i", i))[0]


def _float_to_bits(f):
    return struct.unpack("<i", struct.pack("<f", f))[0]


#: An object's name is a **trailing** NUL-terminated string after a 60-byte fixed
#: header, padded to a 4-byte boundary -- ``len(body) == 0x3c + padded name`` holds
#: for all 9,313 shipped objects, which is what the 64..96 byte size range is.
#:
#: The 16 bytes at 0x04 are *not* the name: they read ``Player`` in 6,383 objects
#: and binary junk in 2,783, across 59 distinct values. That field is AvP's
#: ``lock_user`` (:mod:`heads`), the editor's lock owner. Reading it as the name
#: hid every real name -- ``Head``, ``Waist``, ``Ribs``, ``Chest``,
#: ``Foot Right``, ``Index Right A``.
OBJHEAD1_NAME = heads.OBJHEAD1_NAME
#: ``DUMOBJDT`` is the same shape with a 52-byte header, for all 6,847 of them.
#: AvP's loader gives the fields exactly: ``location``, ``min_extents`` and
#: ``max_extents`` as int32 triples, then a quaternion, then the name. The
#: quaternion is unit in all 6,847, and ``min_extents.y > max_extents.y`` in 6,843
#: of them -- which is what Y-down predicts, since the "minimum" corner is the
#: bottom and the bottom has the larger Y.
DUMOBJDT_NAME = 0x34
DUMOBJ_LOCATION = 0x00
DUMOBJ_MIN = 0x0C
DUMOBJ_MAX = 0x18
DUMOBJ_ORIENT = 0x24


_trailing_name = heads.trailing_name
_objhead_name = heads.objhead_name


#: Leaf chunks that get their own object instead of folding into the parent.
LEAF_AS_OBJECT = frozenset((b"STDLIGHT",))


#: ``OBASEQHD`` is laid out in :mod:`heads`. AvP's first field is ``num_frames``
#: and **Gunlok does not use it as one** -- it is 65536 in all 29,550 shipped
#: sequences, i.e. the full 16.16 span a frame's ``time`` is a position within.
#: ``num_extra_data`` is 0 in all of them, so the name always starts at 0x10.
#:
#: An earlier revision put it at 0x12, reading the leading ``Dz`` as a constant
#: field because it is present in every sequence. It is not a field: the size
#: arithmetic settles it, since ``len(body) == 0x10 + ((len(name) + 4) & ~3)``
#: holds for all 29,550 only when the name starts at 0x10. ``Dz`` is a naming
#: convention in the source assets, so the sequences really are called
#: ``DzSeq_Stand``, ``DzSeq_Walk``, and so on.
OBASEQHD_NAME = heads.SEQHEAD_NAME


def _sequence_name(seq_chunk, fallback):
    """The sequence name out of OBASEQHD: "Seq_Stand", "Seq_Die", "Seq_Walk"."""
    hd = seq_chunk.find(b"OBASEQHD")
    if hd is not None and len(hd.body) > OBASEQHD_NAME:
        text = hd.body[OBASEQHD_NAME:].split(b"\0")[0]
        if text and all(32 <= b < 127 for b in text):
            return text.decode("ascii")
    return fallback


#: A bone is 10 cm long. Nothing reads the length -- these are rigid parts, not a
#: deforming skeleton -- but a zero-length bone is deleted by Blender on exit from
#: edit mode.
BONE_LENGTH = 0.1

#: A level-of-detail variant: ``L<n>#<base part name>``, n in 1..9.
#:
#: These are not extra geometry. The engine swaps the mesh on the hierarchy node
#: that already drives the base part -- AvP's ``Projload.cpp`` files them by
#: ``o_name[1] - '0'`` after ``strcmp(&o_name[3], base_name)`` and keeps only the
#: *shapes* (``deg_ptr->shape = low_detail_array[i]``), the object itself being
#: discarded; Gunlok's own ``BuildObjectLodChain`` @ 0x005b2910 tests the same
#: three characters. So a variant belongs on its base part's bone -- *not* at its
#: own stored placement, which is parked beside the model; see the measurement in
#: :func:`_build_armature`.
#:
#: An ``RBOBJECT`` named this way carries the replacement mesh; a ``DUMMYOBJ``
#: named this way means the part is culled from that level on. Both ride the same
#: bone, so the distinction does not matter here.
LOD_NAME = re.compile(r"^L([1-9])#(.+)$")


def _collect_hierarchy(root):
    """Flatten the OBJCHIER tree into records, parents before children.

    Each record is ``(path, name, parent_path, bound, index, chunk)``. ``path`` is
    the same ``id:index`` form used for absorbed data, so a bone can be matched
    back to the chunk it came from on export.
    """
    out = []

    def walk(chunk, parent_path, prefix):
        for i, kid in enumerate(chunk.children or ()):
            if kid.id != b"OBJCHIER":
                continue
            path = "%s%s:%d" % (prefix, kid.name, i)
            bound = _hierarchy_binding(kid)
            name = bound or _label_for(kid) or "bone_%03d" % len(out)
            out.append((path, name, parent_path, bound, i, kid))
            walk(kid, path, path + "/")

    walk(root, None, "")
    return out


def _unique_bone_names(records):
    """Bone names must be unique within an armature; RIF names are not always."""
    seen = {}
    names = {}
    for path, name, _parent, _bound, _index, _chunk in records:
        base = name or "bone"
        n = seen.get(base, 0)
        seen[base] = n + 1
        names[path] = base if n == 0 else "%s.%03d" % (base, n)
    return names


def _build_armature(collection, root, targets, scale, y_down, fps):
    """The OBJCHIER tree as a real armature, with one Action per sequence.

    Node empties forced one Action per node, which meant a sequence could only be
    played by activating its strip on every node at once -- hence the sequential
    timeline this replaces. With bones, a sequence is a single Action on a single
    object, so every Action starts at frame 0 and switching is one dropdown.
    """
    records = _collect_hierarchy(root)
    if not records:
        return None, 0, 0, 0

    names = _unique_bone_names(records)

    arm_data = bpy.data.armatures.new("%s_rig" % collection.name)
    arm_obj = bpy.data.objects.new(arm_data.name, arm_data)
    collection.objects.link(arm_obj)
    arm_obj["rif_id"] = "OBJCHIER"

    # Rest transforms come from the object each node binds to, exactly as before:
    # nodes carry no transform of their own, and a keyframe is a bone's *full*
    # local transform rather than a delta, so the rest pose has to come from the
    # bound object.
    rest_world = {}
    for path, _name, parent_path, bound, _index, _chunk in records:
        target = targets.get(bound)
        if target is not None:
            rest_world[path] = target.matrix_basis.copy()
        else:
            rest_world[path] = rest_world.get(parent_path, mathutils.Matrix.Identity(4))

    view = bpy.context.view_layer
    view.objects.active = arm_obj
    bpy.ops.object.mode_set(mode="EDIT")
    try:
        for path, _name, parent_path, _bound, _index, _chunk in records:
            bone = arm_data.edit_bones.new(names[path])
            bone.head = (0.0, 0.0, 0.0)
            bone.tail = (0.0, BONE_LENGTH, 0.0)
            bone.matrix = rest_world[path]
            if parent_path is not None and names.get(parent_path) in arm_data.edit_bones:
                bone.parent = arm_data.edit_bones[names[parent_path]]
    finally:
        bpy.ops.object.mode_set(mode="OBJECT")

    # Bookkeeping the export needs, kept on the bones themselves.
    for path, _name, _parent, bound, index, chunk in records:
        bone = arm_data.bones[names[path]]
        bone["rif_path"] = path
        bone["rif_index"] = index
        bone["rif_bound"] = bound or ""
        bone["rif_absorbed"] = _pack_absorbed(_absorb(chunk, {b"OBJHIERD"}))
        # OBJHIERD is regenerated from the bound object's *current* name, so its
        # position has to be remembered separately. It is child 0 in 5,038 of the
        # 5,250 nodes that have one and child 1 in the other 212 -- and some
        # nodes have none at all, which is why -1 is a distinct case rather than
        # a default of 0.
        kids = list(chunk.children or ())
        bone["rif_hierd_index"] = next(
            (i for i, k in enumerate(kids) if k.id == b"OBJHIERD"), -1)
        seqs = chunk.find(b"OBANSEQS")
        if seqs is not None:
            bone["rif_anim_index"] = list(chunk.children or ()).index(seqs)
            bone["rif_anim_absorbed"] = _pack_absorbed(_absorb(seqs, {b"OBANSEQC"}))

    # Meshes ride their bone. Bone parenting attaches to the bone *tail*, not its
    # head, so the world matrix has to be reasserted -- but only once the
    # depsgraph knows the new parent, or the assignment is computed against a
    # stale parent matrix and the mesh ends up a bone-length away. That failure is
    # invisible until export: the object reads back at (0, 0, -BONE_LENGTH) in rif
    # units, rotated by the bone.
    pending = []
    rig_of_part = {}
    for path, _name, _parent, bound, _index, _chunk in records:
        target = targets.get(bound)
        if target is None:
            continue
        world = target.matrix_basis.copy()
        # Two nodes binding one name is legal, and the last one wins because it
        # re-parents. Recording the bone here rather than from `records` keeps a
        # variant on whichever bone its base actually ended up on.
        rig_of_part[bound] = (names[path], world)
        pending.append((target, world))
        target.parent = arm_obj
        target.parent_type = "BONE"
        target.parent_bone = names[path]
        target["rif_rig_parented"] = True

    # A variant goes onto the bone of the part it replaces, *at that part's
    # transform*. No node binds a variant -- the engine reaches it through the
    # base's name -- so without this it is the one thing in the file that does
    # not animate.
    #
    # Its own OBJHEAD1 placement is not used and must not be: the shipped sets
    # are parked beside the model (Gunlok MkII's L5 at +1200 on X, its L7 at
    # -1200), yet the vertices are already in the base part's local frame. Over
    # all 1,258 variant/base mesh pairs the local-frame centroid distance has a
    # median of 35 rif units against a ~2,000-unit character, and *applying* the
    # stored placement raises that to 115 (p90 1,202). So the placement is dead
    # data, exactly as AvP's loader implies by keeping only the shape. It is
    # still written back verbatim -- see `_object_chunk` -- because preserving a
    # field costs nothing and stays correct even if that reading is wrong.
    #
    # The guard is "the base is bound and the variant is not", not a name match:
    # `destructorfrag.RIF` has nine parts genuinely *called* `L7#head`,
    # `L7#pelvis` and so on with no base object at all, and its hierarchy binds
    # those names directly. Those are already parented by the loop above and must
    # be left alone. Across the 563 shipped files this covers 1,477 of the 1,518
    # variants; of the rest, 32 name a base that does not exist (four of them
    # typos, `L5#Face Pipe RIght`) and 9 name one no node binds. Both stay loose,
    # which is what the engine does with them too -- its own compare is
    # byte-exact.
    lod_pending = []
    for part_name, target in targets.items():
        match = LOD_NAME.match(part_name)
        if match is None or target.get("rif_rig_parented"):
            continue
        rig = rig_of_part.get(match.group(2))
        if rig is None:
            continue
        bone, world = rig
        lod_pending.append((target, world))
        target.parent = arm_obj
        target.parent_type = "BONE"
        target.parent_bone = bone
        target["rif_rig_parented"] = True
        target["rif_lod_base"] = match.group(2)

    pending += lod_pending
    bpy.context.view_layer.update()
    for target, world in pending:
        target.matrix_world = world

    # Variants are hidden, because they now *coincide* with the parts they
    # replace. That doubles the model, and it puts a second clickable copy over
    # every part: selecting the chest in the viewport hands you `L5#Chest`, and
    # since the Action editor targets the active object, picking a sequence there
    # silently assigns it to a mesh with no bone channels while the rig goes on
    # playing whichever action was bound at import. Nothing about that looks like
    # a selection mistake -- it looks like only one action can be played.
    #
    # `hide_set` (the eye), not `hide_viewport` (the monitor): the monitor toggle
    # drops the object from the depsgraph, which freezes `matrix_world` at
    # whatever it last evaluated to and makes the placement unverifiable. The eye
    # hides it and takes it out of click selection while it still evaluates.
    for target, _world in lod_pending:
        target.hide_set(True)
        target.hide_render = True

    sequences = _build_actions(arm_obj, records, names, scale, y_down, fps)
    arm_obj["rif_rest"] = [c for row in arm_obj.matrix_basis for c in row]

    # Leave the rig selected as well as active. Every mesh in the file rides a
    # bone, so the armature is the only object an Action means anything on, and
    # the Action editor works on whatever is active.
    view.objects.active = arm_obj
    arm_obj.select_set(True)
    return arm_obj, len(pending) - len(lod_pending), len(lod_pending), sequences


def _build_actions(arm_obj, records, names, scale, y_down, fps):
    """One Action per sequence, animating every bone that has that sequence."""
    arm_data = arm_obj.data
    per_sequence = {}
    for path, _name, _parent, _bound, _index, chunk in records:
        seqs = chunk.find(b"OBANSEQS")
        if seqs is None:
            continue
        for order, seq in enumerate(seqs.children or ()):
            if seq.id != b"OBANSEQC":
                continue
            key = _sequence_name(seq, "sequence_%03d" % order)
            per_sequence.setdefault(key, []).append((path, order, seq))

    anim = arm_obj.animation_data_create()
    made = 0
    first = None
    for key, entries in sorted(per_sequence.items(), key=lambda kv: min(e[1] for e in kv[1])):
        action = bpy.data.actions.new(key)
        if first is None:
            first = action
        action["rif_id"] = "OBANSEQC"
        action["rif_sequence"] = key
        action["rif_index"] = min(order for _p, order, _s in entries)
        action.use_fake_user = True

        slots = getattr(action, "slots", None)
        if slots is not None:
            slot = action.slots.new(id_type="OBJECT", name=arm_obj.name)
            strip = action.layers.new("Layer").strips.new(type="KEYFRAME")
            new_curve = strip.channelbag(slot, ensure=True).fcurves.new
        else:  # pragma: no cover - older Blender
            new_curve = action.fcurves.new

        frames_meta = []
        sound_events = []
        for path, order, seq in entries:
            bone_name = names[path]
            pose_bone = arm_obj.pose.bones.get(bone_name)
            if pose_bone is None:
                continue
            rest_rel = _rest_relative(arm_data.bones[bone_name])
            frames = [k for k in (seq.children or ()) if k.id == b"OBASEQFR"]

            tm = seq.find(b"OBASEQTM")
            duration_s = 1.0
            if tm is not None and len(tm.body) >= 4:
                duration_s = max(struct.unpack_from("<i", tm.body, 0)[0], 1) / 1000.0
            _read_sequence_settings(action, seq)

            # A sequence with no frames still has to be recorded: 973 of the
            # game's 29,550 are empty, and skipping them here drops the chunk.
            curves = {}
            if frames:
                base = 'pose.bones["%s"].' % bpy.utils.escape_identifier(bone_name)
                curves = {p: [new_curve(data_path=base + p, index=i) for i in range(n)]
                          for p, n in (("location", 3), ("rotation_quaternion", 4))}
            keys = []
            for fr in frames:
                props = schema.decode(b"OBASEQFR", fr.body)
                t = props["time"] / FIXED_ONE * duration_s * fps
                local = mathutils.Matrix.LocationRotation(
                    mathutils.Vector(to_blender(props["position"], scale, y_down)),
                    quat_to_blender(props["rotation"], y_down)) \
                    if hasattr(mathutils.Matrix, "LocationRotation") else \
                    (mathutils.Matrix.Translation(to_blender(props["position"], scale, y_down))
                     @ quat_to_blender(props["rotation"], y_down).to_matrix().to_4x4())
                basis = rest_rel.inverted() @ local
                loc = basis.to_translation()
                quat = basis.to_quaternion()
                for i, v in enumerate(loc):
                    curves["location"][i].keyframe_points.insert(t, v, options={"FAST"})
                for i, v in enumerate((quat.w, quat.x, quat.y, quat.z)):
                    curves["rotation_quaternion"][i].keyframe_points.insert(
                        t, v, options={"FAST"})
                # Anchored on the Blender frame, not on a position in the list:
                # export now emits whatever keyframes the F-curves actually
                # carry, so a key has to be recognisable as one that came from
                # the file however many have been inserted around it.
                #
                # The sound index is split out of `flags` rather than carried
                # inside it, so there is one place a sound is written. What is
                # left is the residual -- bit 31 and AvP's low flag mask -- which
                # nothing here understands and everything here preserves.
                flags = _to_unsigned32(props["flags"])
                sound = heads.frame_sound_index(flags)
                keys.append({"frame": t, "time": props["time"],
                             "flags": _to_signed32(flags & ~heads.FRAME_SOUND_INDEX_MASK)})
                if sound:
                    sound_events.append({"bone": bone_name, "frame": t, "index": sound})
            for group in curves.values():
                for fc in group:
                    fc.update()
            # OBASEQHD is regenerated (the name follows the Action, the id is
            # allocated per sequence), so it must not also ride along here or
            # every sequence would export two of them.
            frames_meta.append({"bone": bone_name, "path": path, "order": order,
                                "duration_s": duration_s, "keys": keys,
                                "absorbed": _pack_absorbed(
                                    _absorb(seq, {b"OBASEQFR", b"OBASEQHD"}))})
            if "rif_sub_sequence" not in action:
                head = seq.find(b"OBASEQHD")
                if head is not None:
                    _n, _s, sub = heads.seqhead_fields(head.body)
                    action["rif_sub_sequence"] = sub

        action["rif_bones"] = frames_meta
        # A sound belongs to the sequence at a time, not to a bone: it lives on
        # exactly one bone in 196 of the 208 shipped cases and on every bone in
        # none of them. The bone is still recorded, so an untouched export puts
        # the event back on the one that carried it.
        action["rif_sound_events"] = sound_events
        made += 1

    # Every Action starts at frame 0, so one is simply made active and switching
    # is the Action dropdown on this one object. No NLA, no timeline offsets, no
    # extrapolation games.
    if first is not None:
        anim.action = first
        slots = getattr(first, "slots", None)
        if slots is not None and len(slots):
            anim.action_slot = slots[0]
        scene = bpy.context.scene
        last = max((k.co[0] for fc in _iter_fcurves(first) for k in fc.keyframe_points),
                   default=1.0)
        scene.frame_start = 0
        scene.frame_end = max(int(round(last)), 1)
        scene.frame_set(0)
    return made


# --------------------------------------------------------------------------
# per-sequence settings: OBASEQTM, OBASEQFL, OBASEQSP
# --------------------------------------------------------------------------
#
# All three are optional -- 590, 722 and 582 of the 29,550 shipped sequences
# carry one -- and each is stored on a *subset* of the bones that have the
# sequence.
#
# They are **nearly** per-sequence: for 908 of the 912 (file, sequence) pairs
# that carry an OBASEQTM, every bone that has one has the same value. But not
# all. `game_cursor.RIF`'s DzSeq_Walk carries 800, 600 and 1000 on three
# different bones, `Binary Laser MkI.RIF` and `skyburn.RIF` disagree the same
# way, and `warflash.RIF` has one sequence flagged Loops on one bone and NoLoop
# on another. So the file's own per-bone value is authoritative and is left
# exactly where it is; the Action-level property is an *override*, applied only
# to the chunks whose setting the user actually edited (`rif_seq_edited`).
#
# (An earlier revision called these per-sequence outright. The measurement behind
# that counted "the bones disagree" and "at least one bone lacks it" and found
# the same number -- which does not imply the *present* values agree. The scene
# round-trip caught it on Binary Laser MkI.RIF.)
#
# **Present and absent are different states**, which is why these are ID
# properties that exist or do not, rather than numbers with a sentinel: adding
# `OBASEQTM` to a sequence that never had one is an edit, and so is removing it.

#: Action property <-> chunk. The value is a plain int except for the speed,
#: which is the three fields `schema` names.
SEQUENCE_SETTINGS = {
    b"OBASEQTM": "rif_seq_duration_ms",
    b"OBASEQFL": "rif_seq_flags",
    b"OBASEQSP": "rif_seq_speed",
}


def _read_sequence_settings(action, seq):
    """Lift a sequence's optional settings onto its Action, once.

    ``rif_seq_had`` records which of the three the *file* carried, on any bone.
    Without it export cannot tell "this bone legitimately has no OBASEQTM" from
    "the user just added a duration", and would append one to all 80 bones of a
    sequence that shipped it on three.
    """
    had = set(action.get("rif_seq_had", ()))
    for cid, prop in SEQUENCE_SETTINGS.items():
        kid = seq.find(cid)
        if kid is None:
            continue
        had.add(cid.decode("ascii"))
        if prop in action:
            continue
        props = schema.decode(cid, kid.body)
        if cid == b"OBASEQSP":
            action[prop] = [int(props.get("sequence_speed", 0)),
                            int(props.get("angle", 0)), int(props.get("spare", 0))]
        elif cid == b"OBASEQFL":
            action[prop] = _to_signed32(props.get("flags", 0))
        else:
            action[prop] = int(props.get("duration_ms", 0))
    action["rif_seq_had"] = sorted(had)


def _sequence_setting_body(action, cid):
    """The chunk body an Action's setting encodes to, or None when it has none."""
    prop = SEQUENCE_SETTINGS[cid]
    if prop not in action:
        return None
    value = action[prop]
    if cid == b"OBASEQSP":
        vals = list(value) + [0, 0, 0]
        return schema.encode(cid, {"sequence_speed": int(vals[0]),
                                   "angle": int(vals[1]), "spare": int(vals[2])})
    if cid == b"OBASEQFL":
        return schema.encode(cid, {"flags": _to_signed32(_to_unsigned32(int(value)))})
    return schema.encode(cid, {"duration_ms": int(value)})


def _apply_sequence_settings(action, extras):
    """Rewrite the settings chunks in one sequence's absorbed extras, in place.

    Substituting into the absorbed list rather than appending is what keeps a
    chunk's position: it sits after the frames, but *where* after them varies and
    nothing about that is worth regenerating.

    **Only a setting the user edited is touched.** An untouched one keeps the
    file's own body, which is what preserves the handful of sequences whose bones
    genuinely carry different values. An edited one is substituted everywhere, or
    dropped everywhere if it was removed -- that is how you remove one.

    A setting the Action has and this bone's sequence does not is appended **only
    when the file never had it on any bone** (``rif_seq_had``). Otherwise a
    sequence that shipped its duration on three bones of eighty would come back
    carrying it on all eighty.
    """
    edited = set(action.get("rif_seq_edited", ()))
    if not edited:
        return list(extras)

    had = set(action.get("rif_seq_had", ()))
    out = []
    seen = set()
    for index, chunk in extras:
        if chunk.id in SEQUENCE_SETTINGS and chunk.id.decode("ascii") in edited:
            body = _sequence_setting_body(action, chunk.id)
            seen.add(chunk.id)
            if body is None:
                continue  # removed on the Action
            chunk = rif.Chunk(chunk.id, body)
        out.append((index, chunk))
    top = max([i for i, _c in out], default=-1) + 1
    for cid in (b"OBASEQFL", b"OBASEQTM", b"OBASEQSP"):
        if cid in seen or cid.decode("ascii") not in edited \
                or cid.decode("ascii") in had:
            continue
        body = _sequence_setting_body(action, cid)
        if body is not None:
            # The order the shipped files use when all three are present.
            out.append((top, rif.Chunk(cid, body)))
            top += 1
    return out


def _rest_relative(bone):
    """A bone's rest transform relative to its parent, which is what a pose basis
    is measured against: ``pose_local = rest_relative @ matrix_basis``."""
    if bone.parent is None:
        return bone.matrix_local.copy()
    return bone.parent.matrix_local.inverted() @ bone.matrix_local


def _bone_binding(arm_obj, bone):
    """The object name this node drives, followed through a rename.

    The binding is a *name*, resolved by ``strcmp`` -- so reading it back off the
    object actually parented to this bone is what makes renaming that object
    (which the UI now offers) keep the rig working, instead of silently leaving
    the node pointing at a name nothing answers to.

    A level-of-detail variant rides a bone too and is deliberately not a binding:
    no node binds one, which is the whole reason `_build_armature` has to place
    it by hand. When the bone drives no object, or several, the imported value is
    kept -- "two nodes binding one name is legal" cuts both ways.
    """
    driven = [obj for obj in arm_obj.children
              if obj.parent_type == "BONE" and obj.parent_bone == bone.name
              and obj.get("rif_rig_parented") and "rif_lod_base" not in obj]
    if len(driven) == 1:
        name = rif_object_name(driven[0])
        if name:
            return name
    return bone.get("rif_bound", "") or ""


def _armature_chunks(arm_obj, scale, y_down, fps):
    """The armature back into ``[(index, OBJCHIER chunk)]`` for the file root.

    **Nesting comes from ``bone.parent``, not from the ``rif_path`` strings an
    import recorded.** Those paths are still carried, because they are what an
    absorbed chunk's position is relative to, but reading the tree out of them
    meant a bone added or re-parented in Blender could not change the file: a new
    bone had no path at all and was skipped in silence, and a re-parented one
    went back exactly where it came from.
    """
    arm = arm_obj.data
    bones = list(arm.bones)

    # Actions, grouped by the bone they animate. The grouping is read from the
    # F-curve data paths rather than from stored metadata, so an Action that
    # gained a bone -- or that never had metadata because a script made it --
    # still reaches the file.
    per_bone = {}
    for action in _rif_actions(arm_obj):
        meta = {entry["bone"]: entry for entry in action.get("rif_bones", ())}
        # The **union** of "has curves" and "had a sequence chunk". Curves alone
        # is not enough: 973 of the game's 29,550 sequences carry no frames at
        # all, and an empty sequence produces no F-curve to be found by, so
        # keying on curves alone silently drops the chunk.
        for bone_name in set(meta) | _animated_bones(action):
            per_bone.setdefault(bone_name, []).append((action, meta.get(bone_name)))

    sub_ids = _sequence_ids(arm_obj)

    made = {}
    for index, bone in enumerate(bones):
        children = list(_emit_from(bone.get("rif_absorbed")))

        # Regenerated rather than carried, because the name in it *is* the
        # binding and it has to follow a rename. Emitted only when there is a
        # binding to express or the node already had the chunk -- some shipped
        # nodes have neither, and inventing an empty one for those would add a
        # chunk nobody asked for.
        binding = _bone_binding(arm_obj, bone)
        hierd_index = int(bone.get("rif_hierd_index", 0 if binding else -1))
        if binding or hierd_index >= 0:
            # Half a step before whatever held that index, so it lands back in
            # its own slot without having to renumber every sibling around it.
            children.append((max(hierd_index, 0) - 0.5,
                             rif.Chunk(b"OBJHIERD", heads.make_objhierd(binding))))

        seqs = _sequences_for(bone, per_bone.get(bone.name, ()), arm_obj,
                              scale, y_down, fps, sub_ids)
        if seqs is not None:
            children.append((bone.get("rif_anim_index", len(children)), seqs))

        made[bone.name] = (rif.Chunk(b"OBJCHIER", b"", []), children,
                           int(bone.get("rif_index", index)))

    roots = []
    for bone in bones:
        chunk, _kids, index = made[bone.name]
        parent = bone.parent
        if parent is not None and parent.name in made:
            made[parent.name][1].append((index, chunk))
        else:
            roots.append((index, chunk))

    for chunk, children, _index in made.values():
        children.sort(key=lambda t: t[0])
        chunk.children = [c for _, c in children]
    return roots


def _rif_actions(arm_obj):
    """Every Action that should become a sequence, in a stable order.

    An Action earns this by being marked -- ``rif_id`` from an import, or the
    same key set by *Add Action to Gunlok RIF*. Auto-detecting instead would
    sweep up every scratch Action in the file, and an Action is not owned by the
    object it happens to be assigned to.
    """
    out = [a for a in bpy.data.actions if a.get("rif_id") == "OBANSEQC"]
    out.sort(key=lambda a: (int(a.get("rif_index", 0)), a.name))
    return out


def _animated_bones(action):
    """The pose bones an Action has curves for."""
    found = set()
    for fc in _iter_fcurves(action):
        if fc.data_path.startswith('pose.bones["'):
            rest = fc.data_path[len('pose.bones["'):]
            end = rest.find('"]')
            if end > 0:
                found.add(rest[:end].replace('\\"', '"').replace("\\\\", "\\"))
    return found


def _sequence_ids(arm_obj):
    """``{action name: sub_sequence_number}``, allocating one per new sequence.

    That field is a per-file sequence id -- the same value on every node's copy
    of one sequence, distinct between sequences (see `heads.make_seqhead`) -- so
    a new Action needs a number nothing else in the file is using, and it has to
    be the same number on every bone.
    """
    ids, used = {}, set()
    for action in _rif_actions(arm_obj):
        stored = action.get("rif_sub_sequence")
        if stored is not None:
            ids[action.name] = int(stored)
            used.add(int(stored))
    nxt = max(used, default=-1) + 1
    for action in _rif_actions(arm_obj):
        if action.name not in ids:
            ids[action.name] = nxt
            action["rif_sub_sequence"] = nxt
            used.add(nxt)
            nxt += 1
    return ids


def _sequences_for(bone, entries, arm_obj, scale, y_down, fps, sub_ids):
    """One bone's Actions back into its OBANSEQS chunk."""
    if not entries:
        return None
    out = []
    for order, (action, entry) in enumerate(entries):
        position = int(entry["order"]) if entry else int(action.get("rif_index", order))
        out.append((position, _bone_sequence_chunk(action, entry, bone.name, arm_obj,
                                                   scale, y_down, fps, sub_ids)))
    out.sort(key=lambda t: t[0])
    children = out + list(_emit_from(bone.get("rif_anim_absorbed")))
    children.sort(key=lambda t: t[0])
    return rif.Chunk(b"OBANSEQS", b"", [c for _, c in children])


def _action_extent(action):
    """``(first, last)`` keyframe position over every bone this Action animates.

    Read off the curves rather than from ``action.frame_range``, which pads a
    single-key Action out to a whole frame and would put that key's `time`
    somewhere other than 0.
    """
    lo, hi = None, None
    for fc in _iter_fcurves(action):
        for kp in fc.keyframe_points:
            f = float(kp.co[0])
            lo = f if lo is None or f < lo else lo
            hi = f if hi is None or f > hi else hi
    return None if lo is None else (lo, hi)


def _keyframe_positions(curves):
    """Every distinct frame any of this bone's curves has a key at, sorted.

    The union rather than one curve's, because Blender keys each channel
    separately and a `.rif` frame is a bone's whole transform: a key on Z alone
    still has to become an OBASEQFR, with the other channels sampled there.
    """
    seen = []
    for group in curves.values():
        for fc in group.values():
            for kp in fc.keyframe_points:
                f = float(kp.co[0])
                if not any(abs(f - g) < 1e-4 for g in seen):
                    seen.append(f)
    return sorted(seen)


def _bone_sequence_chunk(action, entry, bone_name, arm_obj, scale, y_down, fps, sub_ids):
    """One (Action, bone) pair back into an OBANSEQC chunk.

    **The frame list comes from the F-curves, not from what the import recorded.**
    It used to be the other way round -- the stored list was iterated and the
    curves merely sampled at those times -- which meant inserting a keyframe in
    Blender changed what a pose looked like but could never change how many
    frames the sequence had, and an Action created from scratch produced nothing
    at all.

    What the import recorded is still used, as *anchors*: a key that came from
    the file keeps its exact ``time``, because those values are authored and no
    formula reproduces them (see `heads.sequence_times`).
    """
    # `duration_s` is deliberately not read here any more. It was how the old
    # code turned a stored `time` back into a frame position to sample at;
    # positions now come from the keyframes themselves, and the duration itself
    # rides through untouched in OBASEQTM.
    entry = entry or {}
    rest_rel = _rest_relative(arm_obj.data.bones[bone_name])

    base = 'pose.bones["%s"].' % bpy.utils.escape_identifier(bone_name)
    curves = {}
    for fc in _iter_fcurves(action):
        if fc.data_path.startswith(base):
            curves.setdefault(fc.data_path[len(base):], {})[fc.array_index] = fc

    anchors = [(float(k["frame"]), int(k["time"])) for k in entry.get("keys", ())]
    # An imported sequence with no keys at all is one of the 973 empty ones; it
    # still has to come back, so an empty frame list is not the same as no entry.
    positions = _keyframe_positions(curves)
    times = heads.sequence_times(positions, anchors, _action_extent(action))
    flags_at = {float(k["frame"]): int(k["flags"]) for k in entry.get("keys", ())}
    sounds_at = [(float(e["frame"]), int(e["index"]))
                 for e in action.get("rif_sound_events", ())
                 if e["bone"] == bone_name]

    frames = []
    for n, (t, time_value) in enumerate(zip(positions, times)):
        loc = [curves.get("location", {}).get(i).evaluate(t)
               if curves.get("location", {}).get(i) else 0.0 for i in range(3)]
        quat = [curves.get("rotation_quaternion", {}).get(i).evaluate(t)
                if curves.get("rotation_quaternion", {}).get(i) else (1.0 if i == 0 else 0.0)
                for i in range(4)]
        basis = (mathutils.Matrix.Translation(loc)
                 @ mathutils.Quaternion(quat).to_matrix().to_4x4())
        local = rest_rel @ basis
        # The residual flags -- bit 31 and AvP's low mask -- are preserved from the
        # file and never generated; a new key gets 0, which is what 96.8% of
        # shipped frames carry. The sound index is spliced back in from the
        # Action's own event list, so a sound is edited in exactly one place.
        flags = next((v for f, v in flags_at.items() if abs(f - t) < 1e-4), 0)
        sound = next((v for f, v in sounds_at if abs(f - t) < 1e-4), 0)
        flags = (_to_unsigned32(flags) & ~heads.FRAME_SOUND_INDEX_MASK) \
            | ((sound & 0x7F) << 24)
        props = {
            "rotation": list(quat_to_rif(local.to_quaternion(), y_down)),
            "position": list(to_rif(local.to_translation(), scale, y_down)),
            "time": int(time_value),
            # `frame_index` is the position in the list, in 323,334 of 323,334
            # shipped frames, so it is regenerated and insertion renumbers.
            "frame_index": n,
            "flags": _to_signed32(flags),
            "num_extra_data": 0,  # zero in every shipped frame
        }
        frames.append((n, rif.Chunk(b"OBASEQFR", schema.encode(b"OBASEQFR", props))))

    # Order is not negotiable and not guessed: OBASEQHD is child 0 in all 29,550
    # shipped sequences, the frames follow it, and the optional extras
    # (OBASEQFL / OBASEQTM / OBASEQSP / HIERBBOX) come after those. Offsetting
    # the absorbed extras past the frame count is what keeps that true when a
    # sequence gains keys.
    name = action.get("rif_sequence", action.name)
    head = rif.Chunk(b"OBASEQHD", heads.make_seqhead(name, sub_ids.get(action.name, 0)))
    extras = _apply_sequence_settings(action, _emit_from(entry.get("absorbed")))
    extras = sorted(extras, key=lambda t: t[0])
    ordered = [head] + [c for _, c in frames] + [c for _, c in extras]
    return rif.Chunk(b"OBANSEQC", b"", ordered)












def _iter_fcurves(action):
    slots = getattr(action, "slots", None)
    if slots is None:  # pragma: no cover - older Blender
        yield from action.fcurves
        return
    for layer in action.layers:
        for strip in layer.strips:
            for bag in strip.channelbags:
                yield from bag.fcurves


_CTX = {"scale": DEFAULT_SCALE, "y_down": True, "fps": 30.0, "fuse_quads": True,
        "textures": {}, "materials": {}, "images": {}, "texture_index": None,
        "table_chunk": None,
        "missing_textures": set(), "undecodable_textures": set()}


def _link(obj, collection, parent):
    collection.objects.link(obj)
    if parent is not None:
        obj.parent = parent


def _mesh_for_shape(shape_chunk, name):
    """REBSHAPE -> a Blender mesh, authored per-face data included as attributes.

    Returns ``(mesh, polygons lost, merge pairs built as quads)``.
    """
    shape = shp.read_shape(shape_chunk)
    me = bpy.data.meshes.new(name)
    if shape is None or not shape.verts:
        return me, 0, 0

    verts = [to_blender(v, _CTX["scale"], _CTX["y_down"]) for v in shape.verts]

    # A **pair id**, not the wire value: the wire names a partner by index, and
    # this importer changes the numbering (it drops faces Blender cannot hold).
    # Both partners get the same id, so the pairing survives that; see
    # `shapes.merge_pairs_from_wire`.
    merge_vals = []
    merge = shape_chunk.find(b"SHPMRGDT")
    if merge is not None:
        n = len(merge.body) // 4
        merge_vals = shp.merge_pairs_from_wire(
            list(struct.unpack_from("<%di" % n, merge.body, 0)), len(shape.polys))

    # `plan_faces` drops the faces Blender cannot hold -- two on the same three
    # vertices, 775 of them across 193 shipped shapes -- and, given the pairing,
    # fuses each merge pair into the quad it stands for. Both decisions are made
    # over the *source* numbering and returned index-for-index with `kept`,
    # which is what keeps the per-face data attached to the right face.
    kept, lost = shp.plan_faces(shape, merge_vals if _CTX["fuse_quads"] else None)
    faces = [f.verts for f in kept]

    me.from_pydata(verts, [], faces)
    me.validate(verbose=False)
    if len(me.polygons) != len(kept):  # nothing shipped hits this
        lost += sum(len(f.sources) for f in kept[len(me.polygons):])
        kept = kept[:len(me.polygons)]
    quads = sum(1 for f in kept if len(f.sources) == 2)

    slot_of = {}
    scale_of = {}
    for ti in sorted({p.texture_index for p in shape.polys}):
        mat = _material_for(ti)
        slot_of[ti] = len(me.materials)
        scale_of[ti] = uv_scale(mat, _CTX["textures"].get(ti))
        me.materials.append(mat)

    et = me.attributes.new("rif_engine_type", "INT", "FACE")
    fl = me.attributes.new("rif_flags", "INT", "FACE")
    hu = me.attributes.new("rif_has_uv", "BOOLEAN", "FACE")
    mg = me.attributes.new(MERGE_PAIR_ATTR, "INT", "FACE")

    uv_layer = me.uv_layers.new(name="UVMap")
    for i, (poly, plan) in enumerate(zip(me.polygons, kept)):
        # A fused quad takes its attributes from the lower-indexed partner,
        # which is also the one the engine's merger inherits from -- and
        # `fuse_quad` only fuses a pair that agrees on all three anyway.
        source_index = plan.sources[0]
        src = shape.polys[source_index]
        poly.material_index = slot_of.get(src.texture_index, 0)
        et.data[i].value = src.engine_type
        fl.data[i].value = src.flags
        # Indexed by where the polygon was in the *file*, not where it ended up
        # in the mesh -- which is exactly the renumbering that made storing the
        # raw wire value wrong. A quad keeps its id too: split it back into two
        # triangles and both halves still name the same pair.
        mg.data[i].value = (merge_vals[source_index]
                            if source_index < len(merge_vals) else shp.MERGE_NONE)
        uvs = plan.uvs
        hu.data[i].value = uvs is not None
        scale = scale_of.get(src.texture_index, (1.0, 1.0))
        for k, loop in enumerate(poly.loop_indices):
            if uvs is None:
                uv_layer.data[loop].uv = (0.0, 0.0)
            elif k < len(uvs):
                uv_layer.data[loop].uv = uv_to_blender(uvs[k], scale)

    me.update()
    # Presence is a separate question from the values, the same way
    # `rif_vtint_header` marks a SHPVTINT. `SHPCENTR` is *recomputed* rather than
    # carried, so without this marker a shape that shipped without one acquires
    # one on export -- 2 of Battler Turret's 4 shapes, and the reason the chunk
    # inventory did not balance. A shape authored from scratch has no marker and
    # so does get one, which is what a well-formed new shape wants.
    if shape_chunk.find(b"SHPCENTR") is None:
        me[NO_CENTRE_PROP] = 1
    return me, lost, quads


def _hierarchy_binding(chunk):
    """The object name an OBJCHIER node binds to, out of its OBJHIERD.

    ``OBJHIERD`` is ``{int32 num_extra_data, int32[n] extra_data, name}`` -- the
    count is 0 in all 5,250 shipped nodes, so the name starts at 4. AvP's
    ``Object_Hierarchy_Data_Chunk::find_object_for_this_section`` resolves it with
    ``strcmp`` against each object's name, which is the only binding there is: it
    reaches 3,541 RBOBJECT and 1,488 DUMMYOBJ, and 221 nodes match nothing at all
    (AvP leaves those with a null object, so a dangling node is legal).
    """
    hd = chunk.find(b"OBJHIERD")
    if hd is None or len(hd.body) < 4:
        return ""
    return _trailing_name(hd.body, 4)


def _label_for(chunk):
    bound = _hierarchy_binding(chunk)
    if bound:
        return bound
    for cid in (b"OBHIERNM", b"RIFFNAME", b"CUTTRNAM", b"CTUSRHIE"):
        nm = chunk.find(cid)
        if nm is not None:
            text = nm.body.split(b"\0")[0].decode("latin-1")
            if text:
                return text
    return chunk.name


def _build(chunk, index, parent_obj, collection, pairs, root_kids, stats):
    """Create the datablock for one chunk, recurse, return the object."""
    cid = chunk.id

    if cid in LEAF_AS_OBJECT:
        light = bpy.data.lights.new("light_%03d" % index, type="POINT")
        obj = bpy.data.objects.new(light.name, light)
        _apply_light(obj, chunk, index, _CTX["scale"], _CTX["y_down"])
        _link(obj, collection, parent_obj)
        return obj

    skip = set(LEAF_AS_OBJECT)

    if cid == b"RBOBJECT":
        head = chunk.find(b"OBJHEAD1")
        data = (list(struct.unpack_from("<%di" % (len(head.body) // 4), head.body, 0))
                if head is not None else [])
        name = _objhead_name(head.body if head is not None else b"") or "object_%03d" % index

        shape_index = pairs.get(index)
        if shape_index is not None:
            shape_chunk = root_kids[shape_index]
            me, lost, quads = _mesh_for_shape(shape_chunk, name)
            stats["lost_faces"] += lost
            stats["quads"] += quads
            obj = bpy.data.objects.new(name, me)
            me["rif_id"] = "REBSHAPE"
            me["rif_index"] = shape_index
            me["rif_absorbed"] = _pack_absorbed(
                _absorb(shape_chunk,
                        GEOMETRY_CHUNKS | set(ATTRIBUTE_CHUNKS) | DISCARDED_CHUNKS))
        else:
            obj = bpy.data.objects.new(name, None)

        if len(data) >= OH_ORIENT.stop:
            obj.location = to_blender(data[OH_LOCATION], _CTX["scale"], _CTX["y_down"])
            obj.rotation_mode = "QUATERNION"
            obj.rotation_quaternion = quat_to_blender(
                [_bits_to_float(x) for x in data[OH_ORIENT]], _CTX["y_down"])
        obj["rif_objhead"] = data
        skip.update((b"OBJHEAD1", b"SHPVTINT"))

        # SHPVTINT is per-vertex lighting held on the object; it belongs on the
        # mesh, and it lands as the paintable colour attribute directly -- that
        # is the stored form, not a view of one (see "Baked vertex lighting").
        # The header rides along as the marker that says this object has a chunk
        # at all, which an all-white attribute could not.
        vt = chunk.find(b"SHPVTINT")
        if vt is not None and obj.data is not None and len(vt.body) >= 16:
            n = (len(vt.body) - 16) // 4
            vals = struct.unpack_from("<%di" % n, vt.body, 16)
            attr_name, _existed = white_light_attribute(obj.data)
            attr = obj.data.color_attributes[attr_name]
            for i in range(min(n, len(attr.data))):
                r, g, b, a = unpack_light(vals[i])
                attr.data[i].color_srgb = (r / 255.0, g / 255.0, b / 255.0, a / 255.0)
            obj.data[VTINT_HEADER_PROP] = list(struct.unpack_from("<4i", vt.body, 0))
    elif cid in (b"REBSHAPE", b"SUBSHAPE"):
        # A shape no object claims -- Elint MkII ships two, of 4 vertices each.
        # It still gets a real mesh rather than an empty, or its geometry would be
        # visible only as a typed array.
        me, lost, quads = _mesh_for_shape(chunk, _label_for(chunk))
        stats["lost_faces"] += lost
        stats["quads"] += quads
        obj = bpy.data.objects.new(me.name, me)
        skip |= GEOMETRY_CHUNKS | set(ATTRIBUTE_CHUNKS) | DISCARDED_CHUNKS
    else:
        # An ambient emitter is a Speaker rather than an Empty -- the one sound
        # in the game with a position, so the one whose transform and falloff
        # mean anything. A dummy whose text is not a `Sound` stays a marker.
        data = None
        if cid == b"DUMMYOBJ":
            text = _dumobjtx_text(chunk.find(b"DUMOBJTX"))
            if text is not None and emitters.is_emitter(text):
                data = bpy.data.speakers.new(_label_for(chunk))
        obj = bpy.data.objects.new(_label_for(chunk), data)

    obj["rif_id"] = chunk.name
    obj["rif_index"] = index
    # A container that became an object may be the one holding the texture
    # table -- REBENVDT does whenever it also holds a LIGHTSET.
    skip = _note_table_owner(chunk, obj, skip)

    # What the rig pass needs: the name an object answers to, and the name a
    # hierarchy node is looking for.
    if cid == b"RBOBJECT":
        obj["rif_name"] = name
    elif cid == b"DUMMYOBJ":
        dt = chunk.find(b"DUMOBJDT")
        dname = _trailing_name(dt.body, DUMOBJDT_NAME) if dt is not None else ""
        if dname:
            obj["rif_name"] = dname
            obj.name = dname
        if dt is not None and len(dt.body) >= DUMOBJDT_NAME:
            obj["rif_dumobjdt"] = list(
                struct.unpack_from("<%di" % (DUMOBJDT_NAME // 4), dt.body, 0))
            obj.location = to_blender(
                struct.unpack_from("<3i", dt.body, DUMOBJ_LOCATION), _CTX["scale"],
                _CTX["y_down"])
            obj.rotation_mode = "QUATERNION"
            obj.rotation_quaternion = quat_to_blender(
                struct.unpack_from("<4f", dt.body, DUMOBJ_ORIENT), _CTX["y_down"])
            if obj.data is None:
                obj.empty_display_type = "CUBE"
            skip.add(b"DUMOBJDT")
        # The emitter half: lifted out of `rif_absorbed` like the two tables,
        # because the scene models it as the Speaker's own settings. Its child
        # index is remembered so export puts the chunk back where it sat.
        if obj.data is not None:
            for i, kid in enumerate(chunk.children or ()):
                if kid.id != b"DUMOBJTX":
                    continue
                obj[EMITTER_INDEX_PROP] = i
                _apply_emitter(obj, _dumobjtx_text(kid), _CTX.get("sound_dir"))
                skip.add(b"DUMOBJTX")
                break
    elif cid == b"OBJCHIER":
        bound = _hierarchy_binding(chunk)
        if bound:
            obj["rif_bound"] = bound

    # **After** every branch above, because each one decides what it has taken
    # out of the chunk and must not be absorbed as well. This used to sit before
    # them, which worked for `RBOBJECT` (it adds to `skip` in the first if-chain,
    # above) and silently did nothing for `DUMMYOBJ`, whose `skip.add(DUMOBJDT)`
    # ran after the absorb had already copied it -- so every dummy exported its
    # DUMOBJDT twice, the absorbed original and the regenerated one. 6,847 of
    # them across the shipped set.
    obj["rif_absorbed"] = _pack_absorbed(_absorb(chunk, skip))

    _link(obj, collection, parent_obj)

    for i, kid in enumerate(chunk.children or ()):
        if kid.id in (b"OBANSEQS", b"OBJCHIER"):
            continue  # the armature owns the hierarchy and its animation
        if kid.children is None and kid.id not in LEAF_AS_OBJECT:
            continue  # a leaf, already absorbed above
        if kid.children is not None and kid.id not in OBJECT_CHUNKS and _is_data_only(kid):
            continue  # a container of pure data, also already absorbed
        _build(kid, i, obj, collection, {}, [], stats)
    return obj












def build_scene(root, name, scale=DEFAULT_SCALE, y_down=True, fps=30.0,
                source_path=None, texture_dir="", load_images=True,
                fuse_quads=True):
    """Chunk tree -> a Blender collection holding the whole file.

    ``source_path`` is only used to find the textures: a ``.RIM`` is not in the
    ``.rif``, so the install has to be located by walking up from the file that
    named it (``texture_dir`` overrides that). Nothing else reads it, and an
    import with no textures found differs from one with them only in what the
    materials display.

    ``fuse_quads`` builds each ``SHPMRGDT`` pair as one quad instead of two
    triangles -- the same fusion the engine performs on a map object, and the
    form the geometry was authored in. Export writes triangles either way.
    """
    _CTX.update(scale=scale, y_down=y_down, fps=fps, fuse_quads=fuse_quads,
                textures={}, materials={}, images={}, texture_index=None,
                table_chunk=None, sound_dir=None,
                missing_textures=set(), undecodable_textures=set())
    stats = {"lost_faces": 0, "objects": 0, "quads": 0}

    collection = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(collection)
    collection["rif_id"] = "REBINFF2"
    collection["rif_scale"] = scale
    collection["rif_y_down"] = y_down
    collection["rif_fps"] = fps

    # Found before anything is built, because an ambient emitter loads its own
    # `.wav` as the object is created -- unlike the INDSOUND table, which is
    # audio a *panel* offers rather than audio an object holds.
    _CTX["sound_dir"] = _sound_root(source_path, "") if load_images else None
    collection["rif_sound_dir"] = _CTX["sound_dir"] or ""

    _CTX["textures"] = _read_texture_table(root, collection)
    root_dir = _texture_root(source_path, texture_dir) if load_images else None
    if root_dir:
        _CTX["texture_index"] = rim.TextureIndex(root_dir)
    stats["texture_root"] = root_dir
    stats["textures"] = len(_CTX["textures"])

    _CTX["sounds"] = _read_sound_table(root, collection)
    stats["sounds"] = len(_CTX["sounds"])

    kids = list(root.children or ())
    pairs = _shape_pairs(root)
    consumed = set(pairs.values())

    absorbed = []
    for i, kid in enumerate(kids):
        if kid.id == b"REBSHAPE" and i in consumed:
            continue  # built as the mesh of the object that names it
        if kid.id == b"OBJCHIER":
            continue  # the armature owns the whole hierarchy
        if kid.id in _SOUND_CHUNKS and _CTX["sounds"]:
            continue  # lifted onto the collection as a table, rebuilt on export
        if kid.children is None and kid.id not in LEAF_AS_OBJECT:
            absorbed.append(("%s:%d" % (kid.name, i), schema.decode(kid.id, kid.body)))
            continue
        if kid.children is not None and kid.id not in OBJECT_CHUNKS and _is_data_only(kid):
            # A file-level container of pure data (REBENVDT, the cutscene tracks)
            # lands on the collection instead of becoming an empty. BMPNAMES is
            # the one exception: it is already on the collection as a table, and
            # export rebuilds it from there.
            prefix = "%s:%d/" % (kid.name, i)
            lifted = _note_table_owner(kid, collection, set(), prefix)
            nested = _absorb(kid, lifted, prefix)
            absorbed.extend(nested if nested else [("%s:%d" % (kid.name, i), {})])
            continue
        _build(kid, i, None, collection, pairs, kids, stats)
        stats["objects"] += 1

    collection["rif_absorbed"] = _pack_absorbed(absorbed)
    stats["sounds_loaded"] = _load_sound_audio(_CTX["sounds"], _CTX["sound_dir"])
    stats["emitters"] = sum(1 for o in collection.objects if EMITTER_TEXT_PROP in o)
    stats["images"] = len(_CTX["images"])
    stats["missing_textures"] = sorted(_CTX["missing_textures"])
    stats["undecodable_textures"] = sorted(_CTX["undecodable_textures"])
    targets = {o["rif_name"]: o for o in collection.objects
               if "rif_name" in o and o.get("rif_id") in ("RBOBJECT", "DUMMYOBJ")}
    _arm, rigged, lod_rigged, sequences = _build_armature(
        collection, root, targets, scale, y_down, fps)
    stats["rigged"] = rigged
    stats["lod_rigged"] = lod_rigged
    stats["sequences"] = sequences
    # After the armature, because this moves entries out of `rif_absorbed` and
    # the rig pass reads that for its own chunks.
    stats["cutscenes"] = build_cutscenes(collection, scale, y_down)
    return collection, stats


# --------------------------------------------------------------------------
# scene -> chunk tree
# --------------------------------------------------------------------------

def _image_size(mat):
    for node in (mat.node_tree.nodes if mat.use_nodes and mat.node_tree else ()):
        if node.type == "TEX_IMAGE" and node.image is not None:
            return node.image.size[0], node.image.size[1]
    return 0, 0


def _texture_table(collection):
    """``(entries, {material name: (texture index, uv scale)})`` for this export.

    The collection's table is kept whole and in order -- entries no polygon
    references are as much part of the file as the ones that are -- and a
    material naming a texture the table does not list appends one. That is the
    only way the table grows, and it is what makes retexturing work: put a
    different ``.RIM`` path in a material's ``rif_bmp_name`` and the polygons
    wearing it move to that entry, gaining one if the file never mentioned it.

    A material with no ``rif_bmp_name`` falls back to its raw
    ``rif_texture_index``, which is how the ``0xfff`` untextured sentinel and
    the junk indices in the ``_shadow`` meshes survive a round trip.
    """
    entries = [_unpack_entry(e) for e in collection.get("rif_bmpnames", ())]
    by_name = {e["name"].replace("\\", "/").lower(): e for e in entries}
    next_index = max([e["index"] for e in entries], default=0) + 1

    mapping = {}
    for mat in _materials_in(collection):
        name = (mat.get("rif_bmp_name", "") or "").strip()
        if not name:
            mapping[mat.name] = (int(mat.get("rif_texture_index", shp.TEXTURE_INDEX_MASK)),
                                 uv_scale(mat))
            continue
        entry = by_name.get(name.replace("\\", "/").lower())
        if entry is None:
            width, height = _image_size(mat)
            entry = bmpnames.make_entry(name, next_index, width, height)
            next_index += 1
            entries.append(entry)
            by_name[name.replace("\\", "/").lower()] = entry
        # Re-derived, not replayed: a material moved onto a texture of another
        # size has to write its UVs in *that* texture's texels.
        mapping[mat.name] = (entry["index"], uv_scale(mat, entry))
    return entries, mapping


def _materials_in(collection):
    seen = []
    for obj in collection.objects:
        for mat in (obj.data.materials if getattr(obj.data, "materials", None) else ()):
            if mat is not None and mat not in seen:
                seen.append(mat)
    return seen


def _table_location(collection):
    """``(owner, path)`` for the texture table: which datablock puts it back.

    ``owner`` is ``""`` for the collection and an object's name otherwise --
    names rather than the datablocks themselves, because Blender hands out a
    fresh Python wrapper per access and ``is`` on two of them is not reliable.

    The table sits under the file-level ``REBENVDT``, which is data folded onto
    the collection in most files but an *object* in the 34 that keep a
    ``LIGHTSET`` there. Both cases are recorded at import by
    :func:`_note_table_owner`, so this only has to find which one happened.
    """
    path = collection.get("rif_bmpnames_path")
    if path:
        return "", path
    for obj in collection.objects:
        path = obj.get("rif_bmpnames_path")
        if path:
            return obj.name, path
    return None, None


def _new_table_location(collection):
    """The same, for a file that never had a table: after ``REBENVDT``'s children.

    That is where AvP's loader looks (``edc->lookup_single_child("BMPNAMES")``)
    and where all 527 shipped tables sit. Every shipped file has exactly one
    ``REBENVDT``, including the 36 with no table, so there is always somewhere
    to put it.
    """
    prefix, last = None, -1
    for path, _props in _unpack_absorbed(collection.get("rif_absorbed")):
        head, _, tail = path.rpartition("/")
        if head.startswith("REBENVDT:") and "/" not in head:
            prefix = head
            last = max(last, _path_index(tail))
    if prefix:
        return "", "%s/BMPNAMES:%d" % (prefix, last + 1)
    for obj in collection.objects:
        if obj.get("rif_id") == "REBENVDT":
            used = [_path_index(p) for p, _ in _unpack_absorbed(obj.get("rif_absorbed"))]
            return obj.name, "BMPNAMES:%d" % (max(used, default=-1) + 1)
    return None, None


def _path_index(segment):
    return int(segment.rpartition("/")[2].rpartition(":")[2] or 0)


def _table_chunk(collection, entries):
    """``(owner, [(path, Chunk)])`` for the table this export writes.

    A file that had a table keeps it even with nothing in it -- 8 shipped files
    carry an empty one, and dropping the chunk would be a change nobody asked
    for. A file that had none only gets one if a material now names a texture.
    """
    owner, path = _table_location(collection)
    if owner is None and entries:
        owner, path = _new_table_location(collection)
    if owner is None or not path:
        return None, []
    version = int(collection.get("rif_bmpnames_version", 0))
    return owner, [(path, rif.Chunk(b"BMPNAMES", bmpnames.encode(version, entries)))]


def _shape_chunk_from_mesh(obj, me, scale, y_down, stats, textures=None):
    """The mesh, plus everything that hangs off REBSHAPE, back into a chunk."""
    me.calc_loop_triangles()
    verts = [to_rif(v.co, scale, y_down) for v in me.vertices]

    # The index a slot writes comes from the export-wide table, so a material
    # renamed onto another texture moves every polygon wearing it at once -- and
    # the UV scale comes from the same place, since it is that texture's size.
    textures = textures or {}
    tex_of_slot = []
    for mat in me.materials:
        if mat is None:
            tex_of_slot.append((shp.TEXTURE_INDEX_MASK, (1.0, 1.0)))
        elif mat.name in textures:
            tex_of_slot.append(textures[mat.name])
        else:
            tex_of_slot.append((int(mat.get("rif_texture_index", shp.TEXTURE_INDEX_MASK)),
                                uv_scale(mat)))

    et = me.attributes.get("rif_engine_type")
    fl = me.attributes.get("rif_flags")
    hu = me.attributes.get("rif_has_uv")
    mg = me.attributes.get(MERGE_PAIR_ATTR)
    uv_layer = me.uv_layers.active

    # **Every face is triangulated on the way out**, whatever it is in Blender:
    # all 1,766,071 shipped polygons are triangles, and the engine builds its
    # render buffers from the unmerged list. A quad is a *merge pair* instead --
    # see the second pass below.
    #
    # Dropped before anything parallel is appended, so polys/uv_lists/merge stay
    # in step and `uv_index` stays contiguous. See shapes.welds_degenerate: a
    # triangle that loses a corner to the weld crashes the game while it builds
    # section adjacency, and the fault names neither the file nor the polygon.
    welded = shp.weld_map(verts)
    records, by_face = [], {}
    for tri in me.loop_triangles:
        if shp.welds_degenerate(tri.vertices, welded):
            stats["degenerate_faces"] += 1
            continue
        face = tri.polygon_index
        # `texels`, not `scale`: `scale` is this function's rif-units-to-metres
        # parameter, and shadowing it here would be a quiet way to write a mesh
        # at the size of its texture.
        texture_index, texels = (tex_of_slot[tri.material_index]
                                 if tri.material_index < len(tex_of_slot)
                                 else (shp.TEXTURE_INDEX_MASK, (1.0, 1.0)))
        engine_type = et.data[face].value if et is not None and face < len(et.data) else 3
        flags = fl.data[face].value if fl is not None and face < len(fl.data) else 0
        has_uv = bool(hu.data[face].value) if hu is not None and face < len(hu.data) else True
        pair_id = (mg.data[face].value if mg is not None and face < len(mg.data)
                   else shp.MERGE_NONE)

        flat = []
        if has_uv and uv_layer is not None:
            for loop in tri.loops:
                flat += uv_to_rif(uv_layer.data[loop].uv, texels)
        by_face.setdefault(face, []).append(len(records))
        records.append((tri, face, engine_type, flags, texture_index, pair_id, tuple(flat)))

    # **A quad is a merge pair.** The two triangles it tessellates into share an
    # edge, a material and a flags word by construction, which is exactly what
    # `MergePolygonsInChunkShape` fuses back into one nav section -- so pairing
    # them needs no stored id and survives any edit that keeps the face a quad.
    # An n-gon is not a pair: the engine's merged polygon is always four-sided.
    quad_pairs, quad_faces = {}, set()
    for face, group in by_face.items():
        if len(group) != 2 or len(me.polygons[face].vertices) != 4:
            continue
        # Whatever the id attribute says, this face is now the pairing -- so it
        # is out of the id pass either way, or a refused quad's two halves would
        # be re-paired through their (identical) inherited id.
        quad_faces.add(face)
        a, b = group
        if len({welded[v] for v in records[a][0].vertices}
               & {welded[v] for v in records[b][0].vertices}) != 2:
            continue        # the tessellation welded away the shared diagonal
        if shp.merges_by_texture(records[a][2]) and not (records[a][6] and records[b][6]):
            # `TexMergePolys` reads a (u,v) per vertex out of each partner's UV
            # record and writes a four-entry one back. With no record there,
            # both run off the end of it, and no shipped pair is in this shape.
            stats["merge_no_uvs"] += 1
            continue
        quad_pairs[a] = b
        quad_pairs[b] = a

    polys, uv_lists, uv_index_of = [], [], {}
    for n, (tri, _face, engine_type, flags, texture_index, _pair, flat) in enumerate(records):
        partner = quad_pairs.get(n)
        if not flat and partner is not None and partner < n:
            # An untextured pair shares one empty UV record, because `MergePolys`
            # compares the **whole `colour` dword** -- uv index included -- and
            # would otherwise refuse a pair it accepted as shipped. All 59,640
            # untextured pairs in the shipped levels share their record.
            uv_index = uv_index_of[partner]
        else:
            # One entry per triangle otherwise, which is what the shipped files
            # do -- and past 65,535 of them the index moves into bits 12-15, or
            # it would wrap and every face after the 65,536th would wear another
            # face's UVs. Four shipped shapes are that big.
            uv_index = len(uv_lists)
            uv_lists.append(flat)
        uv_index_of[n] = uv_index
        colour = shp.encode_colour(texture_index, uv_index)
        polys.append(shp.Poly(engine_type, len(polys), flags, colour, tuple(tri.vertices)))

    # Outside a quad the pairing is rebuilt from the stored ids rather than
    # replayed, so a partner dropped anywhere above becomes an honest -1 instead
    # of an index into a numbering that no longer exists.
    merge = [shp.MERGE_NONE] * len(polys)
    for a, b in quad_pairs.items():
        merge[a] = b
    ids = [shp.MERGE_NONE if r[1] in quad_faces else r[5] for r in records]
    wire, unpaired = shp.merge_wire_from_pairs(ids)
    for i, j in enumerate(wire):
        if merge[i] == shp.MERGE_NONE:
            merge[i] = j
    stats["merge_unpaired"] += unpaired
    stats["merge_quads"] += len(quad_pairs) // 2

    bodies = shp.build_bodies(verts, polys, uv_lists)
    children = [
        (0, rif.Chunk(b"SHPRAWVT", bodies[b"SHPRAWVT"])),
        (1, rif.Chunk(b"SHPVNORM", bodies[b"SHPVNORM"])),
        (2, rif.Chunk(b"SHPPNORM", bodies[b"SHPPNORM"])),
        (3, rif.Chunk(b"SHPPOLYS", bodies[b"SHPPOLYS"])),
        (4, rif.Chunk(b"SHPUVCRD", bodies[b"SHPUVCRD"])),
    ]
    # **The chunk is validated, not trusted.** `MergePolygonsInChunkShape` has no
    # bounds check of any kind, so a malformed table is an out-of-bounds heap
    # write during level load -- with a fault that names neither the file nor the
    # polygon. Omitting it is always safe (every reachable lookup guards on
    # absence, and the only cost is that coplanar pairs stay two triangles), so
    # anything this cannot prove correct is dropped rather than written.
    broken = shp.merge_problems(merge, len(polys))
    if broken:
        stats["merge_dropped"] += 1
        stats.setdefault("merge_reasons", []).append("%s: %s" % (me.name, broken[0]))
    else:
        children.append((6, rif.Chunk(b"SHPMRGDT",
                                      struct.pack("<%di" % len(merge), *merge))))
    if not me.get(NO_CENTRE_PROP):
        children.append((5, rif.Chunk(b"SHPCENTR", shp.centre_body(verts))))
    absorbed = [c for _, c in _emit_absorbed(me)]
    _sync_shape_header(absorbed, obj, verts, len(polys))
    children += [(7 + i, c) for i, c in enumerate(absorbed)]
    children.sort(key=lambda t: t[0])
    stats["shapes"] += 1
    return rif.Chunk(b"REBSHAPE", b"", [c for _, c in children])


def _sync_shape_header(children, obj, verts, num_polys):
    """Bring this shape's ``SHPHEAD1`` back in line with the mesh, in place.

    ``SHPHEAD1`` is not just an id: it carries ``num_verts``, ``num_polys``,
    ``radius`` and the bounding box, all of which AvP's loader reads straight
    into its shape record -- and Gunlok derives a role's collision extents from
    the bounds whenever the GLS gives ``radius``/``height`` as 0. Carried
    through unchanged, as every other absorbed chunk is, it goes stale the moment
    a vertex moves, and for a shape authored in Blender it would never have been
    right in the first place.

    So the derived half is regenerated and the authored half (``flags``,
    ``lock_user``, ``version_no``) is kept. The associated-object name list is
    regenerated too, from the object that owns this mesh: AvP rebuilds it from
    the same source on output, and it is what its by-name association falls back
    to when the id pairing finds nothing. A shape **no object claims** -- Elint
    MkII ships two -- has no such source, so its list is carried instead of being
    emptied.
    """
    head = next((c for c in children if c.id == b"SHPHEAD1"), None)
    if head is None:
        return
    names = heads.shphead_names(head.body)
    if obj.get("rif_id") == "RBOBJECT":
        name = rif_object_name(obj)
        names = [name] if name else []
    head.body = heads.sync_shphead(head.body, heads.shphead_file_id(head.body),
                                   verts, num_polys, names)


def _vtint_chunk(obj, me, stats):
    """The baked per-vertex lighting back into ``SHPVTINT``.

    Reads :data:`LIGHT_COLOR_ATTR` **by name**, never the active colour
    attribute: what a bake or a preview happens to have pointed Blender at is
    not a decision about the file. Gated on :data:`VTINT_HEADER_PROP`, so a
    paintable attribute minted for the preview does not add a chunk.

    Three outcomes, and the split is deliberate. No marker: no chunk, silently --
    the object never had one. Marker but no attribute: no chunk, **counted**, so
    export can say a chunk went away rather than losing it quietly. Marker and an
    attribute that cannot be read as one value per vertex: **raise**, because that
    is a mesh edit having outrun the lighting, and silently averaging or padding
    is the failure this whole design exists to prevent.

    Two of the four header words are not opaque, and both matter:

    - **``[0:2]`` is the light set's name**, and it is the *selector*. An object
      may carry one chunk per light set and AvP's loader takes the one matching
      the active set (``strncmp(svic->light_set_name, ::light_set_name, 8)``), so
      a chunk authored with a zeroed name is simply never found. Gunlok ships
      ``NORMALLT`` on all 4,668 chunks and on all 62 ``LTSETHDR``s, which is why
      a new one defaults to it rather than to zeros.
    - **``[3]`` is ``num_vertices``**, equal to the array length in 4,668 of
      4,668, and the engine trusts it -- it allocates and iterates that many
      times. Editing the mesh changes the array, so this is regenerated rather
      than carried, exactly as ``SHPHEAD1``'s counts are. Carrying it meant a
      mesh that gained or lost a vertex wrote a chunk whose declared count
      disagreed with its own data.
    """
    if not has_lighting(me):
        return None
    if me.color_attributes.get(LIGHT_COLOR_ATTR) is None:
        stats["lighting_dropped"] = stats.get("lighting_dropped", 0) + 1
        return None
    vals, why = lighting_values(me)
    if why is not None:
        raise ValueError(why)
    header = (list(me.get(VTINT_HEADER_PROP, VTINT_HEADER)) + [0, 0, 0, 0])[:4]
    header[3] = len(vals)
    body = struct.pack("<4i", *header) + struct.pack("<%di" % len(vals), *vals)
    return rif.Chunk(b"SHPVTINT", body)


def _object_chunk(obj, scale, y_down, stats):
    """An imported object back into its RBOBJECT chunk (its shape is emitted separately)."""
    data = list(obj.get("rif_objhead", []))
    if len(data) >= OH_ORIENT.stop and "rif_lod_base" not in obj:
        # The **world** transform, not the local one: after the rig pass a mesh
        # sits at identity under its hierarchy node and its placement lives on the
        # node, so reading obj.location would write zeros.
        #
        # A level-of-detail variant is the exception and keeps what it was loaded
        # with. The import pass moves it onto the bone of the part it replaces
        # (see `_build_armature`), which is where the engine draws it but not
        # where the file parked it, so recomputing would overwrite a field the
        # engine does not read with a placement it never had.
        world = obj.matrix_world
        data[OH_LOCATION] = list(to_rif(world.to_translation(), scale, y_down))
        data[OH_ORIENT] = [_float_to_bits(x)
                           for x in quat_to_rif(world.to_quaternion(), y_down)]
    children = [(0, rif.Chunk(b"OBJHEAD1", struct.pack("<%di" % len(data), *data)))]

    if obj.data is not None:
        vt = _vtint_chunk(obj, obj.data, stats)
        if vt is not None:
            children.append((1, vt))
    children += [(2 + i, c) for i, (_, c) in enumerate(_emit_absorbed(obj))]
    children.sort(key=lambda t: t[0])
    return rif.Chunk(b"RBOBJECT", b"", [c for _, c in children])


def _dumobj_chunk(obj, scale, y_down):
    """A dummy's DUMOBJDT, with its transform read back off the object.

    The name is re-appended with AvP's own padding rule, ``(strlen + 4) & ~3``,
    which is what makes the body ``0x34 + padded name``.
    """
    data = list(obj.get("rif_dumobjdt", []))
    body = bytearray(struct.pack("<%di" % len(data), *data))
    # A cull marker for a detail level (`L7#Pin Neck Left`) was moved onto the
    # bone of the part it applies to, so its placement is not read back -- the
    # same exception `_object_chunk` makes, for the same reason.
    if "rif_lod_base" not in obj:
        world = obj.matrix_world
        loc = to_rif(world.to_translation(), scale, y_down)
        quat = quat_to_rif(world.to_quaternion(), y_down)
        struct.pack_into("<3i", body, DUMOBJ_LOCATION, *loc)
        struct.pack_into("<4f", body, DUMOBJ_ORIENT, *quat)

    name = (obj.get("rif_name", "") or "").encode("latin-1")
    body += name + b"\0" * (((len(name) + 4) & ~3) - len(name))
    return rif.Chunk(b"DUMOBJDT", bytes(body))


def _light_chunk(obj, scale, y_down):
    light = obj.data
    colour = ((int(round(light.color[0] * 255)) & 0xFF) << 16 |
              (int(round(light.color[1] * 255)) & 0xFF) << 8 |
              (int(round(light.color[2] * 255)) & 0xFF))
    props = {
        "light_id": obj.get("rif_light_id", 0),
        "position": list(to_rif(obj.location, scale, y_down)),
        "orientation": matrix_to_rif(
            obj.rotation_quaternion if obj.rotation_mode == "QUATERNION"
            else obj.matrix_local.to_quaternion(), y_down),
        "brightness": int(round(light.energy * FIXED_ONE)),
        "spread": obj.get("rif_spread", LIGHT_SPREAD),
        "range": int(round((light.cutoff_distance if light.use_custom_distance else 0.0) / scale)),
        "colour": colour,
        "flags": obj.get("rif_flags", 3),
        "local_flags": obj.get("rif_local_flags", 1),
        "pad": list(obj.get("rif_pad", [0, 0])),
    }
    return rif.Chunk(b"STDLIGHT", schema.encode(b"STDLIGHT", props))





def _suspend_animation(collection):
    """Drop the scene to its recorded rest pose; returns what to put back.

    Export reads world transforms, and the armature carries an active Action, so
    the scene is posed. Animation evaluation *writes into* transform properties,
    so clearing the action does not put them back -- the rest pose has to be
    restored from what was recorded at import.
    """
    saved = []
    for obj in collection.objects:
        anim = obj.animation_data
        rest = obj.get("rif_rest")
        if anim is None and rest is None:
            continue
        if obj.get("rif_cut_role") == CUT_TRACK:
            # A cutscene path *is* its F-curves -- clearing the action here
            # would leave the exporter reading a single posed location and
            # silently writing a one-point path.
            continue
        pose = []
        if obj.type == "ARMATURE":
            # Clearing the action does not clear the pose -- evaluation writes into
            # each pose bone's own transform, so every bone has to go back to rest
            # or the mesh riding it exports from wherever it was posed.
            for pb in obj.pose.bones:
                pose.append((pb, pb.matrix_basis.copy()))
                pb.matrix_basis = mathutils.Matrix.Identity(4)
        saved.append((obj, anim.action if anim else None, obj.matrix_basis.copy(), pose))
        if anim is not None:
            anim.action = None
        if rest is not None:
            obj.matrix_basis = mathutils.Matrix(
                [list(rest[i * 4:i * 4 + 4]) for i in range(4)])
    bpy.context.view_layer.update()
    return saved


def _restore_animation(saved):
    for obj, action, basis, pose in saved:
        obj.matrix_basis = basis
        for pb, pb_basis in pose:
            pb.matrix_basis = pb_basis
        if obj.animation_data is not None:
            obj.animation_data.action = action

def rebuild_tree(collection, scale=None, y_down=None, fps=None):
    """A collection built by build_scene -> the chunk tree, reading only the scene."""
    scale = collection.get("rif_scale", DEFAULT_SCALE) if scale is None else scale
    y_down = bool(collection.get("rif_y_down", True)) if y_down is None else y_down
    fps = float(collection.get("rif_fps", 30.0)) if fps is None else fps
    stats = {"shapes": 0, "objects": 0, "lights": 0, "textures": 0, "new_textures": 0,
             "sounds": 0, "emitters": 0, "degenerate_faces": 0, "lighting_dropped": 0,
             "merge_unpaired": 0, "merge_dropped": 0, "merge_quads": 0,
             "merge_no_uvs": 0}
    # Read the rest pose, not whatever the animation system is posing right now.
    # This also runs the depsgraph, which export needs because matrix_world is
    # stale until it does.
    suspended = _suspend_animation(collection)
    try:
        return _rebuild(collection, scale, y_down, fps, stats)
    finally:
        _restore_animation(suspended)


def _rebuild(collection, scale, y_down, fps, stats):

    # Chunk nesting is not Blender parenting. An object the rig pass parented to a
    # hierarchy node is still a child of the file root on the wire, so it is
    # grouped under None regardless of where it sits in the outliner.
    by_parent = {}
    for obj in collection.objects:
        if "rif_id" not in obj:
            continue
        parent = None if obj.get("rif_rig_parented") else obj.parent
        by_parent.setdefault(parent, []).append(obj)

    entries, textures = _texture_table(collection)
    stats["textures"] = len(entries)
    stats["new_textures"] = len(entries) - len(collection.get("rif_bmpnames", ()))
    table_owner, table_extra = _table_chunk(collection, entries)
    cut_extra = cutscene_chunks(collection, scale, y_down)
    stats["cutscenes"] = sum(1 for _ in cutscene_roots(collection))

    def emit(obj):
        cid = obj["rif_id"].encode("ascii")
        if cid == b"RBOBJECT":
            stats["objects"] += 1
            return _object_chunk(obj, scale, y_down, stats)
        if cid == b"STDLIGHT":
            stats["lights"] += 1
            return _light_chunk(obj, scale, y_down)
        if cid in (b"REBSHAPE", b"SUBSHAPE") and obj.data is not None:
            # An unclaimed shape: its geometry lives in the mesh, not in fields.
            chunk = _shape_chunk_from_mesh(obj, obj.data, scale, y_down, stats, textures)
            chunk.id = cid
            return chunk

        extra = list(table_extra) if table_owner == obj.name else []
        extra += cut_extra.get(obj.name, ())
        children = list(_emit_from(obj.get("rif_absorbed"), extra))
        if cid == b"DUMMYOBJ":
            if "rif_dumobjdt" in obj:
                children.append((0, _dumobj_chunk(obj, scale, y_down)))
            # Emitted independently of the DUMOBJDT above: a dummy missing one is
            # refused before export gets here (`dummy_problems`), and silently
            # dropping its sound as well would hide which fault is which.
            emitter = _emitter_chunk(obj) if EMITTER_TEXT_PROP in obj else None
            if emitter is not None:
                children.append((int(obj.get(EMITTER_INDEX_PROP, 1)), emitter))
                stats["emitters"] += 1
        for kid in by_parent.get(obj, ()):
            children.append((kid.get("rif_index", 0), emit(kid)))
        children.sort(key=lambda t: t[0])
        return rif.Chunk(cid, b"", [c for _, c in children])

    # File-level leaves (RIFVERIN, HIDEGDIS, ...) live on the collection, and so
    # do the texture table and the sound table -- as tables, so they are rebuilt
    # rather than replayed.
    injected = list(table_extra) if table_owner == "" else []
    injected += _sound_chunks(collection)
    stats["sounds"] = len(injected) - (len(table_extra) if table_owner == "" else 0)
    injected += cut_extra.get("", ())
    top = _emit_from(collection.get("rif_absorbed"), injected)

    for obj in by_parent.get(None, ()):
        if obj.type == "ARMATURE":
            top.extend(_armature_chunks(obj, scale, y_down, fps))
            continue
        top.append((obj.get("rif_index", 0), emit(obj)))
        if obj.data is not None and obj.data.get("rif_id") == "REBSHAPE":
            top.append((obj.data.get("rif_index", 0),
                        _shape_chunk_from_mesh(obj, obj.data, scale, y_down, stats, textures)))

    top.sort(key=lambda t: t[0])
    return rif.Chunk(b"REBINFF2", b"", [c for _, c in top]), stats


# --------------------------------------------------------------------------
# cutscenes
# --------------------------------------------------------------------------
#
# A cutscene becomes real datablocks -- a Camera, an Empty it looks at, and an
# Empty per participant -- with the path in **location F-curves**. There is
# deliberately no second copy of the path anywhere: the keyframes *are* the
# CUTPOINT records, which is the same rule `rif_light` follows and the reason
# the packed-int mirror it replaced was deleted.
#
# The timing works because the engine's tick is 40 ms and these objects are
# keyed at 25 fps, so one frame is one tick exactly and every shipped duration
# is a whole number of frames (`cutscene.FPS`).
#
# These objects carry **`rif_cut_role`, never `rif_id`**, so `_rebuild`'s object
# loop skips them exactly as it skips a Speaker; their chunks are injected
# through `_emit_from`'s `extra` instead, the same seam the texture table uses.

#: Roles, in the order they nest.
CUT_SCENE, CUT_PARTICIPANT, CUT_TRACK = "cutscene", "participant", "track"


def _cut_children(obj, role):
    return [c for c in obj.children if c.get("rif_cut_role") == role]


def cutscene_roots(collection):
    """Every cutscene Empty in the collection."""
    return [o for o in collection.objects if o.get("rif_cut_role") == CUT_SCENE]


def _location_curves(obj):
    action = obj.animation_data.action if obj.animation_data else None
    if action is None:
        return []
    return [fc for fc in _iter_fcurves(action)
            if fc.data_path == "location" and fc.array_index < 3]


def _keyed_frames(obj):
    """Sorted integer frames at which this object's location is keyed."""
    frames = set()
    for fc in _location_curves(obj):
        frames.update(int(round(k.co[0])) for k in fc.keyframe_points)
    return sorted(frames)


def _location_at(obj, frame):
    out = list(obj.location)
    for fc in _location_curves(obj):
        out[fc.array_index] = fc.evaluate(frame)
    return out


def _key_location(obj, frames, points, scale, y_down):
    """Write one location keyframe per point, linear so nothing is implied.

    Blender is left to mint the Action and its slot -- doing it by hand differs
    between 4.x and the slotted 5.x actions, and `keyframe_insert` handles both.
    """
    for frame, pt in zip(frames, points):
        obj.location = to_blender(pt, scale, y_down)
        obj.keyframe_insert("location", frame=frame)
    action = obj.animation_data.action if obj.animation_data else None
    if action is None:
        return
    action.use_fake_user = True
    for fc in _iter_fcurves(action):
        for key in fc.keyframe_points:
            # Bezier would ease into every control point and read as the path;
            # the real curve is Catmull-Rom and only the preview can show it.
            key.interpolation = "LINEAR"


def _build_track(cs_obj, part_obj, track, part, collection, scale, y_down):
    """One CUTTRACK -> a Camera (the camera-position track) or an Empty."""
    if part.is_camera_position:
        data = bpy.data.cameras.new("%s_cam" % track.name)
        data.angle = math.radians(track.fov_degrees
                                  if track.fov_degrees is not None else 90.0)
        obj = bpy.data.objects.new(track.name or "camera", data)
    else:
        obj = bpy.data.objects.new(track.name or "track", None)
        obj.empty_display_type = "SPHERE"
        obj.empty_display_size = 0.3
    collection.objects.link(obj)
    obj.parent = part_obj

    obj["rif_cut_role"] = CUT_TRACK
    obj["rif_cut_index"] = track.index
    obj["rif_track_name"] = track.name
    obj["rif_track_name_fields"] = list(track.name_fields)
    obj["rif_start_quat"] = list(track.start_quat)
    obj["rif_end_quat"] = list(track.end_quat)
    obj["rif_has_start_quat"] = int(track.has_start_quat)
    obj["rif_has_end_quat"] = int(track.has_end_quat)
    obj["rif_point_unread"] = list(track.unread)
    obj["rif_point_spares"] = track.spares
    obj["rif_point_zeros"] = cutscene.zero_flags(track)
    obj["rif_final_ms"] = track.durations[-1] if track.points else 0
    obj["rif_fov_fields"] = list(track.fov_fields)
    obj["rif_has_fov"] = int(track.fov_degrees is not None)
    obj["rif_cut_indices"] = [track._name_index, track._point_index,
                              track._fov_index]
    obj["rif_cut_events"] = _pack_absorbed(
        [("CUTEVENT:%d" % i, props) for i, props in track.events])

    if track.points:
        _key_location(obj, cutscene.point_frames(track), track.positions,
                      scale, y_down)
    return obj


def _build_cutscene(collection, cs, owner, scale, y_down):
    root = bpy.data.objects.new("Cutscene %s" % cs.name, None)
    root.empty_display_type = "ARROWS"
    collection.objects.link(root)
    root["rif_cut_role"] = CUT_SCENE
    root["rif_cut_owner"] = owner
    root["rif_cut_prefix"] = cs.prefix
    root["rif_cut_index"] = cs.index
    root["rif_cut_name"] = cs.name
    root["rif_cut_position"] = list(cs.position)
    root["rif_cut_reserved"] = list(cs.reserved)
    root["rif_cut_data_index"] = cs._data_index

    for part in cs.participants:
        pobj = bpy.data.objects.new(part.rif_name or "participant", None)
        pobj.empty_display_type = "PLAIN_AXES"
        collection.objects.link(pobj)
        pobj.parent = root
        pobj["rif_cut_role"] = CUT_PARTICIPANT
        pobj["rif_cut_index"] = part.index
        pobj["rif_user_rif"] = part.rif_name
        pobj["rif_user_fields"] = [part.field_0, part.anim_id, part.user_id,
                                   part.field_3, part.is_camera, part.flags]
        pobj["rif_user_rest"] = list(part.fields_6_11)
        pobj["rif_user_hierarchy"] = part.hierarchy[0] if part.hierarchy else ""
        pobj["rif_user_hier_fields"] = list(part.hierarchy[1]) if part.hierarchy else []
        pobj["rif_has_hierarchy"] = int(part.hierarchy is not None)
        pobj["rif_user_sound"] = list(part.sound_props or ())
        pobj["rif_has_sound"] = int(part.sound_props is not None)
        pobj["rif_cut_indices"] = [part._data_index, part._hier_index,
                                   part._sound_index]
        for track in part.tracks:
            _build_track(root, pobj, track, part, collection, scale, y_down)
    return root


def build_cutscenes(collection, scale, y_down):
    """Promote every absorbed cutscene subtree to objects. Returns the count."""
    made = 0
    owners = [("", collection)] + [(o.name, o) for o in list(collection.objects)]
    for owner, datablock in owners:
        mine, rest = cutscene.split_absorbed(_absorbed_entries(datablock))
        if not mine:
            continue
        datablock["rif_absorbed"] = _pack_absorbed(rest)
        for cs in cutscene.parse(mine):
            _build_cutscene(collection, cs, owner, scale, y_down)
            made += 1
    return made


def _track_model(obj, scale, y_down):
    track = cutscene.Track(obj.get("rif_cut_index", 0))
    track.name = obj.get("rif_track_name", "")
    track.name_fields = list(obj.get("rif_track_name_fields", (0, 0)))
    track.start_quat = list(obj.get("rif_start_quat", (0.0, 0.0, 0.0, 1.0)))
    track.end_quat = list(obj.get("rif_end_quat", (0.0, 0.0, 0.0, 1.0)))
    track.has_start_quat = int(obj.get("rif_has_start_quat", 0))
    track.has_end_quat = int(obj.get("rif_has_end_quat", 0))
    track.unread = list(obj.get("rif_point_unread", (0, 0)))
    idx = list(obj.get("rif_cut_indices", (0, 0, 0)))
    track._name_index, track._point_index, track._fov_index = idx

    if obj.type == "CAMERA" and obj.data is not None:
        track.fov_degrees = math.degrees(obj.data.angle)
    elif obj.get("rif_has_fov"):
        track.fov_degrees = 90.0
    track.fov_fields = list(obj.get("rif_fov_fields", (0, 0)))

    frames = _keyed_frames(obj)
    if frames:
        positions = [to_rif(_location_at(obj, f), scale, y_down) for f in frames]
    else:
        # An unkeyed track is a single control point where the object sits.
        frames, positions = [0], [to_rif(obj.location, scale, y_down)]
    spares = list(obj.get("rif_point_spares", ()))[:len(positions)]
    zeros = list(obj.get("rif_point_zeros", ()))[:len(positions)]
    track.points = cutscene.pack_points(
        positions, frames, spares, int(obj.get("rif_final_ms", 0)), zeros)

    track.events = [(int(path.rpartition(":")[2]), props)
                    for path, props in _unpack_absorbed(obj.get("rif_cut_events"))]
    return track


def _cutscene_model(root, scale, y_down):
    cs = cutscene.Cutscene(int(root.get("rif_cut_index", 0)))
    cs.prefix = root.get("rif_cut_prefix", "")
    cs.name = root.get("rif_cut_name", "")
    cs.position = list(root.get("rif_cut_position", (0, 0, 0)))
    cs.reserved = list(root.get("rif_cut_reserved", (0, 0)))
    cs._data_index = int(root.get("rif_cut_data_index", 0))

    for pobj in sorted(_cut_children(root, CUT_PARTICIPANT),
                       key=lambda o: o.get("rif_cut_index", 0)):
        part = cutscene.Participant(pobj.get("rif_cut_index", 0))
        part.rif_name = pobj.get("rif_user_rif", "")
        f = list(pobj.get("rif_user_fields", (0, -1, 0, 0, 1, 0)))
        (part.field_0, part.anim_id, part.user_id,
         part.field_3, part.is_camera, part.flags) = f
        part.fields_6_11 = list(pobj.get("rif_user_rest", (0,) * 6))
        if pobj.get("rif_has_hierarchy"):
            part.hierarchy = (pobj.get("rif_user_hierarchy", ""),
                              list(pobj.get("rif_user_hier_fields", (0, 0, 0))))
        if pobj.get("rif_has_sound"):
            part.sound_props = list(pobj.get("rif_user_sound", (0,) * 6))
        idx = list(pobj.get("rif_cut_indices", (0, 0, 0)))
        part._data_index, part._hier_index, part._sound_index = idx

        for tobj in sorted(_cut_children(pobj, CUT_TRACK),
                           key=lambda o: o.get("rif_cut_index", 0)):
            part.tracks.append(_track_model(tobj, scale, y_down))
        cs.participants.append(part)
    return cs


def cutscene_chunks(collection, scale, y_down):
    """``{owner name: [(path, Chunk)]}`` for every cutscene in the collection."""
    by_owner = {}
    for root in cutscene_roots(collection):
        cs = _cutscene_model(root, scale, y_down)
        by_owner.setdefault(root.get("rif_cut_owner", ""), []).append(cs)
    return {owner: cutscene.emit(scenes) for owner, scenes in by_owner.items()}


def track_frames(obj):
    """The keyframes of one cutscene track object -- its control points."""
    return _keyed_frames(obj)


def cutscene_problems_for(root):
    """Pre-flight checks for one cutscene, in the spirit of the shape-id one."""
    out = []
    if not root.get("rif_cut_name", ""):
        out.append("No name, so PLAY CUTSCENE cannot reach it")
    try:
        cs = _cutscene_model(root, 1.0, True)
    except Exception as exc:  # noqa: BLE001
        return out + ["%s" % exc]

    if cs.camera_position_track() is None:
        out.append("No camera-position participant (is_camera == 0)")
    ends = any(cutscene.EVENT_CONTROL in list(props["kinds"])
               for part in cs.participants for track in part.tracks
               for _i, props in track.events)
    if not ends:
        # Running off the end of a path does not end a cutscene; the camera
        # simply parks and the player is left locked out.
        out.append("No control event: this would never end")
    cam = cs.camera_position_track()
    if cam is not None and cam.tracks and len(cam.tracks[0].points) < 2:
        out.append("The camera path has fewer than two control points")
    return out


def cutscene_problems(collection):
    """Every cutscene's problems, prefixed with which one."""
    out = []
    for root in cutscene_roots(collection):
        out.extend("%s: %s" % (root.name, why)
                   for why in cutscene_problems_for(root))
    return out


def add_cutscene_event(root, kind, command="", position=0.0):
    """Append an event to this cutscene's camera track. Returns the track object."""
    cam_part = next((p for p in _cut_children(root, CUT_PARTICIPANT)
                     if list(p.get("rif_user_fields", (0,) * 6))[4] == 0), None)
    if cam_part is None:
        return None
    tracks = _cut_children(cam_part, CUT_TRACK)
    if not tracks:
        return None
    tobj = sorted(tracks, key=lambda o: o.get("rif_cut_index", 0))[0]

    events = list(_unpack_absorbed(tobj.get("rif_cut_events")))
    index = 1 + max([int(p.rpartition(":")[2]) for p, _f in events], default=-1)
    if kind == "CONSOLE":
        props = cutscene.console_event(command, position)
    else:
        props = cutscene.end_event(position)
    events.append(("CUTEVENT:%d" % index, props))
    tobj["rif_cut_events"] = _pack_absorbed(events)
    return tobj


def preview_cutscene_path(root, per_segment=12):
    """A poly curve along the spline the engine will actually follow.

    Derived data, regenerated on demand and carrying no ``rif_`` id, so export
    skips it exactly as it skips a Speaker. It exists because Blender's own
    F-curve interpolation is *not* the engine's: the keys are Catmull-Rom
    control points, and on the shipped paths the spline departs from the
    straight line between them by a median 5.6% of segment length (max 47%).
    """
    collection = root.users_collection[0] if root.users_collection else None
    made = 0
    for part in _cut_children(root, CUT_PARTICIPANT):
        for tobj in _cut_children(part, CUT_TRACK):
            name = "%s_preview" % tobj.name
            old = bpy.data.objects.get(name)
            if old is not None:
                bpy.data.objects.remove(old, do_unlink=True)
            frames = _keyed_frames(tobj)
            if len(frames) < 2:
                continue
            pts = [_location_at(tobj, f) for f in frames]
            samples = cutscene.sample_path(pts, per_segment)
            curve = bpy.data.curves.new(name, "CURVE")
            curve.dimensions = "3D"
            spline = curve.splines.new("POLY")
            spline.points.add(len(samples) - 1)
            for i, p in enumerate(samples):
                spline.points[i].co = (p[0], p[1], p[2], 1.0)
            obj = bpy.data.objects.new(name, curve)
            if collection is not None:
                collection.objects.link(obj)
            obj.parent = tobj.parent
            made += 1
    return made


def add_cutscene(collection, name, camera=None, target=None):
    """Author a new cutscene, optionally adopting an existing camera pair."""
    owner_obj = _rebenvdt_object(collection)
    owner = owner_obj.name if owner_obj is not None else ""
    holder = owner_obj if owner_obj is not None else collection
    prefix = _cutscene_prefix(holder, owner_obj is not None)
    index = 1 + max([r.get("rif_cut_index", 0) for r in cutscene_roots(collection)
                     if r.get("rif_cut_owner", "") == owner], default=-1)

    cs = cutscene.new_cutscene(name, prefix, index)
    scale = collection.get("rif_scale", DEFAULT_SCALE)
    y_down = bool(collection.get("rif_y_down", True))
    root = _build_cutscene(collection, cs, owner, scale, y_down)

    tracks = [t for p in _cut_children(root, CUT_PARTICIPANT)
              for t in _cut_children(p, CUT_TRACK)]
    for existing, made in zip((camera, target), tracks):
        if existing is not None:
            made.location = existing.location
            if made.type == "CAMERA" and existing.type == "CAMERA":
                made.data.angle = existing.data.angle
    return root


def _cutscene_prefix(holder, is_object):
    """``REBENVDT:n/SPECLOBJ:m`` for a new cutscene on this owner.

    Reuses the file's existing `SPECLOBJ` when it has one -- every shipped file
    with cutscenes keeps them all under a single container.
    """
    for path, _props in _absorbed_entries(holder):
        parts = path.split("/")
        for i, seg in enumerate(parts):
            if seg.startswith("%s:" % cutscene.SPECLOBJ):
                return "/".join(parts[:i + 1])
    if is_object:
        # The REBENVDT object owns its children directly.
        return "%s:0" % cutscene.SPECLOBJ
    return "REBENVDT:0/%s:0" % cutscene.SPECLOBJ


# --------------------------------------------------------------------------
# authoring
# --------------------------------------------------------------------------
#
# Everything above assumes the scene came from `build_scene`, because the
# structural properties it reads -- `rif_id`, `rif_index`, `rif_objhead`,
# `rif_absorbed` -- only exist on datablocks the importer minted. A mesh added
# with Add > Mesh has none of them and `_rebuild` skips it in silence.
#
# So this section is the other way in: mint those properties directly. Two
# identities are all that a hand-made object needs beyond its geometry, and both
# are invisible in Blender's own UI, which is why they get accessors here rather
# than being left to whoever remembers the byte offsets:
#
# - **the name**, which lives at OBJHEAD1+0x3c and is what the engine resolves by
#   `strcmp` -- the map section's `name`, every `for "<rif object>"` spawn point,
#   and OBJHIERD's node binding. Renaming the *Blender* object does not touch it.
# - **the shape id**, OBJHEAD1+0x38 against SHPHEAD1+0x14, which is what pairs an
#   object with its geometry. Duplicating an object in Blender duplicates the id
#   too, and two objects claiming one shape is not representable on re-import.


def rif_collections():
    """Every collection in the file that stands for a RIF."""
    return [c for c in bpy.data.collections if c.get("rif_id") == "REBINFF2"]


def collection_for(obj):
    """The RIF collection ``obj`` belongs to, or None."""
    for coll in rif_collections():
        if obj.name in coll.objects:
            return coll
    return None


def _objhead(obj):
    return heads.from_words(obj.get("rif_objhead", []))


def _set_objhead(obj, body):
    obj["rif_objhead"] = heads.to_words(body)


def rif_object_name(obj):
    """The name this object answers to *inside the file*.

    For an ``RBOBJECT`` that is the trailing string in ``OBJHEAD1``; a
    ``DUMMYOBJ`` keeps its own ``rif_name``, because ``_dumobj_chunk`` re-appends
    the name from there on export.
    """
    if obj.get("rif_id") == "RBOBJECT":
        return heads.objhead_name(_objhead(obj))
    return obj.get("rif_name", "") or ""


def set_rif_object_name(obj, name):
    """Rename ``obj`` in the file, and follow it in the outliner.

    Blender may uniquify ``obj.name``; the RIF name is stored separately and is
    the authoritative one, so a suffixed outliner entry does not reach the file.
    """
    name = (name or "").strip()
    cid = obj.get("rif_id")
    if cid == "RBOBJECT":
        _set_objhead(obj, heads.set_objhead_name(_objhead(obj), name))
    elif cid != "DUMMYOBJ":
        return False
    obj["rif_name"] = name  # what the rig pass matches OBJHIERD against
    if name:
        obj.name = name
    return True


def _absorbed_entries(datablock):
    return list(_unpack_absorbed(datablock.get("rif_absorbed")))


def _rewrite_absorbed(datablock, cid, props):
    """Replace the fields of the first absorbed ``cid``, keeping its path."""
    entries = _absorbed_entries(datablock)
    target = cid.decode("latin-1")
    for i, (path, _fields) in enumerate(entries):
        if path.rpartition("/")[2].rpartition(":")[0] == target:
            entries[i] = (path, dict(props))
            datablock["rif_absorbed"] = _pack_absorbed(entries)
            return True
    return False


def _absorbed_body(datablock, cid):
    target = cid.decode("latin-1")
    for path, fields in _absorbed_entries(datablock):
        if path.rpartition("/")[2].rpartition(":")[0] == target:
            return heads.from_words(fields.get(schema.GENERIC_FIELD, []))
    return b""


def rif_shape_id(obj):
    """The id pairing this object with its geometry, or -1."""
    if obj.get("rif_id") != "RBOBJECT":
        return -1
    return heads.objhead_shape_id(_objhead(obj))


def set_rif_shape_id(obj, shape_id):
    """Set the id on **both** halves of the pair.

    Setting only one of them is the failure this exists to prevent: the object
    would name a shape that no ``SHPHEAD1`` claims, and re-import would give it
    no mesh at all.
    """
    if obj.get("rif_id") != "RBOBJECT":
        return False
    _set_objhead(obj, heads.set_objhead_shape_id(_objhead(obj), shape_id))
    me = obj.data
    if me is not None and me.get("rif_id") in ("REBSHAPE", "SUBSHAPE"):
        body = _absorbed_body(me, b"SHPHEAD1")
        if body:
            body = bytearray(body.ljust(heads.SHPHEAD1_HEADER, b"\0"))
            struct.pack_into("<i", body, heads.SHPHEAD1_FILE_ID, shape_id)
            _rewrite_absorbed(me, b"SHPHEAD1",
                              {schema.GENERIC_FIELD: heads.to_words(bytes(body))})
    return True


def shape_id_users(collection):
    """``{shape id: [object name, ...]}`` for every object that claims one."""
    out = {}
    for obj in collection.objects:
        if obj.get("rif_id") != "RBOBJECT" or obj.data is None:
            continue
        out.setdefault(rif_shape_id(obj), []).append(obj.name)
    return out


def next_shape_id(collection):
    used = [sid for sid in shape_id_users(collection) if sid >= 0]
    return max(used, default=0) + 1


def next_chunk_index(collection):
    """The next free ``rif_index`` among the file's top-level children.

    Objects, their meshes and the collection's own absorbed leaves all share one
    numbering, because ``_rebuild`` sorts every top-level child by it.
    """
    used = [_path_index(path) for path, _ in _absorbed_entries(collection)
            if "/" not in path]
    for obj in collection.objects:
        if "rif_id" not in obj:
            continue
        used.append(int(obj.get("rif_index", 0)))
        if obj.data is not None and obj.data.get("rif_id") in ("REBSHAPE", "SUBSHAPE"):
            used.append(int(obj.data.get("rif_index", 0)))
    return max(used, default=-1) + 1


#: The file-level chunks a new RIF starts with, in the order and at the sizes
#: ``SQUARE.RIF`` -- the smallest shipped file, 1,004 bytes -- carries them.
#: ``BMPNAMES`` is deliberately absent: ``_new_table_location`` appends it after
#: ``REBENVDT``'s children the first time a material names a texture, which is
#: also how the 36 shipped files without a table would gain one. ``BMNAMEXT`` is
#: 52 bytes in all 563 files regardless of how many textures the table holds, so
#: emitting it zeroed is not a guess about its contents scaling.
def _file_level_chunks(stem):
    endthead = bytearray(24)  # {flags, lock_user[16], version_no}
    return [
        ("RIFVERIN:0", {"version": 0}),
        ("REBENVDT:1/ENDTHEAD:0", {schema.GENERIC_FIELD: heads.to_words(endthead)}),
        ("REBENVDT:1/RIFFNAME:1", {"name": stem, "size": len(heads.pad_name(stem))}),
        ("REBENVDT:1/BMNAMEXT:2", {schema.GENERIC_FIELD: [0] * 13}),
        ("REBENVDT:1/BMNAMVER:3", {"version": 1}),
    ]


def new_collection(name, scale=DEFAULT_SCALE, y_down=True, fps=30.0):
    """An empty RIF, ready for :func:`adopt_object` and then export."""
    stem = os.path.splitext(os.path.basename(name))[0] or "untitled"
    collection = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(collection)
    collection["rif_id"] = "REBINFF2"
    collection["rif_scale"] = scale
    collection["rif_y_down"] = y_down
    collection["rif_fps"] = fps
    collection["rif_bmpnames"] = []
    collection["rif_bmpnames_version"] = 1
    collection["rif_absorbed"] = _pack_absorbed(_file_level_chunks(stem))
    return collection


def adopt_object(collection, obj, name=None):
    """Give ``obj`` the properties that make it part of ``collection``'s file.

    A mesh becomes an ``RBOBJECT`` with its own ``REBSHAPE``; an empty becomes an
    ``RBOBJECT`` with no geometry; a light becomes a ``STDLIGHT`` under the
    file's ``LIGHTSET``, which is created on first use. Returns a message on
    refusal, ``None`` on success.

    **The empty is the unattested case, and a mesh is the way a shipped level
    does it.** ``level01.RIF``'s spawn points are ordinary ``RBOBJECT``s carrying
    a 24-vertex marker mesh, and its ``camhund`` camera plane is a 4-vertex quad;
    across all 563 files the id pairing resolves every one of the 9,313 objects,
    so **no shipped object is geometry-less**. An empty is therefore a shape the
    format allows and the assets never take -- offered because it is the obvious
    thing to reach for, but a small mesh is the choice with evidence behind it.

    **This never makes a ``DUMMYOBJ``, and that is not an omission.**
    ``RifFilterObjectsByName`` -> ``RifCollectObjectChunks`` @ 0x005b0900 keeps a
    child only if its id is literally ``RBOBJECT``, so a ``for "<rif object>"``
    spawn point can never resolve to a dummy -- the two are disjoint namespaces.
    A dummy is the *locator* system (ambient sound, console and trigger
    positions, MP and enemy spawns, CTF points) and has its own entry point,
    :func:`adopt_dummy`.
    """
    if "rif_id" in obj:
        return "%s is already in a RIF collection" % obj.name
    if obj.type == "SPEAKER":
        return ("%s is a speaker; a positional sound is a DUMMYOBJ carrying a "
                "DUMOBJTX, so use Add as Locator or Add Ambient Emitter"
                % obj.name)
    if obj.type not in ("MESH", "EMPTY", "ARMATURE", "LIGHT"):
        return ("%s is a %s; only meshes, empties, armatures and lights can be "
                "RIF objects" % (obj.name, obj.type.lower()))

    if obj.type == "LIGHT":
        # A light is not an object in the file's object list at all -- it lives
        # under REBENVDT's LIGHTSET -- so it takes none of the shape/OBJHEAD1
        # bookkeeping below.
        return _adopt_light(collection, obj)

    if obj.type == "ARMATURE":
        # An armature is the file's OBJCHIER tree, not an object in it, so it
        # takes none of the per-object bookkeeping below -- `_rebuild` routes it
        # to `_armature_chunks` before ever reading `rif_objhead`. Without the
        # marker it is skipped in silence like any other unadopted datablock.
        obj["rif_id"] = "OBJCHIER"
        for bone in obj.data.bones:
            adopt_bone(obj, bone)
        _link_into(collection, obj)
        return None

    name = (name or obj.name).strip() or "object"
    index = next_chunk_index(collection)
    shape_id = -1

    me = obj.data if obj.type == "MESH" else None
    if me is not None:
        shape_id = next_shape_id(collection)
        me["rif_id"] = "REBSHAPE"
        me["rif_index"] = index + 1
        me["rif_absorbed"] = _pack_absorbed([
            ("SHPHEAD1:0", {schema.GENERIC_FIELD: heads.to_words(
                # The counts and bounds here are provisional: every export
                # regenerates them from the mesh (see `_sync_shape_header`), so
                # this only has to be a well-formed record with the right id.
                heads.make_shphead([name], shape_id, [], 0))}),
        ])
        _ensure_mesh_attributes(me)

    obj["rif_id"] = "RBOBJECT"
    obj["rif_index"] = index
    obj["rif_name"] = name
    _set_objhead(obj, heads.make_objhead(name, shape_id=shape_id))

    # OBINTDT holds the editor's note string and is present on all 9,313 shipped
    # objects, so a new one gets an empty note rather than no chunk.
    obj["rif_absorbed"] = _pack_absorbed([
        ("OBINTDT\0:0/OBJNOTES:0", {schema.GENERIC_FIELD: heads.to_words(b"\0\0\0\0")}),
    ])

    _link_into(collection, obj)
    if obj.rotation_mode != "QUATERNION":
        obj.rotation_mode = "QUATERNION"
    return None


# --------------------------------------------------------------------------
# Locators: a DUMMYOBJ, and the ambient emitter that is one
# --------------------------------------------------------------------------
#
# A dummy is **a name at a position, and that is the whole of it**. Every
# top-level one in a level rif becomes a 0x3c-byte `MapAuxObject`, and seven
# consumers then look records up by name -- always a linear scan, always
# case-insensitive. So the name is an API: the engine builds strings like
# `Goodie A2`, `baddie c`, `Flag_3` and `dumpresk` itself and scans for them.
#
# Every gate below is measured over all 6,847 shipped dummies, and one of them is
# a crash rather than a nuisance.

#: The `DUMOBJDT` a new dummy starts with: 13 int32, of which export overwrites
#: the location and the quaternion from the object's own transform. The
#: extents at +0x0c and +0x18 are read by **nothing** in the engine, so they are
#: left at zero rather than derived from a bounding box that has no consumer.
_NEW_DUMOBJDT = [0] * (DUMOBJDT_NAME // 4)

#: The identity quaternion, in the `(x, y, z, w)` float order `DUMOBJDT` stores.
#: Written as bits, because the record is an int32 array in Blender.
_IDENTITY_QUAT_BITS = list(struct.unpack("<4i", struct.pack("<4f", 0.0, 0.0, 0.0, 1.0)))


def adopt_dummy(collection, obj, name=None):
    """Make ``obj`` a top-level ``DUMMYOBJ`` -- a named locator.

    Returns a message on refusal, ``None`` on success. An **Empty** becomes a
    marker; a **Speaker** becomes an ambient emitter, which is the only thing a
    dummy can be besides a marker.

    The refusals are the measured gates, not caution:

    - **Top level only.** ``RifCollectDummyChunks`` @ 0x005b0ae0 walks the root's
      direct children and never recurses, and all 6,847 shipped dummies are at
      depth 0. A dummy parented under another RIF object would be written and
      never seen.
    - **A non-empty name.** ``DummyObjectDataChunk_CtorFromBuffer`` @ 0x005d2390
      stores ``NULL`` for an empty one -- not ``""`` -- and every name-matching
      consumer skips the record. 0 of 6,847 ship empty.

    The third gate cannot be refused here because it is about what export
    *writes*: a dummy with no ``DUMOBJDT`` is an unchecked null dereference
    during level load (``MapAuxObject_Ctor`` @ 0x005a971a), so one is created
    here and :func:`dummy_problems` refuses an export that lost it.
    """
    if "rif_id" in obj:
        return "%s is already in a RIF collection" % obj.name
    if obj.type not in ("EMPTY", "SPEAKER"):
        return ("%s is a %s; a locator has no geometry, so a dummy is an empty "
                "(a marker) or a speaker (an ambient sound)" % (obj.name, obj.type.lower()))
    if obj.parent is not None and "rif_id" in obj.parent:
        return ("%s is parented to %s; the engine only collects dummies from the "
                "file root, so a nested one is never seen" % (obj.name, obj.parent.name))

    name = (name or obj.name).strip()
    if not name:
        return ("a dummy needs a name -- an empty one is stored as NULL and every "
                "consumer skips the record")

    obj["rif_id"] = "DUMMYOBJ"
    obj["rif_index"] = next_chunk_index(collection)
    obj["rif_name"] = name
    obj.name = name
    obj["rif_dumobjdt"] = list(_NEW_DUMOBJDT[:DUMOBJ_ORIENT // 4]) + _IDENTITY_QUAT_BITS
    obj["rif_absorbed"] = _pack_absorbed([])
    if obj.rotation_mode != "QUATERNION":
        obj.rotation_mode = "QUATERNION"
    if obj.type == "EMPTY":
        obj.empty_display_type = "CUBE"
    _link_into(collection, obj)
    return None


def add_emitter(collection, wav_name, name=None, sound_dir=None):
    """A new ambient sound: a Speaker that *is* a top-level ``DUMMYOBJ``.

    Returns ``(object, None)`` or ``(None, message)``. The wav is a bare file
    name resolved by the sound system's own directory list -- ``Sound\\environ``
    is where all 44 shipped ones live -- and is never written back.
    """
    wav_name = (wav_name or "").strip()
    if not wav_name:
        return None, "an emitter needs a .wav name; that is what line 2 of the text stores"

    label = (name or os.path.splitext(wav_name)[0]).strip() or "emitter"
    spk = bpy.data.speakers.new(label)
    obj = bpy.data.objects.new(label, spk)
    bpy.context.scene.collection.objects.link(obj)
    why = adopt_dummy(collection, obj, label)
    if why is not None:
        bpy.data.objects.remove(obj, do_unlink=True)
        return None, why

    root_dir = sound_dir or (collection.get("rif_sound_dir", "") or None)
    _apply_emitter(obj, emitters.new_text(wav_name), root_dir)
    return obj, None


def dummy_problems(collection):
    """``(errors, warnings)`` for every ``DUMMYOBJ`` this export would write.

    The errors are the two shapes the game's own data never takes, so nothing in
    Gunlok is hardened against them:

    - **no ``DUMOBJDT``** -- ``DummyObjectChunk_GetDataChunk`` @ 0x005d21d0
      returns NULL and ``MapAuxObject_Ctor`` dereferences it unchecked, which is
      an access violation during level load;
    - **an empty name**, which is stored as ``NULL`` and skipped by every
      consumer.

    Duplicate names are a **warning**, not a refusal: 62 shipped files have them
    and they work. The catch is that resolution differs --
    ``ConsoleParsePosition`` takes the *first* match where the other six
    consumers take the *last* -- so a duplicate is a thing to know about rather
    than a thing to forbid.
    """
    errors, warnings = [], []
    by_name = {}
    for obj in (collection.objects if collection else ()):
        if obj.get("rif_id") != "DUMMYOBJ":
            continue
        name = (obj.get("rif_name", "") or "").strip()
        if "rif_dumobjdt" not in obj:
            errors.append("%s has no DUMOBJDT; a dummy without one crashes Gunlok "
                          "during level load" % obj.name)
        if not name:
            errors.append("%s has an empty name in the file; it is stored as NULL "
                          "and no consumer can find it" % obj.name)
        if obj.parent is not None and "rif_id" in obj.parent:
            errors.append("%s is parented to %s; only top-level dummies are "
                          "collected" % (obj.name, obj.parent.name))
        if name:
            by_name.setdefault(name.lower(), []).append(obj.name)

    for _name, objs in sorted(by_name.items()):
        if len(objs) > 1:
            warnings.append("%d dummies are named %r; the console takes the first "
                            "match and triggers take the last"
                            % (len(objs), objs[0]))

    for obj in emitter_objects(collection):
        for why in emitters.problems(emitter_text_from_speaker(obj)):
            warnings.append("%s: %s" % (obj.name, why))
        # ToMap unlinks and frees the record it turns into a sound, so an
        # emitter's name never reaches the locator system at all.
        if (obj.get("rif_name", "") or "").lower() in by_name and len(
                by_name.get((obj.get("rif_name", "") or "").lower(), ())) > 1:
            warnings.append("%s is an emitter, so its name never resolves -- ToMap "
                            "frees the record before any consumer sees it" % obj.name)
    return errors, warnings


def _link_into(collection, obj):
    if obj.name in collection.objects:
        return
    for other in list(obj.users_collection):
        other.objects.unlink(obj)
    collection.objects.link(obj)


# --------------------------------------------------------------------------
# Lights: the LIGHTSET a new one needs to live in
# --------------------------------------------------------------------------

def _next_child_index(datablock, children=()):
    """The next free index among a container's own children.

    Absorbed leaves and child objects share one numbering, because ``_rebuild``
    merges the two and sorts by it. The number is a sort key and nothing else --
    it orders the children and is then dropped, never written to the file.
    """
    used = [_path_index(path) for path, _ in _absorbed_entries(datablock)
            if "/" not in path]
    used += [int(o.get("rif_index", 0)) for o in children]
    return max(used, default=-1) + 1


def _children_of(collection, parent, chunk_id=None):
    objs = [o for o in collection.objects if o.parent == parent and "rif_id" in o]
    if chunk_id is not None:
        objs = [o for o in objs if o.get("rif_id") == chunk_id]
    return objs


def _rebenvdt_object(collection):
    for obj in collection.objects:
        if obj.get("rif_id") == "REBENVDT":
            return obj
    return None


def _promote_rebenvdt(collection):
    """The file-level ``REBENVDT`` as a real object, creating it if it is absorbed.

    A container becomes a Blender object exactly when something inside it needs
    one (:func:`_is_data_only`), so in the 502 shipped files with no lights
    ``REBENVDT`` is a path prefix on the collection rather than a datablock.
    Adding the first light is what stops that being true, and this performs the
    same lift the importer already does for the 62 files that have a
    ``LIGHTSET``. Returns ``None`` if there is no ``REBENVDT`` at all, which no
    shipped file and no :func:`new_collection` produces.
    """
    existing = _rebenvdt_object(collection)
    if existing is not None:
        return existing

    entries = _absorbed_entries(collection)
    prefix = next((path.partition("/")[0] for path, _ in entries
                   if path.partition("/")[0].rpartition(":")[0] == "REBENVDT"), None)
    if prefix is None:
        return None

    kept, moved = [], []
    for path, props in entries:
        head, sep, tail = path.partition("/")
        if head != prefix:
            kept.append((path, props))
        elif sep:
            moved.append((tail, props))
        # A bare "REBENVDT:n" entry is the empty-container marker; the object is
        # now that container, so it is dropped rather than carried.

    obj = bpy.data.objects.new("REBENVDT", None)
    obj["rif_id"] = "REBENVDT"
    obj["rif_index"] = _path_index(prefix)
    obj["rif_absorbed"] = _pack_absorbed(moved)

    # The texture table's recorded location has to move with it: the path is
    # relative to whichever datablock stores it, and that has just changed.
    table = collection.get("rif_bmpnames_path")
    if table and table.partition("/")[0] == prefix:
        obj["rif_bmpnames_path"] = table.partition("/")[2]
        del collection["rif_bmpnames_path"]

    collection["rif_absorbed"] = _pack_absorbed(kept)
    _link_into(collection, obj)
    return obj


def lightset_for(collection, create=True):
    """The collection's ``LIGHTSET`` object, building one if it has none."""
    for obj in collection.objects:
        if obj.get("rif_id") == "LIGHTSET":
            return obj
    if not create:
        return None
    parent = _promote_rebenvdt(collection)
    if parent is None:
        return None

    obj = bpy.data.objects.new("LIGHTSET", None)
    obj["rif_id"] = "LIGHTSET"
    obj["rif_index"] = _next_child_index(parent, _children_of(collection, parent))
    obj["rif_absorbed"] = _pack_absorbed([
        ("LTSETHDR:0", {schema.GENERIC_FIELD: heads.to_words(LIGHTSET_HEADER)}),
        ("AMBIENCE:1", {"ambience": LIGHTSET_AMBIENCE}),
    ])
    obj.parent = parent
    _link_into(collection, obj)
    return obj


def next_light_id(collection):
    """A file-unique ``light_id``.

    Unique within the file in all 38 that have lights, but **not** ``0..n-1`` --
    only 10 of the 38 are numbered that way -- so it is an id the editor handed
    out, not a position. Allocating past the highest reproduces that.
    """
    used = [int(o.get("rif_light_id", 0)) for o in collection.objects
            if o.get("rif_id") == "STDLIGHT"]
    return max(used, default=-1) + 1


def _place_ambience_last(collection, lightset):
    """Keep ``AMBIENCE`` after the lights, which is where all 62 shipped sets put it.

    Only the ordering is at stake -- AvP's loader reaches both leaves through
    ``lookup_single_child``, so nothing depends on it -- but matching the shipped
    layout costs one rewrite of a sort key that never reaches the file.
    """
    highest = max((int(o.get("rif_index", 0))
                   for o in _children_of(collection, lightset, "STDLIGHT")), default=0)
    entries = _absorbed_entries(lightset)
    for i, (path, props) in enumerate(entries):
        if path.rpartition(":")[0] == "AMBIENCE":
            entries[i] = ("AMBIENCE:%d" % (highest + 1), props)
            lightset["rif_absorbed"] = _pack_absorbed(entries)
            return


# --------------------------------------------------------------------------
# Baked vertex lighting: the paintable colour attribute IS the stored value
# --------------------------------------------------------------------------
#
# `SHPVTINT` is one packed 0xAARRGGBB per vertex on the wire, and the scene holds
# it as `rif_light`, a per-vertex BYTE_COLOR attribute -- the form the viewport
# draws, Vertex Paint edits, and a Cycles bake can target directly
# (Bake > Output > Target > Active Color Attribute). Export packs it back.
#
# **Read and write it through `color_srgb`, never `color`.** `color` converts
# sRGB bytes to and from linear floats, and measured over all 256 values per
# channel that loses a least-significant bit on 157 of them. `color_srgb` is the
# stored byte: 256/256 exact through a .blend save and reload, alpha included.
# That measurement is what makes storing the colour form lossless, and so what
# makes it safe to store *only* that -- there is no packed mirror to desync from,
# and no bake that lands in the scene but not in the file.
#
# It is **name-gated**, and that is the whole discipline. Export reads
# `rif_light` and nothing else -- never `color_attributes.active_color`, which
# is Blender-wide UI state that a bake, a preview, or any other feature can
# repoint. So an attribute that is not this one cannot become the file's
# lighting by accident; :func:`adopt_color_attribute` is the deliberate way to
# fold one in, and it is where the corner-domain averaging and the clamp live.
#
# Two states have to stay distinguishable and the attribute alone cannot do it:
# an object that ships no `SHPVTINT` at all (the engine defaults it to
# 0xFFFFFFFF) and one that ships an all-white chunk. :data:`VTINT_HEADER_PROP`
# is the marker, which it can be because it is already the chunk's own header --
# it exists exactly when the object carries a chunk, and it carries the light set
# name that selects it. That is what lets the preview mint a white `rif_light`
# to render through without silently adding a chunk to every unlit mesh.

#: The stored, paintable, exported attribute: one BYTE_COLOR per vertex.
LIGHT_COLOR_ATTR = "rif_light"

#: The mesh property holding the chunk's own 4-int header, and -- by its mere
#: presence -- "this object carries a SHPVTINT". Set on import for an object that
#: had one, and by :func:`enable_lighting` for one being lit for the first time.
VTINT_HEADER_PROP = "rif_vtint_header"

#: 0xFFFFFFFF as a signed int32: white, fully opaque. What a vertex with no
#: lighting yet gets, so a mesh that never had a SHPVTINT can still be baked to.
LIGHT_WHITE = -1


def _byte(x):
    """A 0..1 float to 0..255, clamped -- a bake can return values past 1.0."""
    if x <= 0.0:
        return 0
    if x >= 1.0:
        return 255
    return int(round(x * 255.0))


def unpack_light(v):
    """Packed 0xAARRGGBB -> ``(r, g, b, a)`` bytes."""
    u = v & 0xFFFFFFFF
    return ((u >> 16) & 0xFF, (u >> 8) & 0xFF, u & 0xFF, (u >> 24) & 0xFF)


def pack_light(r, g, b, a):
    """``(r, g, b, a)`` bytes -> the signed int32 Blender's INT attribute holds."""
    v = ((a & 0xFF) << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF)
    return v - (1 << 32) if v & 0x80000000 else v


def _set_active_color(me, attr):
    """Make ``attr`` the active colour attribute, which is what a bake targets."""
    for i, a in enumerate(me.color_attributes):
        if a.name == attr.name:
            for prop in ("active_color_index", "render_color_index"):
                with contextlib.suppress(AttributeError, TypeError):
                    setattr(me.color_attributes, prop, i)
            return


def white_light_attribute(me, name=LIGHT_COLOR_ATTR):
    """Ensure ``name`` is a per-vertex BYTE_COLOR, white where it is new, and active.

    Deliberately does **not** set :data:`VTINT_HEADER_PROP`, so this alone does
    not give the object a ``SHPVTINT``. That is what lets the preview render an
    unlit mesh the way the engine does -- white diffuse -- without adding a chunk
    to the file behind the author's back. :func:`enable_lighting` is the call
    that means "and export it".

    An existing attribute of the wrong shape is replaced rather than written
    into: its values do not describe this mesh. Returns ``(name, existed)``.
    """
    attr = me.color_attributes.get(name)
    existed = attr is not None
    if attr is not None and (attr.data_type != "BYTE_COLOR" or attr.domain != "POINT"
                             or len(attr.data) != len(me.vertices)):
        me.color_attributes.remove(attr)
        attr, existed = None, False
    if attr is None:
        attr = me.color_attributes.new(name=name, type="BYTE_COLOR", domain="POINT")
        for item in attr.data:
            item.color_srgb = (1.0, 1.0, 1.0, 1.0)
    _set_active_color(me, attr)
    return attr.name, existed


def has_lighting(me):
    """Does this mesh carry a ``SHPVTINT``? The marker, not the attribute."""
    return me is not None and VTINT_HEADER_PROP in me


def enable_lighting(me):
    """Start lighting this mesh: a white ``rif_light`` if it has none, plus the marker.

    The attribute is made **active**, which is what makes
    *Bake > Output > Target > Active Color Attribute* write straight into the
    stored value -- there is no packing step. Returns ``(name, had_lighting)``.
    """
    had = has_lighting(me)
    name, _existed = white_light_attribute(me)
    if not had:
        me[VTINT_HEADER_PROP] = list(VTINT_HEADER)
    return name, had


def adopt_color_attribute(me, name=None):
    """Fold a colour attribute into ``rif_light``, the one export reads.

    The escape hatch for a bake that landed somewhere else -- Blender's default
    new colour attribute is **corner**-domain, and a bake targets whatever is
    active. Reads ``name``, else the active attribute. A corner attribute is
    averaged per vertex, because the file has one value per vertex and no way to
    express a per-corner one, and values above 1.0 clamp.

    This is the only place either of those reductions happens. Export refuses a
    ``rif_light`` it cannot use rather than doing them silently, so a lossy
    conversion is always something the author asked for.

    Returns ``(source name, None)`` or ``(None, reason)``.
    """
    attr = me.color_attributes.get(name) if name else None
    if attr is None and not name:
        try:
            attr = me.color_attributes.active_color
        except AttributeError:
            attr = None
    if attr is None:
        return None, ("%s has no colour attribute %s"
                      % (me.name, "called %r" % name if name else "active"))

    # `data` is empty while the mesh is in Edit Mode, and goes short or stale when
    # the mesh is edited after the attribute was made. Either way the values do
    # not describe this mesh, so refuse rather than adopt whatever is there --
    # silently writing a partial SHPVTINT is worse than not writing one.
    n = len(me.vertices)
    want = n if attr.domain == "POINT" else len(me.loops)
    if me.is_editmode:
        return None, "%s is in Edit Mode; leave it before adopting lighting" % me.name
    if len(attr.data) != want:
        return None, ("%s: colour attribute %r holds %d value(s) for %d %s -- the mesh "
                      "changed since it was made; re-bake before adopting it"
                      % (me.name, attr.name, len(attr.data), want,
                         "vertices" if attr.domain == "POINT" else "corners"))

    # Already the stored attribute, and usable as it stands: nothing to convert,
    # so do not round-trip the bytes through a copy. Only the marker may be
    # missing, which is exactly the "I painted the preview's attribute and now I
    # want it exported" case.
    if attr.name == LIGHT_COLOR_ATTR and attr.domain == "POINT":
        me[VTINT_HEADER_PROP] = list(me.get(VTINT_HEADER_PROP, VTINT_HEADER))
        _set_active_color(me, attr)
        return attr.name, None

    if attr.domain == "POINT":
        colors = [tuple(attr.data[i].color_srgb) for i in range(n)]
    else:
        sums = [[0.0, 0.0, 0.0, 0.0] for _ in range(n)]
        counts = [0] * n
        for li, loop in enumerate(me.loops):
            c = attr.data[li].color_srgb
            acc = sums[loop.vertex_index]
            for k in range(4):
                acc[k] += c[k]
            counts[loop.vertex_index] += 1
        colors = [tuple(s[k] / counts[i] for k in range(4)) if counts[i]
                  else (1.0, 1.0, 1.0, 1.0)      # a loose vertex, unreachable by a bake
                  for i, s in enumerate(sums)]

    # Quantize through the same clamp export would have applied, so what the
    # viewport shows from here on is what the file gets.
    source = attr.name
    dst_name, _existed = white_light_attribute(me)
    dst = me.color_attributes[dst_name]
    for i, c in enumerate(colors[:len(dst.data)]):
        r, g, b, a = (_byte(x) for x in c)
        dst.data[i].color_srgb = (r / 255.0, g / 255.0, b / 255.0, a / 255.0)
    me[VTINT_HEADER_PROP] = list(me.get(VTINT_HEADER_PROP, VTINT_HEADER))
    return source, None


def lighting_refusal(me):
    """Why export would refuse this mesh's ``rif_light``, or None if it would not.

    Deliberately separate from :func:`lighting_values` and O(1): the panel calls
    it on every redraw, and packing a 13,000-vertex array to answer "is this
    usable?" would make selecting a level object crawl.
    """
    attr = me.color_attributes.get(LIGHT_COLOR_ATTR)
    if attr is None:
        return ("%s is marked as carrying vertex lighting but has no %r attribute"
                % (me.name, LIGHT_COLOR_ATTR))
    if attr.domain != "POINT" or attr.data_type != "BYTE_COLOR":
        return ("%s: %r is a %s %s attribute; vertex lighting is one BYTE_COLOR "
                "per vertex -- use \"Use Active Color Attribute\" to convert it"
                % (me.name, LIGHT_COLOR_ATTR, attr.domain.lower(), attr.data_type))
    if len(attr.data) != len(me.vertices):
        return ("%s: %r holds %d value(s) for %d vertices -- the mesh changed "
                "since it was made" % (me.name, LIGHT_COLOR_ATTR,
                                       len(attr.data), len(me.vertices)))
    return None


def lighting_values(me):
    """``rif_light`` as the packed int32 array the chunk holds, or a refusal.

    Returns ``(values, None)`` or ``(None, reason)``. The reason is export's, so
    it names the operator that fixes it rather than describing the shape.
    """
    why = lighting_refusal(me)
    if why is not None:
        return None, why
    attr = me.color_attributes[LIGHT_COLOR_ATTR]
    return [pack_light(*[_byte(c) for c in attr.data[i].color_srgb])
            for i in range(len(me.vertices))], None


def lighting_problems(collection):
    """Every mesh whose lighting export would refuse, as a list of reasons.

    Export raises rather than writing a file that disagrees with the scene, so
    this exists to say so *before* anything is written -- the same shape as the
    shared-shape-id check.
    """
    problems = []
    for obj in collection.objects if collection else ():
        if obj.get("rif_id") != "RBOBJECT" or obj.data is None:
            continue
        if not has_lighting(obj.data):
            continue
        if obj.data.color_attributes.get(LIGHT_COLOR_ATTR) is None:
            continue        # reported as a dropped chunk, not refused -- see _vtint_chunk
        why = lighting_refusal(obj.data)
        if why is not None:
            problems.append(why)
    return problems


def _adopt_light(collection, obj):
    """A Blender light -> a ``STDLIGHT`` under the file's ``LIGHTSET``."""
    lightset = lightset_for(collection)
    if lightset is None:
        return "%s has no REBENVDT to hold a light set" % collection.name

    lights = _children_of(collection, lightset, "STDLIGHT")
    obj["rif_id"] = "STDLIGHT"
    # Index 0 is LTSETHDR's, so lights start at 1.
    obj["rif_index"] = max((int(o.get("rif_index", 0)) for o in lights), default=0) + 1
    obj["rif_light_id"] = next_light_id(collection)
    obj["rif_spread"] = LIGHT_SPREAD
    obj["rif_flags"] = 3
    obj["rif_local_flags"] = 1
    obj["rif_pad"] = [0, 0]

    # Blender's defaults are in photometric watts and metres; the chunk wants a
    # 16.16 multiplier and rif units. `range` 0 means the light lights nothing,
    # which is what an untouched Blender light would otherwise export.
    obj.data.energy = LIGHT_ENERGY
    if not obj.data.use_custom_distance:
        obj.data.use_custom_distance = True
    obj.rotation_mode = "QUATERNION"

    obj.parent = lightset
    _link_into(collection, obj)
    _place_ambience_last(collection, lightset)
    return None


def adopt_bone(arm_obj, bone):
    """Give a hand-added bone the bookkeeping export reads.

    Only ``rif_index`` (its order among its siblings) is really needed -- nesting
    comes from ``bone.parent`` and the binding is regenerated -- so this is
    mostly about making a new bone indistinguishable from an imported one in the
    panel. A bone with no ``rif_path`` still exports; the path is carried only so
    an absorbed chunk knows where it came from.
    """
    if "rif_index" in bone:
        return False
    siblings = [b for b in arm_obj.data.bones if b.parent is bone.parent]
    bone["rif_index"] = max((int(b.get("rif_index", -1)) for b in siblings), default=-1) + 1
    bone["rif_bound"] = ""
    bone["rif_absorbed"] = _pack_absorbed([])
    bone["rif_hierd_index"] = 0
    return True


def adopt_action(arm_obj, action, name=None):
    """Mark an Action as one of this rig's sequences.

    Marked rather than auto-detected: an Action is not owned by the object it is
    assigned to, and sweeping up every Action with pose-bone curves would export
    whatever someone was experimenting with. ``_sequence_ids`` allocates the
    per-file sequence id on the next export.
    """
    if action.get("rif_id") == "OBANSEQC":
        return "%s is already a sequence" % action.name
    existing = _rif_actions(arm_obj)
    action["rif_id"] = "OBANSEQC"
    action["rif_sequence"] = (name or action.name).strip() or action.name
    action["rif_index"] = max((int(a.get("rif_index", -1)) for a in existing), default=-1) + 1
    action.use_fake_user = True
    for bone_name in _animated_bones(action):
        bone = arm_obj.data.bones.get(bone_name)
        if bone is not None:
            adopt_bone(arm_obj, bone)
    return None


def add_sound(collection, path, sound_file=None):
    """Add a table entry. Returns it.

    ``path`` is what the file stores, relative to the install's ``Sound`` folder
    and backslash-separated (``Robots\\GL_click08.wav``). The audio is optional
    and only ever loaded for audition -- export writes the path, never the wave.
    """
    entries = sound_table(collection)
    entry = snd.make_entry(next_sound_index(collection), path)
    entries.append(entry)
    set_sound_table(collection, entries)
    collection[SOUND_ACTIVE_PROP] = len(entries) - 1
    if sound_file and os.path.isfile(sound_file):
        # A sound that will not load is never worth failing the operator: the
        # path is what exports, and the audio is only for audition.
        with contextlib.suppress(Exception):
            sound = bpy.data.sounds.load(sound_file, check_existing=True)
            sound[SOUND_PATH_PROP] = path
            sound.use_fake_user = True
    return entry


def sound_events(action):
    """``[{bone, frame, index}]`` for one Action, as stored."""
    out = []
    for e in action.get("rif_sound_events", ()):
        out.append({"bone": e["bone"], "frame": float(e["frame"]), "index": int(e["index"])})
    return out


def set_sound_event(action, bone_name, frame, index):
    """Put (or clear, with ``index`` 0) a sound on one key of one bone.

    Matched with the same tolerance keyframe anchors use, so an event set here
    lands on an existing key rather than beside it.
    """
    events = [e for e in sound_events(action)
              if not (e["bone"] == bone_name and abs(e["frame"] - frame) < 1e-4)]
    if index:
        events.append({"bone": bone_name, "frame": float(frame), "index": int(index)})
    events.sort(key=lambda e: (e["bone"], e["frame"]))
    action["rif_sound_events"] = events
    return True


def sequence_setting(action, cid):
    """One optional setting's value, or None when the sequence has none."""
    prop = SEQUENCE_SETTINGS[cid]
    if prop not in action:
        return None
    value = action[prop]
    return list(value) if cid == b"OBASEQSP" else int(value)


def set_sequence_setting(action, cid, value):
    """Set a setting, or remove it with ``value=None``.

    Removing is a real edit -- most sequences carry none of the three -- so this
    deletes the property rather than storing a sentinel.

    Marks the setting **edited**, which is what licenses export to overwrite the
    file's own per-bone bodies with this one value. Until then they are left
    alone, because a few shipped sequences really do carry different values on
    different bones.
    """
    prop = SEQUENCE_SETTINGS[cid]
    if value is None:
        if prop in action:
            del action[prop]
    else:
        action[prop] = [int(v) for v in value] if cid == b"OBASEQSP" else int(value)
    edited = set(action.get("rif_seq_edited", ()))
    edited.add(cid.decode("ascii"))
    action["rif_seq_edited"] = sorted(edited)


def rif_armature(collection):
    """The collection's rig, or None. There is at most one."""
    for obj in collection.objects if collection else ():
        if obj.type == "ARMATURE":
            return obj
    return None


#: A polygon's engine type, flags and "did this have UVs" have no Blender
#: equivalent, so they ride as face attributes. Export defaults each of them
#: when the layer is missing; creating them up front is what makes them
#: *editable* in the spreadsheet rather than fixed at the default.
def _ensure_mesh_attributes(me):
    for attr_name, kind, default in (("rif_engine_type", "INT", 3),
                                     ("rif_flags", "INT", 0),
                                     (MERGE_PAIR_ATTR, "INT", shp.MERGE_NONE)):
        if me.attributes.get(attr_name) is None:
            attr = me.attributes.new(attr_name, kind, "FACE")
            for item in attr.data:
                item.value = default
    if me.attributes.get("rif_has_uv") is None:
        attr = me.attributes.new("rif_has_uv", "BOOLEAN", "FACE")
        has_uv = bool(me.uv_layers)
        for item in attr.data:
            item.value = has_uv


# --------------------------------------------------------------------------
# Navigation mesh preview
# --------------------------------------------------------------------------

NAV_WALKABLE_ATTR = "rif_walkable"
NAV_ISLAND_ATTR = "rif_nav_island"


def _face_attr(me, name, kind):
    attr = me.attributes.get(name)
    if attr is not None and (attr.domain != "FACE" or attr.data_type != kind
                             or len(attr.data) != len(me.polygons)):
        me.attributes.remove(attr)
        attr = None
    return attr or me.attributes.new(name, kind, "FACE")


def navmesh_preview(obj, scale=None, y_down=None):
    """Classify this mesh's faces the way Gunlok's nav builder does.

    Writes two FACE attributes -- ``rif_walkable`` (BOOLEAN) and
    ``rif_nav_island`` (INT, -1 where not walkable, else the connected region
    ordered largest-first) -- and leaves the walkable faces **selected**, which
    is the part that needs no viewport setup to see.

    Deliberately writes no colour attribute. Export is name-gated on
    ``rif_light`` so one left here could never *become* the lighting on its own,
    but :func:`adopt_color_attribute` reads whichever attribute is **active** --
    so leaving one would put a navmesh preview one click away from being adopted
    as this object's baked lighting. The INT island id is trivially turned into
    colour with Geometry Nodes when that is wanted.

    Returns ``(stats dict, reason)`` -- ``reason`` is a string when nothing was
    done.
    """
    me = getattr(obj, "data", None)
    if me is None or not hasattr(me, "polygons"):
        return None, "%s is not a mesh" % obj.name
    if me.is_editmode:
        return None, "%s is in Edit Mode; leave it before previewing" % obj.name
    if not len(me.polygons):
        return None, "%s has no faces" % obj.name

    collection = collection_for(obj)
    if scale is None:
        scale = float(collection.get("rif_scale", 1.0)) if collection else 1.0
    if y_down is None:
        y_down = bool(collection.get("rif_y_down", True)) if collection else True

    # The engine rotates the stored normal by the object's matrix before testing
    # it, so a mesh whose object still carries a rotation is classified in the
    # orientation it will actually load with -- not the one its vertices imply.
    rot = obj.matrix_world.to_3x3()
    fl = me.attributes.get("rif_flags")

    normals_rif = []
    flags = []
    for i, poly in enumerate(me.polygons):
        n = rot @ poly.normal
        if n.length > 0.0:
            n = n.normalized()
        normals_rif.append((n.x, -n.z, n.y) if y_down else (n.x, n.y, n.z))
        flags.append(fl.data[i].value if fl is not None and i < len(fl.data) else 0)

    walk_flags = [shp.is_walkable(normals_rif[i], flags[i]) for i in range(len(me.polygons))]

    # Weld by the *quantized* position, because that is the identity the engine's
    # vertex records have -- see shapes.weld_map.
    verts = [to_rif(v.co, scale, y_down) for v in me.vertices]
    welded = shp.weld_map(verts)
    faces = {i: tuple(welded[v] for v in p.vertices) for i, p in enumerate(me.polygons)}
    islands = shp.nav_islands(faces, lambda f: walk_flags[f])

    wa = _face_attr(me, NAV_WALKABLE_ATTR, "BOOLEAN")
    isl = _face_attr(me, NAV_ISLAND_ATTR, "INT")
    for i, poly in enumerate(me.polygons):
        wa.data[i].value = walk_flags[i]
        isl.data[i].value = islands.get(i, -1)
        poly.select = walk_flags[i]
    me.update()

    sizes = {}
    for isle in islands.values():
        sizes[isle] = sizes.get(isle, 0) + 1
    n_walk = sum(walk_flags)
    steep = sum(1 for i in range(len(me.polygons))
                if not walk_flags[i] and normals_rif[i][1] < 0
                and not (flags[i] & shp.NAV_BLOCKED_FLAG))
    return {
        "faces": len(me.polygons),
        "walkable": n_walk,
        "blocked_flag": sum(1 for f in flags if f & shp.NAV_BLOCKED_FLAG),
        "faces_down": sum(1 for i in range(len(me.polygons)) if normals_rif[i][1] >= 0),
        "too_steep": steep,
        "islands": len(sizes),
        "largest": max(sizes.values()) if sizes else 0,
    }, None


# --------------------------------------------------------------------------
# Gunlok preview: seeing the baked lighting the way the engine draws it
# --------------------------------------------------------------------------
#
# The engine's surface shading is two decisions long, and both were read out of
# the binary rather than guessed:
#
# - `BuildShapeVertexBuffers` @ 0x005ab300 fills a 0x24-byte vertex whose +0x18
#   is the D3DCOLOR diffuse, and assigns `diffuse = shpvtint->values[vert] |
#   0xFF000000` -- the stored word **undecoded**, with the file's top byte
#   ignored and alpha forced opaque. A mesh with no SHPVTINT gets 0xFFFFFFFF.
#   Gunlok does *not* do AvP's `sqrt((r^2+g^2+b^2)/3)` reduction.
# - `InitBuiltinMaterials` @ 0x005757b2 gives `Mat_Opaque` COLOROP =
#   D3DTOP_MODULATE with COLORARG1 = D3DTA_TEXTURE and COLORARG2 = D3DTA_DIFFUSE.
#
# So an opaque surface is exactly `texel * vertex_colour`, per channel, on the
# 8-bit numbers -- and because this is D3D8 fixed function, that multiply happens
# in **gamma space**, on the sRGB-encoded bytes, not in linear light.
#
# **That is the whole difficulty, and it is not a rounding detail.** The obvious
# material -- Color Attribute x Image Texture -> Emission -- multiplies in linear
# space, which would be equivalent only if sRGB were a pure power law. It is not:
# it has a linear toe below 0.04045, so `(a^g * b^g)^(1/g) = a*b` fails. Measured
# against the 8-bit reference, the naive graph is wrong by up to 7.43/255, and it
# is worst in the dark midrange where Gunlok's lighting actually lives (2.30 LSB
# at light 0x08, 6.49 at 0x20, 7.43 at 0x40, 0 only at 0xff). A pure 2.2 power
# law gives exactly 0.0 error, which is what identifies the piecewise toe as the
# culprit rather than any imprecision.
#
# The preview therefore multiplies the *stored* numbers and converts once at the
# end, which needs three things to line up:
#
# - the image texture is set to **Non-Color**, so its output is `byte / 255`
#   rather than the linearised value sRGB would give;
# - the colour attribute is *not* raw -- a BYTE_COLOR attribute is sRGB-encoded
#   storage and Blender linearises it on read -- so it goes through an exact
#   linear->sRGB encode to recover `byte / 255`;
# - the product is converted back with an exact sRGB->linear decode before
#   Emission, because the view transform encodes it again on the way to the
#   screen.
#
# Both conversions are the **exact piecewise** function, built out of Math nodes.
# Blender's `Gamma` node is a pure power law and would reintroduce the very error
# being avoided, and there is no colour-space conversion node in the shader node
# set at all (only in the compositor). Measured end to end by rendering, the
# graph below reproduces `texel * light / 255` with **0.00 LSB** error on every
# channel, where the naive linear graph misses by 2.5 to 5.6 on the same inputs.
#
# Emission is deliberate: nothing may re-light the result. That also makes the
# world irrelevant -- verified as identical pixels at world strength 0 and 20 --
# so this does not touch scene lighting.

#: Bumped when the transfer-function groups change shape, so a ``.blend`` made by
#: an older addon rebuilds them instead of silently keeping the older graph.
PREVIEW_GROUP_VERSION = 1

ENCODE_GROUP = "RIF Linear to sRGB"
DECODE_GROUP = "RIF sRGB to Linear"

#: On a preview material: the material it stands in for. An **ID pointer**, not a
#: name, so renaming or reordering cannot orphan it; verified to survive a
#: ``.blend`` save and reload.
PREVIEW_SOURCE_PROP = "rif_preview_source"

#: On the scene: the colour management and viewport state to put back.
PREVIEW_STATE_PROP = "rif_preview_restore"

_SRGB_LINEAR_CUT = 0.0031308      # linear side of the toe
_SRGB_GAMMA_CUT = 0.04045         # encoded side of the toe


def _math_node(tree, op, y=None, z=None, clamp=False):
    """A Math node with its constant inputs set **by index**.

    Never by socket name: a Math node calls all three "Value", and the
    multi-type nodes elsewhere in Blender's set repeat names across data types.
    """
    node = tree.nodes.new("ShaderNodeMath")
    node.operation = op
    node.use_clamp = clamp
    for i, v in ((1, y), (2, z)):
        if v is not None and i < len(node.inputs):
            node.inputs[i].default_value = v
    return node


def _srgb_transfer_group(name, decode):
    """A node group applying the exact piecewise sRGB transfer function.

    ``decode`` selects sRGB -> linear; otherwise linear -> sRGB. One chain per
    channel, because Math nodes are scalar and Blender has no per-channel
    conditional on a colour.

    Rebuilt when :data:`PREVIEW_GROUP_VERSION` moves, reused otherwise -- so the
    two groups exist once per ``.blend`` however many materials reference them.
    """
    group = bpy.data.node_groups.get(name)
    if group is not None:
        if group.get("rif_transfer_version") == PREVIEW_GROUP_VERSION:
            return group
        bpy.data.node_groups.remove(group)

    group = bpy.data.node_groups.new(name, "ShaderNodeTree")
    group["rif_transfer_version"] = PREVIEW_GROUP_VERSION
    group.interface.new_socket("Color", in_out="INPUT", socket_type="NodeSocketColor")
    group.interface.new_socket("Color", in_out="OUTPUT", socket_type="NodeSocketColor")

    gin = group.nodes.new("NodeGroupInput")
    gin.location = (-1000, 0)
    gout = group.nodes.new("NodeGroupOutput")
    gout.location = (700, 0)
    sep = group.nodes.new("ShaderNodeSeparateColor")
    sep.location = (-800, 0)
    comb = group.nodes.new("ShaderNodeCombineColor")
    comb.location = (500, 0)
    group.links.new(sep.inputs[0], gin.outputs[0])
    group.links.new(gout.inputs[0], comb.outputs[0])

    for ch in range(3):
        row = -300 * ch
        # A bake can hand back values outside 0..1, and POWER of a negative base
        # is not the continuation anybody wants, so each channel is clamped once
        # and every branch reads the clamped value.
        clamped = _math_node(group, "MULTIPLY", y=1.0, clamp=True)
        group.links.new(clamped.inputs[0], sep.outputs[ch])
        src = clamped.outputs[0]

        if decode:
            lo = _math_node(group, "MULTIPLY", y=1.0 / 12.92)
            shift = _math_node(group, "MULTIPLY_ADD", y=1.0 / 1.055, z=0.055 / 1.055)
            hi = _math_node(group, "POWER", y=2.4)
            cut = _SRGB_GAMMA_CUT
        else:
            lo = _math_node(group, "MULTIPLY", y=12.92)
            shift = _math_node(group, "POWER", y=1.0 / 2.4)
            hi = _math_node(group, "MULTIPLY_ADD", y=1.055, z=-0.055)
            cut = _SRGB_LINEAR_CUT
        group.links.new(shift.inputs[0], src)
        group.links.new(hi.inputs[0], shift.outputs[0])
        group.links.new(lo.inputs[0], src)

        below = _math_node(group, "LESS_THAN", y=cut)
        group.links.new(below.inputs[0], src)

        # out = (lo - hi) * below + hi. Written as arithmetic rather than with a
        # Mix node because Mix repeats "A"/"B" once per data type and its float
        # sockets are only reachable by index.
        delta = _math_node(group, "SUBTRACT")
        group.links.new(delta.inputs[0], lo.outputs[0])
        group.links.new(delta.inputs[1], hi.outputs[0])
        out = _math_node(group, "MULTIPLY_ADD")
        group.links.new(out.inputs[0], delta.outputs[0])
        group.links.new(out.inputs[1], below.outputs[0])
        group.links.new(out.inputs[2], hi.outputs[0])
        group.links.new(comb.inputs[ch], out.outputs[0])

        for i, node in enumerate((clamped, shift, hi, lo, below, delta, out)):
            node.location = (-620 + 150 * i, row - (120 if node is lo else 0))

    return group


def source_image(mat):
    """The image a material wears, or None -- the texture the engine samples."""
    if not (mat and mat.use_nodes and mat.node_tree):
        return None
    nodes = [n for n in mat.node_tree.nodes
             if n.type == "TEX_IMAGE" and n.image is not None]
    if not nodes:
        return None
    # Prefer the one actually feeding base colour, which is what `_wire_texture`
    # builds; fall back to the first, so a hand-edited material still previews.
    for node in nodes:
        for link in mat.node_tree.links:
            if link.from_node is node and link.to_socket.name in ("Base Color", "Color"):
                return node.image
    return nodes[0].image


def is_preview_material(mat):
    return mat is not None and PREVIEW_SOURCE_PROP in mat


def preview_material(source):
    """The Gunlok-shading stand-in for ``source``, made once and reused.

    One preview per source material, so objects sharing a material share its
    preview and the texture index each material stands for is left untouched.
    """
    if is_preview_material(source):
        return source
    for mat in bpy.data.materials:
        if is_preview_material(mat) and mat.get(PREVIEW_SOURCE_PROP) == source:
            _build_preview_tree(mat, source)
            return mat

    mat = bpy.data.materials.new(("GK Preview %s" % source.name)[:59])
    mat[PREVIEW_SOURCE_PROP] = source
    _build_preview_tree(mat, source)
    return mat


def _build_preview_tree(mat, source):
    """``texel * light`` in gamma space, decoded once, into Emission."""
    mat.use_nodes = True
    tree = mat.node_tree
    tree.nodes.clear()

    out = tree.nodes.new("ShaderNodeOutputMaterial")
    out.location = (700, 0)
    emission = tree.nodes.new("ShaderNodeEmission")
    emission.location = (400, -60)
    emission.inputs["Strength"].default_value = 1.0

    light = tree.nodes.new("ShaderNodeAttribute")
    light.location = (-820, -260)
    light.attribute_type = "GEOMETRY"
    light.attribute_name = LIGHT_COLOR_ATTR
    light.label = "Baked vertex lighting"

    encode = tree.nodes.new("ShaderNodeGroup")
    encode.location = (-600, -260)
    encode.node_tree = _srgb_transfer_group(ENCODE_GROUP, decode=False)
    encode.label = "back to the stored bytes"
    tree.links.new(encode.inputs[0], light.outputs["Color"])

    product = tree.nodes.new("ShaderNodeVectorMath")
    product.location = (-160, -60)
    product.operation = "MULTIPLY"
    product.label = "D3DTOP_MODULATE"

    image = source_image(source)
    if image is None:
        # The 0xfff untextured sentinel, or a material whose `.RIM` is not
        # installed. COLORARG1 has nothing to sample, so the diffuse stands
        # alone -- white texel times light, i.e. the light itself.
        product.inputs[0].default_value = (1.0, 1.0, 1.0)
        tex = None
    else:
        tex = tree.nodes.new("ShaderNodeTexImage")
        tex.location = (-820, 200)
        tex.image = image
        tex.interpolation = "Closest"
        # The multiply has to see the stored byte. **The image's colour space is
        # read, never written**: it is a property of a *shared* datablock, so
        # forcing it to Non-Color here would leave the author's own materials
        # rendering a linearised texture as though it were raw once the preview
        # is restored. An sRGB image (which is what `_load_png` produces, and so
        # every imported `.RIM`) is handed back through the same encode the
        # colour attribute uses; one already tagged Non-Color is already raw.
        texel = tex.outputs["Color"]
        if image.colorspace_settings.name != "Non-Color":
            to_bytes = tree.nodes.new("ShaderNodeGroup")
            to_bytes.location = (-600, 200)
            to_bytes.node_tree = _srgb_transfer_group(ENCODE_GROUP, decode=False)
            to_bytes.label = "back to the stored bytes"
            tree.links.new(to_bytes.inputs[0], texel)
            texel = to_bytes.outputs[0]
        tree.links.new(product.inputs[0], texel)
    tree.links.new(product.inputs[1], encode.outputs[0])

    decode = tree.nodes.new("ShaderNodeGroup")
    decode.location = (60, -60)
    decode.node_tree = _srgb_transfer_group(DECODE_GROUP, decode=True)
    decode.label = "to linear for the view transform"
    tree.links.new(decode.inputs[0], product.outputs[0])
    tree.links.new(emission.inputs["Color"], decode.outputs[0])

    if tex is not None and _has_alpha(image):
        # Cut-outs are alpha-tested in the engine, so a previewed fence or sprite
        # has to keep its holes rather than render as a solid quad.
        transparent = tree.nodes.new("ShaderNodeBsdfTransparent")
        transparent.location = (400, 160)
        mix = tree.nodes.new("ShaderNodeMixShader")
        mix.location = (560, 60)
        tree.links.new(mix.inputs[0], tex.outputs["Alpha"])
        tree.links.new(mix.inputs[1], transparent.outputs[0])
        tree.links.new(mix.inputs[2], emission.outputs["Emission"])
        tree.links.new(out.inputs["Surface"], mix.outputs[0])
        if hasattr(mat, "surface_render_method"):
            mat.surface_render_method = "DITHERED"
        elif hasattr(mat, "blend_method"):
            mat.blend_method = "CLIP"
    else:
        tree.links.new(out.inputs["Surface"], emission.outputs["Emission"])
    return mat


def is_shadow_object(obj):
    """Is this the silhouette caster rather than something the camera sees?

    ``level01_shadow.rif`` and its 24 siblings are a low-polygon stand-in used
    only to build shadow volumes (``level_loading_notes.md``), so in a preview
    they should cast and not appear. They are whole separate files, so the
    collection name is the reliable signal and the object name the fallback.
    """
    coll = collection_for(obj)
    names = [obj.name, rif_object_name(obj) or ""]
    if coll is not None:
        names.append(coll.name)
    return any("_shadow" in n.lower() for n in names)


def _preview_meshes(collection):
    return [o for o in collection.all_objects
            if o.type == "MESH" and o.data is not None]


def preview_setup(collections=None, shadow_casters=True):
    """Dress every RIF mesh so the viewport shows Gunlok's own shading.

    Returns ``(stats, reason)``. Reversible: each preview material records the
    material it replaced, and the scene records the colour management it changed.
    """
    targets = list(collections) if collections is not None else rif_collections()
    if not targets:
        return None, "No RIF collection in the scene"

    stats = {"objects": 0, "materials": 0, "lit": 0, "shadow": 0, "untextured": 0}
    seen = {}
    for collection in targets:
        for obj in _preview_meshes(collection):
            if shadow_casters and is_shadow_object(obj):
                # Ray visibility, which is exactly the role the engine gives it.
                obj.visible_camera = False
                obj.visible_shadow = True
                stats["shadow"] += 1
                continue

            # A mesh with no lighting still has to render: the engine gives an
            # object with no SHPVTINT a white diffuse, and a ShaderNodeAttribute
            # naming an attribute that does not exist reads as black. So mint a
            # white one -- and *only* a white one, never the marker, or looking
            # at an unlit mesh would give it a chunk. An attribute that is
            # already there is left exactly as it is: it is the stored lighting,
            # which is precisely the work this preview exists to show.
            if obj.data.color_attributes.get(LIGHT_COLOR_ATTR) is None:
                white_light_attribute(obj.data)
                stats["lit"] += 1

            for slot in obj.material_slots:
                source = slot.material
                if source is None or is_preview_material(source):
                    continue
                preview = seen.get(source.name)
                if preview is None:
                    preview = preview_material(source)
                    seen[source.name] = preview
                    stats["materials"] += 1
                    if source_image(source) is None:
                        stats["untextured"] += 1
                slot.material = preview
            stats["objects"] += 1

    remember_and_set_display()
    return stats, None


def _viewport_spaces():
    """Every 3D viewport's active space, keyed so restore can find it again.

    Keyed by screen *and* area index, not by screen: a workspace may hold two
    viewports set to different shading, and collapsing them onto the screen name
    would restore one of them to the other's mode.
    """
    for screen in bpy.data.screens:
        for i, area in enumerate(screen.areas):
            if area.type != "VIEW_3D":
                continue
            space = area.spaces.active
            if space is not None and space.type == "VIEW_3D":
                yield "%s/%d" % (screen.name, i), area, space


def remember_and_set_display(scene=None):
    """Colour management and viewport shading, recorded so restore is exact.

    ``Standard`` is not a preference: Filmic and AgX are tone mappings, so they
    would both shift the midtones this is trying to reproduce and roll off the
    highlights rather than clamping at 1.0 the way the game does. Exposure and
    gamma are reset for the same reason -- either one silently rescales the
    result.
    """
    scene = scene or bpy.context.scene
    view = scene.view_settings
    if PREVIEW_STATE_PROP not in scene:
        scene[PREVIEW_STATE_PROP] = json.dumps({
            "view_transform": view.view_transform,
            "look": view.look,
            "exposure": view.exposure,
            "gamma": view.gamma,
            "shading": [[name, space.shading.type]
                        for name, _area, space in _viewport_spaces()],
        })

    view.view_transform = "Standard"
    with contextlib.suppress(TypeError):
        view.look = "None"
    view.exposure = 0.0
    view.gamma = 1.0
    # Material Preview, because Solid mode cannot do this at all:
    # `View3DShading.color_type` is a single choice (VERTEX **or** TEXTURE) and
    # the struct carries no blend or multiply option to combine them.
    for _name, _area, space in _viewport_spaces():
        space.shading.type = "MATERIAL"


def preview_restore():
    """Put the authored materials and the colour management back."""
    stats = {"objects": 0, "materials": 0, "removed": 0}
    for obj in bpy.data.objects:
        touched = False
        for slot in obj.material_slots:
            source = slot.material.get(PREVIEW_SOURCE_PROP) if slot.material else None
            if source is None:
                continue
            slot.material = source
            stats["materials"] += 1
            touched = True
        stats["objects"] += touched

    # A preview nothing references any more is rebuilt on demand, so leaving it
    # would only accumulate. One still in use somewhere is left alone.
    for mat in list(bpy.data.materials):
        if is_preview_material(mat) and mat.users == 0:
            bpy.data.materials.remove(mat)
            stats["removed"] += 1

    scene = bpy.context.scene
    saved = scene.get(PREVIEW_STATE_PROP)
    if saved:
        state = json.loads(saved)
        view = scene.view_settings
        with contextlib.suppress(TypeError):
            view.view_transform = state.get("view_transform", "Standard")
        with contextlib.suppress(TypeError):
            view.look = state.get("look", "None")
        view.exposure = state.get("exposure", 0.0)
        view.gamma = state.get("gamma", 1.0)
        want = dict(state.get("shading", []))
        for name, _area, space in _viewport_spaces():
            if name in want:
                with contextlib.suppress(TypeError):
                    space.shading.type = want[name]
        del scene[PREVIEW_STATE_PROP]
    return stats, None


def preview_is_active():
    """Is anything currently wearing a preview material?

    Asked of the **materials**, not of every object's slots: this is called from
    a panel's ``draw`` and an operator's ``poll``, both of which run on every
    redraw, and a level is thousands of objects against a few dozen materials. A
    preview material with users is one that something is wearing.
    """
    return any(is_preview_material(mat) and mat.users for mat in bpy.data.materials)


def preview_shade(texel, light):
    """The engine's own arithmetic for one texel: ``texel * light / 255``.

    The reference the material is measured against, kept beside it so the test
    and the description cannot drift apart.
    """
    return tuple(t * light[i] / 255.0 for i, t in enumerate(texel))
