"""Typed view over REBSHAPE geometry. Imports no ``bpy``.

Layouts here were recovered by measuring every shape in the 563 shipped ``.rif``
files, cross-checked against AvP's ``3dc/win95/CHNKTYPE.HPP``. Two of them differ
from what ``rif_chunk_format.md`` currently records, and from AvP:

- **``SHPRAWVT`` holds ``int32`` triples**, AvP's ``ChunkVectorInt`` -- not floats.
  ``SHPVNORM`` and ``SHPPNORM`` *are* floats (``ChunkVectorFloat``).
- **``SHPPOLYS`` is ``{engine_type, normal_index, flags, colour, vert_ind[5]}``**,
  36 bytes. AvP's ``ChunkPoly`` has an explicit ``num_verts`` before a
  ``vert_ind[4]``; Gunlok dropped the count and terminates the list with ``-1``.
  All 1,766,071 polygons in the shipped assets are triangles -- exactly three
  valid indices, ``-1`` in both spare slots, every index in range.

``colour`` packs the material references: texture index in the low 12 bits, UV
index in the high 16. Gunlok does *not* use AvP's extended UV encoding (bits
12-15); decoding it that way turns 12,097 valid polygons into 244,763 invalid
ones, so the plain ``colour >> 16`` is right.
"""

import math
import struct

try:  # as a package inside Blender, or flat on sys.path from the tests
    from . import rif
except ImportError:  # pragma: no cover
    import rif

POLY_STRIDE = 36
MAX_POLY_VERTS = 5

TEXTURE_INDEX_MASK = 0xFFF

#: Bits 12-15 of ``colour``. AvP's ``ChunkPoly::GetUVIndex`` folds them in as the
#: top of a 20-bit UV index whenever they are set; Gunlok uses them **only in the
#: shapes whose UV table does not fit in 16 bits**, and for something else
#: entirely everywhere else. Both halves of that are measured across all 1.77M
#: shipped polygons:
#:
#: - **Four shipped shapes have more than 65,535 UV entries** -- ``city ruins``
#:   (77,669), ``level07`` (70,764), ``level15`` (68,358), ``level12`` (65,663) --
#:   and each stores one entry per polygon in order. Decoding those with the
#:   nibble reproduces ``uv_index == polygon index`` for 282,412 of their 282,454
#:   polygons; ignoring it reproduces 262,118, because every index past 65,535
#:   silently *wraps into range* and picks some other polygon's UVs.
#: - **In every other shape the nibble is not an index.** It takes all 15 values
#:   with no pattern (0x3 on 78,643 polygons, 0x8 on 50,160, 0xe on 45,949),
#:   folding it in puts 244,763 indices out of range, and 99.05% of the polygons
#:   carrying it have no usable UV entry even under the plain decode -- they are
#:   the untextured junk the ``_shadow`` meshes are full of.
#:
#: So the rule is a property of the shape, not of the polygon: a table that
#: cannot be addressed in 16 bits is read with the nibble, and one that can is
#: not. That is decidable from the file itself and needs no heuristic.
UV_INDEX_NIBBLE = 0xF000
UV_INDEX_16BIT_MAX = 0xFFFF
UV_INDEX_MAX = 0xFFFFF


def decode_uv_index(colour, extended):
    if extended and (colour & UV_INDEX_NIBBLE):
        return ((colour & UV_INDEX_NIBBLE) << 4) | (colour >> 16)
    return colour >> 16


def encode_colour(texture_index, uv_index):
    """``colour`` for one polygon, in the form the shipped files use."""
    if uv_index > UV_INDEX_MAX:
        raise ValueError("UV index %d does not fit in 20 bits" % uv_index)
    colour = ((uv_index & 0xFFFF) << 16) | (texture_index & TEXTURE_INDEX_MASK)
    if uv_index > UV_INDEX_16BIT_MAX:
        colour |= ((uv_index >> 16) & 0xF) << 12
    return colour


class Poly:
    """One triangle. ``verts`` is the tuple of valid vertex indices."""

    __slots__ = ("engine_type", "normal_index", "flags", "colour", "verts")

    def __init__(self, engine_type, normal_index, flags, colour, verts):
        self.engine_type = engine_type
        self.normal_index = normal_index
        self.flags = flags
        self.colour = colour
        self.verts = verts

    @property
    def texture_index(self):
        return self.colour & TEXTURE_INDEX_MASK

    @property
    def uv_index(self):
        """The plain 16-bit index. :meth:`Shape.uv_index_for` is the real answer,
        because whether bits 12-15 count is a property of the shape's UV table
        rather than of the polygon -- see :data:`UV_INDEX_NIBBLE`."""
        return self.colour >> 16

    def __repr__(self):
        return "<Poly %r tex=%d uv=%d>" % (self.verts, self.texture_index, self.uv_index)


class Shape:
    """Decoded geometry for one REBSHAPE/SUBSHAPE chunk.

    Holds a reference to the source ``chunk`` so an exporter can write back into
    the same tree and leave every sibling it does not understand untouched.
    """

    __slots__ = ("chunk", "verts", "polys", "uv_lists", "vert_normals", "poly_normals",
                 "extended_uv")

    def __init__(self, chunk, verts, polys, uv_lists, vert_normals, poly_normals):
        self.chunk = chunk
        self.verts = verts
        self.polys = polys
        self.uv_lists = uv_lists
        self.vert_normals = vert_normals
        self.poly_normals = poly_normals
        #: Whether this shape's UV table needs more than 16 bits to address.
        self.extended_uv = len(uv_lists) > UV_INDEX_16BIT_MAX

    def uv_index_for(self, poly):
        return decode_uv_index(poly.colour, self.extended_uv)

    def uvs_for(self, poly):
        """UV pairs for this polygon, or None when it carries no usable UVs.

        Values are **texels**, not fractions: they run 0..width and 0..height, so
        they only mean something beside the size of the texture the polygon
        names. See ``rif_chunk_format.md``.

        The UV list is indexed, not parallel to the polygon list -- 139 shapes
        have fewer entries than polygons. 534 shapes also contain polygons whose
        index points past the end (concentrated in ``_shadow`` meshes, which are
        untextured); those get None rather than an exception.
        """
        i = self.uv_index_for(poly)
        if i >= len(self.uv_lists):
            return None
        uv = self.uv_lists[i]
        if len(uv) != len(poly.verts) * 2:
            return None
        return [(uv[k * 2], uv[k * 2 + 1]) for k in range(len(poly.verts))]

    def bounds(self):
        if not self.verts:
            return (0, 0, 0), (0, 0, 0)
        lo = tuple(min(v[k] for v in self.verts) for k in range(3))
        hi = tuple(max(v[k] for v in self.verts) for k in range(3))
        return lo, hi

    def centre(self):
        """The SHPCENTR payload: integer centre of the bounds + radius about the origin.

        The division truncates toward zero, matching C's ``/`` on the negative
        sums -- flooring instead is off by one on roughly half of them.
        """
        lo, hi = self.bounds()
        centre = tuple(int((lo[k] + hi[k]) / 2) for k in range(3))
        radius = max((math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2) for v in self.verts),
                     default=0.0)
        return centre, radius


def _read_vec3i(body):
    return [struct.unpack_from("<iii", body, i) for i in range(0, len(body) - 11, 12)]


def _read_vec3f(body):
    return [struct.unpack_from("<fff", body, i) for i in range(0, len(body) - 11, 12)]


def read_shape(chunk):
    """Decode a REBSHAPE/SUBSHAPE chunk, or return None if it carries no mesh."""
    raw = chunk.find(b"SHPRAWVT")
    pol = chunk.find(b"SHPPOLYS")
    if raw is None or pol is None:
        return None

    verts = _read_vec3i(raw.body)

    polys = []
    for off in range(0, len(pol.body) - POLY_STRIDE + 1, POLY_STRIDE):
        engine_type, normal_index, flags, colour = struct.unpack_from("<iiiI", pol.body, off)
        idx = struct.unpack_from("<%di" % MAX_POLY_VERTS, pol.body, off + 16)
        n = 0
        while n < MAX_POLY_VERTS and idx[n] != -1:
            n += 1
        polys.append(Poly(engine_type, normal_index, flags, colour, idx[:n]))

    uv_lists = []
    uvc = chunk.find(b"SHPUVCRD")
    if uvc is not None and len(uvc.body) >= 4:
        count, = struct.unpack_from("<I", uvc.body, 0)
        off = 4
        for _ in range(count):
            if off + 4 > len(uvc.body):
                break
            n, = struct.unpack_from("<I", uvc.body, off)
            off += 4
            if off + n * 8 > len(uvc.body):
                break
            uv_lists.append(struct.unpack_from("<%df" % (n * 2), uvc.body, off))
            off += n * 8

    vn = chunk.find(b"SHPVNORM")
    pn = chunk.find(b"SHPPNORM")
    return Shape(
        chunk,
        verts,
        polys,
        uv_lists,
        _read_vec3f(vn.body) if vn is not None else [],
        _read_vec3f(pn.body) if pn is not None else [],
    )


def iter_shapes(root):
    """Yield a Shape for every REBSHAPE/SUBSHAPE in the tree that has geometry."""
    for chunk in root.walk():
        if chunk.id in (b"REBSHAPE", b"SUBSHAPE"):
            shape = read_shape(chunk)
            if shape is not None:
                yield shape


# --------------------------------------------------------------------------
# Encoding
# --------------------------------------------------------------------------

#: Chunks derived from the mesh that this module cannot regenerate. They are
#: dropped when geometry changes, because keeping a stale one is worse than
#: keeping none -- their contents are indexed by vertex/polygon count.
DERIVED_CHUNKS = (b"SHPPCINF", b"SHPMRGDT", b"SHPVTINT")


def _normalize(x, y, z):
    n = math.sqrt(x * x + y * y + z * z)
    if n == 0.0:
        return (0.0, 0.0, 0.0)
    return (x / n, y / n, z / n)


def face_normal(verts, tri):
    a, b, c = (verts[i] for i in tri)
    ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
    return _normalize(uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)


def build_bodies(verts, polys, uv_lists):
    """Render mesh data to the five chunk bodies that describe it.

    Vertex normals are the normalized average of the incident face normals, which
    is what the shipped data looks like but is *not* verified to reproduce it
    exactly; see ``tests/test_shapes.py``.
    """
    raw = bytearray()
    for v in verts:
        raw += struct.pack("<iii", int(v[0]), int(v[1]), int(v[2]))

    pnorms = [face_normal(verts, p.verts) for p in polys]

    accum = [[0.0, 0.0, 0.0] for _ in verts]
    for p, n in zip(polys, pnorms):
        for vi in p.verts:
            accum[vi][0] += n[0]
            accum[vi][1] += n[1]
            accum[vi][2] += n[2]
    vnorms = [_normalize(*a) for a in accum]

    poly_body = bytearray()
    for i, p in enumerate(polys):
        poly_body += struct.pack("<iiiI", p.engine_type, i, p.flags, p.colour)
        idx = list(p.verts) + [-1] * (MAX_POLY_VERTS - len(p.verts))
        poly_body += struct.pack("<%di" % MAX_POLY_VERTS, *idx)

    uv_body = bytearray(struct.pack("<I", len(uv_lists)))
    for uv in uv_lists:
        uv_body += struct.pack("<I", len(uv) // 2)
        uv_body += struct.pack("<%df" % len(uv), *uv)

    vn_body = bytearray()
    for n in vnorms:
        vn_body += struct.pack("<fff", *n)
    pn_body = bytearray()
    for n in pnorms:
        pn_body += struct.pack("<fff", *n)

    return {
        b"SHPRAWVT": bytes(raw),
        b"SHPPOLYS": bytes(poly_body),
        b"SHPUVCRD": bytes(uv_body),
        b"SHPVNORM": bytes(vn_body),
        b"SHPPNORM": bytes(pn_body),
    }


def centre_body(verts):
    lo = tuple(min(v[k] for v in verts) for k in range(3)) if verts else (0, 0, 0)
    hi = tuple(max(v[k] for v in verts) for k in range(3)) if verts else (0, 0, 0)
    centre = tuple(int((lo[k] + hi[k]) / 2) for k in range(3))
    radius = max((math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2) for v in verts), default=0.0)
    return struct.pack("<iiif", centre[0], centre[1], centre[2], radius)


def write_shape(chunk, verts, polys, uv_lists):
    """Replace the geometry of ``chunk`` in place.

    Returns the list of derived chunk ids that had to be dropped, or ``None`` if
    the geometry was identical to what the chunk already held -- in which case
    nothing is touched at all. That check is what makes an import/export cycle
    with no edits reproduce the source byte for byte: a shape only pays the cost
    of losing its unregenerable derived chunks if it actually changed.

    Every sibling chunk not named here keeps its original bytes and position.
    """
    bodies = build_bodies(verts, polys, uv_lists)

    unchanged = True
    for cid in (b"SHPRAWVT", b"SHPPOLYS", b"SHPUVCRD"):
        existing = chunk.find(cid)
        if existing is None:
            if bodies[cid] not in (b"", struct.pack("<I", 0)):
                unchanged = False
                break
        elif existing.body != bodies[cid]:
            unchanged = False
            break
    if unchanged:
        return None

    bodies[b"SHPCENTR"] = centre_body(verts)

    dropped = []
    kept = []
    for child in chunk.children:
        if child.id in bodies:
            child.body = bodies.pop(child.id)
            child.children = None
            kept.append(child)
        elif child.id in DERIVED_CHUNKS:
            dropped.append(child.name)
        else:
            kept.append(child)

    # Anything the shape did not already have (a mesh that gained UVs, say) is
    # appended after the chunks that were present.
    for cid, body in bodies.items():
        kept.append(rif.Chunk(cid, body))

    chunk.children = kept
    return dropped
