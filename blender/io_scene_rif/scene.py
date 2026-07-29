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
import math
import os
import re
import struct
import tempfile

import bpy
import mathutils

from . import bmpnames
from . import rif
from . import rim
from . import schema
from . import shapes as shp

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

#: Per-polygon and per-vertex data that is *authored*, not derived, so it becomes a
#: Blender attribute and rides along with the mesh through an edit:
#:
#: - ``SHPMRGDT`` is exactly one int32 per polygon in all 9,357 shipped shapes
#:   (AvP's ``{int *merge_data; int num_polys}``) -> a face attribute.
#: - ``SHPVTINT`` is per-vertex lighting and a child of ``RBOBJECT``, never of a
#:   shape, in all 4,668 cases -> a vertex attribute on the object's mesh.
ATTRIBUTE_CHUNKS = {
    b"SHPMRGDT": ("rif_merge_group", "INT", "FACE"),
    b"SHPVTINT": ("rif_vertex_intensity", "INT", "POINT"),
}

#: Preprocessed render data with no known generator. **Discarded on load and
#: omitted on export**, which is legal rather than lossy: 681 of the 9,357 shipped
#: shapes carry no ``SHPPCINF`` at all, and every AvP code path guards on its
#: lookup returning null. Byte-exact round-tripping is not a goal; semantic
#: equivalence is.
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
_STRUCTURAL = frozenset(("rif_id", "rif_index", "rif_absorbed", "rif_scale", "rif_y_down",
                         "rif_shape_index", "rif_texture_index", "rif_pair",
                         "rif_name", "rif_bound", "rif_objhead", "rif_dumobjdt",
                         "rif_rest", "rif_rig_parented", "rif_lod_base",
                         "rif_anim_index",
                         "rif_anim_absorbed", "rif_vtint_header", "rif_fps",
                         "rif_bmp_name", "rif_bmpnames", "rif_bmpnames_path",
                         "rif_bmpnames_version", "rif_uv_scale"))


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
# chunk tree -> scene
# --------------------------------------------------------------------------

#: An object names its shape by id, not by position.
OBJHEAD1_SHAPE_ID = 0x38   # AvP's Object_Header_Chunk::shape_id_no
SHPHEAD1_FILE_ID = 0x14    # AvP's Shape_Header_Chunk::file_id_num


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
    for key in ("light_id", "field_0x38", "flags", "field_0x48", "field_0x4c"):
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
#: and binary junk in 2,783, across 59 distinct values, so that is a separate
#: (often uninitialised) tag. Reading it as the name hid every real name --
#: ``Head``, ``Waist``, ``Ribs``, ``Chest``, ``Foot Right``, ``Index Right A``.
OBJHEAD1_NAME = 0x3C
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


def _trailing_name(body, offset):
    if len(body) <= offset:
        return ""
    text = body[offset:].split(b"\0")[0]
    if not text or not all(32 <= b < 127 for b in text):
        return ""
    return text.decode("ascii")


def _objhead_name(body):
    return _trailing_name(body, OBJHEAD1_NAME)


#: Leaf chunks that get their own object instead of folding into the parent.
LEAF_AS_OBJECT = frozenset((b"STDLIGHT",))


#: ``OBASEQHD`` is ``{int32 num_frames, sequence_number, sub_sequence_number,
#: num_extra_data, int32[num_extra_data], name}`` -- AvP's
#: ``Object_Animation_Sequence_Header_Chunk::fill_data_block``. ``num_extra_data``
#: is 0 in all 29,550 shipped sequences, so the name always starts at 0x10.
#:
#: An earlier revision put it at 0x12, reading the leading ``Dz`` as a constant
#: field because it is present in every sequence. It is not a field: the size
#: arithmetic settles it, since ``len(body) == 0x10 + ((len(name) + 4) & ~3)``
#: holds for all 29,550 only when the name starts at 0x10. ``Dz`` is a naming
#: convention in the source assets, so the sequences really are called
#: ``DzSeq_Stand``, ``DzSeq_Walk``, and so on.
OBASEQHD_NAME = 0x10


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
        bone["rif_absorbed"] = _pack_absorbed(_absorb(chunk, set()))
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

            # A sequence with no frames still has to be recorded: 973 of the
            # game's 29,550 are empty, and skipping them here drops the chunk.
            curves = {}
            if frames:
                base = 'pose.bones["%s"].' % bpy.utils.escape_identifier(bone_name)
                curves = {p: [new_curve(data_path=base + p, index=i) for i in range(n)]
                          for p, n in (("location", 3), ("rotation_quaternion", 4))}
            meta = []
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
                meta.append({"time": props["time"], "frame_index": props["frame_index"],
                             "flags": _to_signed32(props["flags"]),
                             "field_0x28": props["field_0x28"]})
            for group in curves.values():
                for fc in group:
                    fc.update()
            frames_meta.append({"bone": bone_name, "path": path, "order": order,
                                "duration_s": duration_s, "frames": meta,
                                "absorbed": _pack_absorbed(_absorb(seq, {b"OBASEQFR"}))})

        action["rif_bones"] = frames_meta
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


def _rest_relative(bone):
    """A bone's rest transform relative to its parent, which is what a pose basis
    is measured against: ``pose_local = rest_relative @ matrix_basis``."""
    if bone.parent is None:
        return bone.matrix_local.copy()
    return bone.parent.matrix_local.inverted() @ bone.matrix_local


def _armature_chunks(arm_obj, scale, y_down, fps):
    """The armature back into ``[(index, OBJCHIER chunk)]`` for the file root."""
    arm = arm_obj.data
    by_path = {}
    for bone in arm.bones:
        path = bone.get("rif_path")
        if path:
            by_path[path] = bone

    # Actions, grouped by the bone they animate, so a bone can rebuild its own
    # OBANSEQS from the Actions that mention it.
    per_bone = {}
    for action in bpy.data.actions:
        if action.get("rif_id") != "OBANSEQC":
            continue
        for entry in action.get("rif_bones", ()):
            per_bone.setdefault(entry["bone"], []).append((action, entry))

    made = {}

    def chunk_for(path):
        if path in made:
            return made[path]
        bone = by_path[path]
        children = list(_emit_from(bone.get("rif_absorbed")))

        seqs = _sequences_for(bone, per_bone.get(bone.name, ()), arm_obj, scale, y_down, fps)
        if seqs is not None:
            children.append((bone.get("rif_anim_index", len(children)), seqs))

        chunk = rif.Chunk(b"OBJCHIER", b"", [])
        made[path] = (chunk, children)
        return made[path]

    for path in sorted(by_path, key=lambda p: p.count("/")):
        chunk_for(path)

    roots = []
    for path, (chunk, _kids) in made.items():
        head, _, last = path.rpartition("/")
        index = int(last.rpartition(":")[2])
        if head and head in made:
            made[head][1].append((by_path[path].get("rif_index", 0), chunk))
        else:
            roots.append((index, chunk))

    for chunk, children in made.values():
        children.sort(key=lambda t: t[0])
        chunk.children = [c for _, c in children]
    return roots


def _sequences_for(bone, entries, arm_obj, scale, y_down, fps):
    """One bone's Actions back into its OBANSEQS chunk."""
    if not entries:
        return None
    out = []
    for action, entry in entries:
        out.append((entry["order"], _bone_sequence_chunk(action, entry, arm_obj,
                                                         scale, y_down, fps)))
    out.sort(key=lambda t: t[0])
    children = out + list(_emit_from(bone.get("rif_anim_absorbed")))
    children.sort(key=lambda t: t[0])
    return rif.Chunk(b"OBANSEQS", b"", [c for _, c in children])


def _bone_sequence_chunk(action, entry, arm_obj, scale, y_down, fps):
    """One (Action, bone) pair back into an OBANSEQC chunk."""
    bone_name = entry["bone"]
    duration_s = float(entry.get("duration_s", 1.0)) or 1.0
    rest_rel = _rest_relative(arm_obj.data.bones[bone_name])

    base = 'pose.bones["%s"].' % bpy.utils.escape_identifier(bone_name)
    curves = {}
    for fc in _iter_fcurves(action):
        if fc.data_path.startswith(base):
            curves.setdefault(fc.data_path[len(base):], {})[fc.array_index] = fc

    frames = []
    for n, meta in enumerate(entry.get("frames", ())):
        t = float(meta["time"]) / FIXED_ONE * duration_s * fps
        loc = [curves.get("location", {}).get(i).evaluate(t)
               if curves.get("location", {}).get(i) else 0.0 for i in range(3)]
        quat = [curves.get("rotation_quaternion", {}).get(i).evaluate(t)
                if curves.get("rotation_quaternion", {}).get(i) else (1.0 if i == 0 else 0.0)
                for i in range(4)]
        basis = (mathutils.Matrix.Translation(loc)
                 @ mathutils.Quaternion(quat).to_matrix().to_4x4())
        local = rest_rel @ basis
        props = {
            "rotation": list(quat_to_rif(local.to_quaternion(), y_down)),
            "position": list(to_rif(local.to_translation(), scale, y_down)),
            "time": int(meta["time"]),
            "frame_index": int(meta["frame_index"]),
            "flags": _to_signed32(int(meta["flags"])),
            "field_0x28": int(meta["field_0x28"]),
        }
        frames.append((n, rif.Chunk(b"OBASEQFR", schema.encode(b"OBASEQFR", props))))

    children = frames + list(_emit_from(entry.get("absorbed")))
    children.sort(key=lambda t: t[0])
    return rif.Chunk(b"OBANSEQC", b"", [c for _, c in children])












def _iter_fcurves(action):
    slots = getattr(action, "slots", None)
    if slots is None:  # pragma: no cover - older Blender
        yield from action.fcurves
        return
    for layer in action.layers:
        for strip in layer.strips:
            for bag in strip.channelbags:
                yield from bag.fcurves


_CTX = {"scale": DEFAULT_SCALE, "y_down": True, "fps": 30.0,
        "textures": {}, "materials": {}, "images": {}, "texture_index": None,
        "table_chunk": None,
        "missing_textures": set(), "undecodable_textures": set()}


def _link(obj, collection, parent):
    collection.objects.link(obj)
    if parent is not None:
        obj.parent = parent


def _mesh_for_shape(shape_chunk, name):
    """REBSHAPE -> a Blender mesh, authored per-face data included as attributes."""
    shape = shp.read_shape(shape_chunk)
    me = bpy.data.meshes.new(name)
    if shape is None or not shape.verts:
        return me, 0

    verts = [to_blender(v, _CTX["scale"], _CTX["y_down"]) for v in shape.verts]

    # **Drop the faces Blender cannot hold here, not in validate().** Two faces
    # on the same three vertices are legal in a shape and 775 of them ship
    # across 193 shapes -- 28 of Maskelyn MkII's 44 polygons are one. Letting
    # `validate()` remove them silently renumbers `me.polygons` out from under
    # the source list, so every face after the first duplicate takes the
    # *previous* face's texture, UVs, flags and merge group. Removing them
    # deterministically keeps the two lists index-for-index.
    faces, kept, lost = [], [], 0
    seen = set()
    for index, poly in enumerate(shape.polys):
        if len(poly.verts) < 3:
            continue
        key = frozenset(poly.verts)
        if len(key) < 3 or key in seen:
            lost += 1
            continue
        seen.add(key)
        faces.append(tuple(poly.verts))
        kept.append((index, poly))

    me.from_pydata(verts, [], faces)
    me.validate(verbose=False)
    if len(me.polygons) != len(kept):  # nothing shipped hits this
        lost += len(kept) - len(me.polygons)
        kept = kept[:len(me.polygons)]

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
    mg = me.attributes.new("rif_merge_group", "INT", "FACE")

    merge_vals = []
    merge = shape_chunk.find(b"SHPMRGDT")
    if merge is not None:
        n = len(merge.body) // 4
        merge_vals = list(struct.unpack_from("<%di" % n, merge.body, 0))

    uv_layer = me.uv_layers.new(name="UVMap")
    for i, (poly, (source_index, src)) in enumerate(zip(me.polygons, kept)):
        poly.material_index = slot_of.get(src.texture_index, 0)
        et.data[i].value = src.engine_type
        fl.data[i].value = src.flags
        # SHPMRGDT is one value per *source* polygon, so it is indexed by where
        # the polygon was in the file, not by where it ended up in the mesh.
        mg.data[i].value = (merge_vals[source_index]
                            if source_index < len(merge_vals) else -1)
        uvs = shape.uvs_for(src)
        hu.data[i].value = uvs is not None
        scale = scale_of.get(src.texture_index, (1.0, 1.0))
        for k, loop in enumerate(poly.loop_indices):
            if uvs is None:
                uv_layer.data[loop].uv = (0.0, 0.0)
            elif k < len(uvs):
                uv_layer.data[loop].uv = uv_to_blender(uvs[k], scale)

    me.update()
    return me, lost


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
            me, lost = _mesh_for_shape(shape_chunk, name)
            stats["lost_faces"] += lost
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

        # SHPVTINT is per-vertex lighting held on the object; it belongs on the mesh.
        vt = chunk.find(b"SHPVTINT")
        if vt is not None and obj.data is not None and len(vt.body) >= 16:
            n = (len(vt.body) - 16) // 4
            vals = struct.unpack_from("<%di" % n, vt.body, 16)
            attr = obj.data.attributes.new("rif_vertex_intensity", "INT", "POINT")
            for i in range(min(n, len(obj.data.vertices))):
                attr.data[i].value = _to_signed32(vals[i])
            obj.data["rif_vtint_header"] = list(struct.unpack_from("<4i", vt.body, 0))
    elif cid in (b"REBSHAPE", b"SUBSHAPE"):
        # A shape no object claims -- Elint MkII ships two, of 4 vertices each.
        # It still gets a real mesh rather than an empty, or its geometry would be
        # visible only as a typed array.
        me, lost = _mesh_for_shape(chunk, _label_for(chunk))
        stats["lost_faces"] += lost
        obj = bpy.data.objects.new(me.name, me)
        skip |= GEOMETRY_CHUNKS | set(ATTRIBUTE_CHUNKS) | DISCARDED_CHUNKS
    else:
        obj = bpy.data.objects.new(_label_for(chunk), None)

    obj["rif_id"] = chunk.name
    obj["rif_index"] = index
    # A container that became an object may be the one holding the texture
    # table -- REBENVDT does whenever it also holds a LIGHTSET.
    skip = _note_table_owner(chunk, obj, skip)
    obj["rif_absorbed"] = _pack_absorbed(_absorb(chunk, skip))

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
            obj.empty_display_type = "CUBE"
            skip.add(b"DUMOBJDT")
    elif cid == b"OBJCHIER":
        bound = _hierarchy_binding(chunk)
        if bound:
            obj["rif_bound"] = bound

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
                source_path=None, texture_dir="", load_images=True):
    """Chunk tree -> a Blender collection holding the whole file.

    ``source_path`` is only used to find the textures: a ``.RIM`` is not in the
    ``.rif``, so the install has to be located by walking up from the file that
    named it (``texture_dir`` overrides that). Nothing else reads it, and an
    import with no textures found differs from one with them only in what the
    materials display.
    """
    _CTX.update(scale=scale, y_down=y_down, fps=fps,
                textures={}, materials={}, images={}, texture_index=None,
                table_chunk=None,
                missing_textures=set(), undecodable_textures=set())
    stats = {"lost_faces": 0, "objects": 0}

    collection = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(collection)
    collection["rif_id"] = "REBINFF2"
    collection["rif_scale"] = scale
    collection["rif_y_down"] = y_down
    collection["rif_fps"] = fps

    _CTX["textures"] = _read_texture_table(root, collection)
    root_dir = _texture_root(source_path, texture_dir) if load_images else None
    if root_dir:
        _CTX["texture_index"] = rim.TextureIndex(root_dir)
    stats["texture_root"] = root_dir
    stats["textures"] = len(_CTX["textures"])

    kids = list(root.children or ())
    pairs = _shape_pairs(root)
    consumed = set(pairs.values())

    absorbed = []
    for i, kid in enumerate(kids):
        if kid.id == b"REBSHAPE" and i in consumed:
            continue  # built as the mesh of the object that names it
        if kid.id == b"OBJCHIER":
            continue  # the armature owns the whole hierarchy
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
    mg = me.attributes.get("rif_merge_group")
    uv_layer = me.uv_layers.active

    polys = []
    uv_lists = []
    merge = []
    for tri in me.loop_triangles:
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
        merge.append(mg.data[face].value if mg is not None and face < len(mg.data) else -1)

        flat = []
        if has_uv and uv_layer is not None:
            for loop in tri.loops:
                flat += uv_to_rif(uv_layer.data[loop].uv, texels)
        # One entry per triangle, which is what the shipped files do -- and past
        # 65,535 of them the index moves into bits 12-15, or it would wrap and
        # every face after the 65,536th would wear another face's UVs. Four
        # shipped shapes are that big.
        uv_index = len(uv_lists)
        uv_lists.append(tuple(flat))
        colour = shp.encode_colour(texture_index, uv_index)
        polys.append(shp.Poly(engine_type, len(polys), flags, colour, tuple(tri.vertices)))

    bodies = shp.build_bodies(verts, polys, uv_lists)
    children = [
        (0, rif.Chunk(b"SHPRAWVT", bodies[b"SHPRAWVT"])),
        (1, rif.Chunk(b"SHPVNORM", bodies[b"SHPVNORM"])),
        (2, rif.Chunk(b"SHPPNORM", bodies[b"SHPPNORM"])),
        (3, rif.Chunk(b"SHPPOLYS", bodies[b"SHPPOLYS"])),
        (4, rif.Chunk(b"SHPUVCRD", bodies[b"SHPUVCRD"])),
        (5, rif.Chunk(b"SHPCENTR", shp.centre_body(verts))),
        (6, rif.Chunk(b"SHPMRGDT", struct.pack("<%di" % len(merge), *merge))),
    ]
    children += [(7 + i, c) for i, (_, c) in enumerate(_emit_absorbed(me))]
    children.sort(key=lambda t: t[0])
    stats["shapes"] += 1
    return rif.Chunk(b"REBSHAPE", b"", [c for _, c in children])


def _vtint_chunk(obj, me):
    attr = me.attributes.get("rif_vertex_intensity")
    if attr is None:
        return None
    header = list(me.get("rif_vtint_header", [0, 0, 0, 0]))
    vals = [attr.data[i].value for i in range(len(me.vertices))]
    body = struct.pack("<4i", *header[:4]) + struct.pack("<%di" % len(vals), *vals)
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
        vt = _vtint_chunk(obj, obj.data)
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
        "field_0x38": obj.get("rif_field_0x38", 0),
        "range": int(round((light.cutoff_distance if light.use_custom_distance else 0.0) / scale)),
        "colour": colour,
        "flags": obj.get("rif_flags", 3),
        "field_0x48": obj.get("rif_field_0x48", 1),
        "field_0x4c": list(obj.get("rif_field_0x4c", [0, 0])),
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
    stats = {"shapes": 0, "objects": 0, "lights": 0, "textures": 0, "new_textures": 0}
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

        children = list(_emit_from(obj.get("rif_absorbed"),
                                   table_extra if table_owner == obj.name else ()))
        if cid == b"DUMMYOBJ" and "rif_dumobjdt" in obj:
            children.append((0, _dumobj_chunk(obj, scale, y_down)))
        for kid in by_parent.get(obj, ()):
            children.append((kid.get("rif_index", 0), emit(kid)))
        children.sort(key=lambda t: t[0])
        return rif.Chunk(cid, b"", [c for _, c in children])

    # File-level leaves (RIFVERIN, HIDEGDIS, ...) live on the collection, and so
    # does the texture table -- as a table, so it is rebuilt rather than replayed.
    top = _emit_from(collection.get("rif_absorbed"),
                     table_extra if table_owner == "" else ())

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
