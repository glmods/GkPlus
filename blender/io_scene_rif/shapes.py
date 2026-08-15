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
#:
#: ``scene.py`` does reconstruct ``SHPMRGDT``, from the **quads** each pair was
#: imported as (:func:`plan_faces`) and, for a pair that could not be one, from
#: the **pair id** the scene stores in place of the polygon index the wire holds
#: -- see :func:`merge_pairs_from_wire` below, and :func:`merge_problems`, which
#: export runs before writing the chunk at all.
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


def weld_map(verts):
    """Vertex index -> index of the first vertex sharing its exact position.

    The engine welds vertex records by position when it loads a shape, so this is
    the identity its topology actually runs on -- and because ``SHPRAWVT`` is
    integer, two vertices closer together than one RIF unit weld even when they
    are distinct in Blender. Export quantizes, so this can only be computed on
    the quantized coordinates.
    """
    first, out = {}, []
    for i, v in enumerate(verts):
        out.append(first.setdefault(tuple(v), i))
    return out


def welds_degenerate(tri, welded):
    """Does this triangle lose a corner once its vertices are welded?

    Such a polygon carries a *repeated* vertex record, and Gunlok's polygon
    adjacency predicate (vtable slot 0x50, ``PolygonAdjacencyTest`` @ 0x0048ecf0)
    collects shared vertices into a fixed three-element buffer without a bound
    check. Two such polygons meeting in one section-grid cell reach four or more
    matches, and the fourth write lands exactly on the function's /GS cookie --
    the game dies during ``LoadOrBuildSectionAdjacency`` with
    STATUS_STACK_BUFFER_OVERRUN (0xc0000409), nowhere near anything that names
    the asset.

    **Only level geometry reaches that code**, which is why the shipped set is
    not evidence that this is fine: 14 files carry such a triangle
    (``corps building.RIF`` has 22, ``gastowerfrag.RIF`` 14) but every one is
    under ``RIF\\Objects`` or ``RIF\\Units``, and **no file under ``RIF\\Levels``
    has a single one**. A prop never goes through section adjacency. Since a
    triangle that loses a corner has zero area and renders nothing either way,
    export drops it everywhere rather than guessing which shape will be a map.
    """
    return len({welded[i] for i in tri}) < len(tri)


# --------------------------------------------------------------------------
# SHPMRGDT: a pairing between polygons, stored as a shared id
# --------------------------------------------------------------------------
#
# `SHPMRGDT` holds one int32 per polygon naming **the polygon it pairs with** to
# be fused into a quad -- an involution, not a per-face group id. So it is a
# *reference into the polygon list*, and any editor that reorders or drops a
# polygon invalidates it wholesale: dropping one face renumbers every face after
# it, so a single drop corrupts the entire table rather than one pair.
#
# `MergePolygonsInChunkShape` @ 0x005d7900 walks it bounded by the `SHPPOLYS`
# count -- never by `SHPMRGDT`'s own, which its loader computes and nothing ever
# compares -- indexes `poly_list[m[i]]` with **no range check**, and writes into
# a `malloc(num_polys * 0x24)` with no bound on the write cursor. Break the
# involution and it writes off the end of the heap block. Full trace in
# `rif_chunk_format.md`, "Merging polygons into quads".
#
# The scene therefore stores a **pair id**: both partners of a pair carry the
# same number, everything else carries `MERGE_NONE`. That is stable under
# reordering and degrades correctly under dropping -- a survivor whose partner
# went simply has an id nothing else shares, and writes `-1`.

#: `SHPMRGDT`'s "this polygon pairs with nothing", and the scene's "unpaired".
MERGE_NONE = -1


def merge_pairs_from_wire(merge, num_polys):
    """``SHPMRGDT`` values -> one shared id per pair, :data:`MERGE_NONE` elsewhere.

    A pairing the engine would not act on -- out of range, self-referential, or
    not mirrored -- is read as unpaired rather than carried, so a malformed
    source file cannot become a malformed output file. No shipped shape needs
    that: all 9,357 are proper involutions.
    """
    out = [MERGE_NONE] * num_polys
    nxt = 0
    for i in range(num_polys):
        if out[i] != MERGE_NONE:
            continue
        j = merge[i] if i < len(merge) else MERGE_NONE
        if j in (MERGE_NONE, i) or not 0 <= j < num_polys:
            continue
        if (merge[j] if j < len(merge) else MERGE_NONE) != i:
            continue
        out[i] = out[j] = nxt
        nxt += 1
    return out


def merge_wire_from_pairs(ids):
    """The reverse: ``(SHPMRGDT values, ids that could not be paired)``.

    An id carried by exactly two polygons becomes a mutual pair. One carried by
    a single polygon is a pair whose partner was dropped, and writes
    :data:`MERGE_NONE` -- which is the whole point of storing an id rather than
    an index. One carried by three or more is what duplicating a face in Blender
    produces: the first two in index order pair and the rest go unpaired, since
    nothing can say which two were meant.
    """
    where = {}
    for i, pair_id in enumerate(ids):
        if pair_id != MERGE_NONE:
            where.setdefault(pair_id, []).append(i)

    out = [MERGE_NONE] * len(ids)
    unpaired = 0
    for members in where.values():
        if len(members) < 2:
            unpaired += 1
            continue
        a, b = members[0], members[1]
        out[a], out[b] = b, a
        unpaired += len(members) - 2
    return out, unpaired


def merge_problems(merge, num_polys):
    """Every way this ``SHPMRGDT`` would break ``MergePolygonsInChunkShape``.

    Empty means the engine can walk it. This is the predicate all 24 shipped
    level map objects satisfy, and the one the export validates against before
    writing -- because the failure is an out-of-bounds heap write during level
    load, with a fault address that names neither the file nor the polygon.
    """
    out = []
    if len(merge) != num_polys:
        out.append("%d merge entries for %d polygons; the engine reads "
                   "num_polys of them regardless" % (len(merge), num_polys))
    for i, j in enumerate(merge[:num_polys]):
        if j == MERGE_NONE:
            continue
        if not 0 <= j < num_polys:
            out.append("m[%d] = %d is outside [0, %d)" % (i, j, num_polys))
        elif j == i:
            out.append("m[%d] pairs polygon %d with itself" % (i, i))
        elif j >= len(merge) or merge[j] != i:
            out.append("m[%d] = %d but m[%d] = %s; the pairing is not mutual"
                       % (i, j, j, merge[j] if j < len(merge) else "absent"))
        if len(out) >= 8:
            out.append("... and possibly more")
            break
    return out


# --------------------------------------------------------------------------
# Fusing a pair into a quad, and splitting it back
# --------------------------------------------------------------------------
#
# A merge pair *is* a quad -- that is what `MergePolygonsInChunkShape` makes of
# it -- so it is the natural thing for a modeller to edit. The wire form stays
# two triangles plus `SHPMRGDT`, because that is what all 1,766,071 shipped
# polygons are, and the engine builds its render buffers from the unmerged list
# before the merger ever runs.
#
# The fuse has to be lossless in both directions, which is a stronger condition
# than the engine's own merge test. Everything a Blender face can hold only once
# has to already agree between the two triangles -- material, `engine_type`,
# `flags`, and the UVs at the two shared vertices -- and the quad has to
# tessellate back into exactly the two triangles it came from.

#: `MergePolygonsInChunkShape` picks its comparator on `engine_type`:
#: `TexMergePolys` @ 0x005d7590 inside these ranges, `MergePolys` @ 0x005d77e0
#: outside. The two do not test the same things, and only the textured one
#: reads UVs -- see :func:`merges_by_texture`.
TEXTURED_MERGE_TYPES = ((5, 7), (0x14, 0x18))


def merges_by_texture(engine_type):
    """Does this polygon's merge go through ``TexMergePolys``?

    That matters to a *writer* because ``TexMergePolys`` reads one ``(u,v)`` per
    vertex out of each partner's UV record and writes a fused four-entry record
    back into the lower-indexed one. A polygon with no UV record at all makes
    both of those run off the end of it. No shipped pair is in that shape --
    all 516,550 textured-path pairs have a full record on both partners -- so
    export refuses to write one rather than find out what the engine does.
    """
    return any(lo <= engine_type <= hi for lo, hi in TEXTURED_MERGE_TYPES)


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def quad_would_flip(verts, quad, normal):
    """Would Blender tessellate this quad along the *other* diagonal?

    ``calc_loop_triangles`` splits a quad ``0-1-2`` / ``0-2-3`` unless
    ``is_quad_flip_v3_first_third_fast`` rejects that diagonal, in which case it
    splits ``0-1-3`` / ``1-2-3``. :func:`fuse_quad` puts the shared edge *on*
    the ``0-2`` diagonal precisely so that export reproduces the two source
    triangles, so a quad the tessellator would flip must not be fused at all --
    it would come back as two different triangles with two different normals,
    and on level geometry the normal is what decides walkability.

    Both of Blender's variants are checked, because which one runs depends on
    whether face normals are being computed in the same pass and that is not
    ours to choose: the plain one asks whether the two halves face opposite
    ways, the ``_with_normal`` one whether the diagonal lies inside the quad at
    all. They agree on a coplanar quad and part company on a folded one.

    The test is sign-only, so it is valid in RIF integer space: the importer's
    swizzle to Blender is a rotation composed with a positive uniform scale.
    """
    v1, v2, v3, v4 = (verts[i] for i in quad)
    d13 = _sub(v3, v1)
    if _dot(_cross(_sub(v2, v1), d13), _cross(_sub(v4, v1), d13)) > 0.0:
        return True
    tangent = _cross(d13, normal)
    base = _dot(v1, tangent)
    return _dot(v4, tangent) >= base or _dot(v2, tangent) < base


def fuse_quad(shape, ia, ib):
    """Two paired triangles as one quad: ``(vertex indices, uvs)`` or None.

    The quad is wound ``(shared0, odd_b, shared1, odd_a)`` so that its ``0-2``
    diagonal is the shared edge and Blender's tessellation gives back ``b`` then
    ``a``. ``uvs`` is one texel pair per corner, or None when neither triangle
    has a UV record.

    None means "leave these two as triangles" -- the pairing still rides along
    as :data:`MERGE_NONE`-or-not on the face attribute, so nothing is lost by
    refusing. Every reason to refuse is a thing a Blender face cannot hold
    twice, or a tessellation that would not come back.
    """
    a, b = shape.polys[ia], shape.polys[ib]
    if len(a.verts) != 3 or len(b.verts) != 3:
        return None
    # A face carries one material, one engine_type and one flags word. The
    # engine's own mergers test all three too, so a pair that disagrees would
    # not have been merged in game either.
    if (a.engine_type, a.flags, a.texture_index) != (b.engine_type, b.flags, b.texture_index):
        return None

    av, bv = list(a.verts), list(b.verts)
    shared = [v for v in av if v in bv]
    if len(shared) != 2:
        return None
    if av[(av.index(shared[0]) + 1) % 3] != shared[1]:
        shared.reverse()
    ka = av.index(shared[0])
    if av[(ka + 1) % 3] != shared[1]:
        return None
    # Consistent winding: `b` must cross the shared edge the other way, or the
    # two triangles are a fold rather than two halves of one surface.
    kb = bv.index(shared[1])
    if bv[(kb + 1) % 3] != shared[0]:
        return None
    odd_a, odd_b = av[(ka + 2) % 3], bv[(kb + 2) % 3]

    ua, ub = shape.uvs_for(a), shape.uvs_for(b)
    if (ua is None) != (ub is None):
        return None
    uvs = None
    if ua is not None:
        # A quad holds one UV per corner, so a seam across the shared edge
        # cannot be fused. `TexMergePolys` requires the same thing, bit for
        # bit, which is why 27 shipped pairs never merge in game either.
        if any(ua[av.index(v)] != ub[bv.index(v)] for v in shared):
            return None
        uvs = [ua[ka], ub[bv.index(odd_b)], ua[(ka + 1) % 3], ua[(ka + 2) % 3]]

    na = face_normal(shape.verts, a.verts)
    nb = face_normal(shape.verts, b.verts)
    if _dot(na, nb) <= 0.0:
        return None
    quad = (shared[0], odd_b, shared[1], odd_a)
    if len(set(quad)) != 4:
        return None
    if quad_would_flip(shape.verts, quad,
                       (na[0] + nb[0], na[1] + nb[1], na[2] + nb[2])):
        return None
    return quad, uvs


class Face:
    """One face of the Blender mesh, and the source polygons it stands for.

    ``sources`` is one polygon index for a triangle and two for a fused quad,
    lower first -- the same partner the engine's merger takes the merged
    polygon's attributes from.
    """

    __slots__ = ("verts", "sources", "uvs")

    def __init__(self, verts, sources, uvs):
        self.verts = verts
        self.sources = sources
        self.uvs = uvs

    def __repr__(self):
        return "<Face %r from %r>" % (self.verts, self.sources)


def plan_faces(shape, pair_ids=None):
    """The faces a Blender mesh should hold for this shape: ``(faces, lost)``.

    ``lost`` counts the polygons Blender cannot represent -- two faces on the
    same three vertices, which 775 shipped polygons across 193 shapes are.
    **They are dropped here rather than by** ``validate()``, which renumbers
    ``me.polygons`` out from under the source list and makes every face after
    the first duplicate wear the previous one's texture, UVs and flags.

    ``pair_ids`` is the pair-id form of ``SHPMRGDT``
    (:func:`merge_pairs_from_wire`); pass it to fuse each pair into a quad, or
    None to keep every polygon a triangle. Fusing never changes *which* source
    polygons survive -- the drop above is decided first, over the original
    numbering -- so the two options differ only in how the survivors are
    grouped.
    """
    kept, seen, lost = [], set(), 0
    for index, poly in enumerate(shape.polys):
        if len(poly.verts) < 3:
            continue
        key = frozenset(poly.verts)
        if len(key) < 3 or key in seen:
            lost += 1
            continue
        seen.add(key)
        kept.append(index)

    partner = {}
    if pair_ids:
        members = {}
        for index in kept:
            pid = pair_ids[index] if index < len(pair_ids) else MERGE_NONE
            if pid != MERGE_NONE:
                members.setdefault(pid, []).append(index)
        for group in members.values():
            if len(group) == 2:
                partner[group[0]], partner[group[1]] = group[1], group[0]

    faces, taken, quad_keys = [], set(), set()
    for index in kept:
        if index in taken:
            continue
        other = partner.get(index)
        if other is not None and other > index:
            fused = fuse_quad(shape, index, other)
            # A second quad on the same four vertices is the duplicate-face rule
            # again, one dimension up: Blender would drop one of them.
            if fused is not None and frozenset(fused[0]) not in quad_keys:
                quad_keys.add(frozenset(fused[0]))
                faces.append(Face(fused[0], (index, other), fused[1]))
                taken.update((index, other))
                continue
        poly = shape.polys[index]
        faces.append(Face(tuple(poly.verts), (index,), shape.uvs_for(poly)))
        taken.add(index)
    return faces, lost


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


# --------------------------------------------------------------------------
# Navigation mesh
# --------------------------------------------------------------------------
#
# Gunlok has no separate navmesh: characters walk on the level's own polygons.
# ``FUN_004888d0`` turns the map geometry into an array of 0x40-byte nav
# polygons, storing the object-rotated ``SHPPNORM`` at +0x08 and the masked
# ``SHPPOLYS`` flags at +0x14, and ``BuildPolygonAdjacencyGrid`` @ 0x0048aa00
# then links the survivors into the graph the ``.map`` sidecar caches.  Its
# filter is the definition of "walkable":
#
#     if ((flags >> 8 & 1) == 0 && normal.y < 0) ... slot 0x50 ...
#
# The builder writes those two fields like this (0x00488c?? region):
#
#     flags = SHPPOLYS.flags & 0x3fffc1
#     if (ny*ny + 1e-5 < nx*nx + nz*nz) flags |= 0x100      # steeper than 45 deg
#     if (ny == 0.0)                    ny = -1e-5
#
# so bit 0x100 means "not walkable", it survives the mask, and level01 sets it
# on 1,914 polygons by hand -- i.e. it is an authored blocker as well as the
# loader's slope verdict.

#: ``SHPPOLYS.flags`` bit meaning "a character may not stand here".
NAV_BLOCKED_FLAG = 0x100

#: The bits of ``SHPPOLYS.flags`` the nav builder keeps.
NAV_FLAG_MASK = 0x3FFFC1

#: Tie-break epsilon in the slope test. It puts the limit at exactly 45 degrees:
#: for a unit normal ``ny**2 + eps >= 1 - ny**2`` solves to ``|ny| >= 0.7071``.
NAV_SLOPE_EPSILON = 1e-5


def nav_is_steep(normal):
    """Is this face steeper than the engine's 45 degree limit?

    Written component for component rather than as an angle threshold, because
    that is what the engine does and it does not assume a unit normal.
    """
    nx, ny, nz = normal
    return ny * ny + NAV_SLOPE_EPSILON < nx * nx + nz * nz


def nav_flags(normal, flags):
    """The flags word the nav builder stores, given the RIF-space normal."""
    out = flags & NAV_FLAG_MASK
    if nav_is_steep(normal):
        out |= NAV_BLOCKED_FLAG
    return out


def is_walkable(normal, flags):
    """Can a character stand on this polygon?  ``normal`` is in RIF coordinates.

    Two conditions, both from the adjacency filter: the polygon must not be
    blocked, and it must face **up** -- which in RIF's Y-down world means a
    negative Y. A face whose normal is exactly horizontal is nudged to -1e-5 by
    the loader, reproduced here, though such a face is always steep anyway.
    """
    ny = normal[1]
    if ny == 0.0:
        ny = -NAV_SLOPE_EPSILON
    return (nav_flags(normal, flags) & NAV_BLOCKED_FLAG) == 0 and ny < 0.0


def nav_islands(faces, walkable):
    """Group walkable faces into connected regions.

    ``faces`` maps a face index to its tuple of **welded** vertex ids; two
    walkable faces are linked when they share exactly two of them, which is what
    ``PolygonAdjacencyTest`` (slot 0x50) accepts -- it collects the shared
    vertices and returns true only on a count of 2, i.e. a shared edge.

    Returns ``{face index: island id}`` over the walkable faces only, with island
    ids assigned in ascending order of size (0 is the largest), so the region a
    unit can actually reach is easy to pick out.
    """
    parent = {f: f for f in faces if walkable(f)}

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[rb] = ra

    by_edge = {}
    for f in parent:
        vs = faces[f]
        for i in range(len(vs)):
            a, b = vs[i], vs[(i + 1) % len(vs)]
            by_edge.setdefault((a, b) if a < b else (b, a), []).append(f)
    for shared in by_edge.values():
        for other in shared[1:]:
            union(shared[0], other)

    groups = {}
    for f in parent:
        groups.setdefault(find(f), []).append(f)
    order = sorted(groups.values(), key=len, reverse=True)
    return {f: i for i, members in enumerate(order) for f in members}
