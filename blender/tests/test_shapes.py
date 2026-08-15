"""Validate the REBSHAPE decode/encode against every shipped shape.

    python blender/tests/test_shapes.py "<Gunlok dir>"

Four claims are checked, in decreasing order of how much the importer leans on them:

1. Every polygon is a triangle with in-range vertex indices.
2. Re-encoding an *unchanged* shape reproduces SHPRAWVT, SHPPOLYS and SHPUVCRD
   byte for byte -- the geometry chunks the exporter fully owns.
3. The regenerated SHPCENTR matches the shipped one byte for byte, which pins
   both the integer centre rounding and "radius about the origin, not the centre".
4. Recomputed face normals agree in *direction* with the shipped SHPPNORM, which
   is what fixes the winding convention. Floats will not match bit for bit, so
   this reports the angular error distribution instead of asserting equality.
"""

import os
import sys
import math
import struct
import collections

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "io_scene_rif"))

import rif  # noqa: E402
import shapes as shp  # noqa: E402


def _control():
    """The validator must reject each shape the engine cannot walk.

    A predicate nothing violates proves nothing, and every one of these is a
    real thing the exporter used to produce: a renumbered index that ran off the
    end, a pairing whose partner was dropped, a table shorter than the polygon
    list the engine reads regardless.
    """
    cases = [
        ([-1, 2, 1, 4], 4),        # m[3] = 4 is out of range
        ([1, 0, 3, -1], 4),        # m[2] = 3 but m[3] = -1: not mutual
        ([2, -1, 2, -1], 4),       # m[2] pairs with itself
        ([1, 0], 4),               # two entries for four polygons
    ]
    caught = sum(1 for merge, n in cases if shp.merge_problems(merge, n))
    good = [1, 0, -1, -1]
    if shp.merge_problems(good, 4):
        return 0                   # a valid table must pass, or the rest is noise
    return caught


def _tri_rotations(tri):
    tri = tuple(tri)
    return {tri[k:] + tri[:k] for k in range(len(tri))}


def _tessellate(quad):
    """Blender's own split of a quad: ``0-1-2`` then ``0-2-3``."""
    return (quad[0], quad[1], quad[2]), (quad[0], quad[2], quad[3])


def _fuse_control():
    """:func:`shapes.fuse_quad` must refuse each pair a quad cannot represent.

    Every case here is something the shipped set contains at least one of, and
    a fusion that took it would either lose per-face data Blender can hold only
    once or come back as two different triangles.
    """
    verts = [(0, 0, 0), (10, 0, 0), (5, 10, 0), (5, -10, 0), (50, 0, 0), (60, 10, 0)]

    def shape(polys, uv_lists=()):
        return shp.Shape(None, verts, polys, list(uv_lists), [], [])

    def poly(vs, engine_type=3, flags=0, colour=1):
        return shp.Poly(engine_type, 0, flags, colour, vs)

    good = shape([poly((2, 0, 1)), poly((3, 1, 0))])
    if shp.fuse_quad(good, 0, 1) is None:
        return 0                    # a fusable pair must fuse, or the rest is noise

    cases = [
        # A fold: `b` crosses the shared edge the same way `a` does, so the two
        # triangles face opposite ways and are not two halves of one surface.
        shape([poly((2, 0, 1)), poly((3, 0, 1))]),
        # Different flags -- one face, one flags word. 1 shipped pair.
        shape([poly((2, 0, 1)), poly((3, 1, 0), flags=0x100)]),
        # Different material. A face carries one.
        shape([poly((2, 0, 1)), poly((3, 1, 0), colour=2)]),
        # Not adjacent: one shared vertex. This is the pairing that makes
        # `TexMergePolys` clobber a UV record before it notices (0x005d777a).
        shape([poly((2, 0, 1)), poly((1, 4, 5))]),
        # A UV seam across the shared edge -- a quad holds one UV per corner.
        shape([poly((2, 0, 1)), poly((3, 1, 0))],
              [(0.0, 0.0, 1.0, 0.0, 1.0, 1.0), (9.0, 9.0, 1.0, 0.0, 1.0, 1.0)]),
    ]
    cases[-1].polys[0].colour = 1 | (0 << 16)
    cases[-1].polys[1].colour = 1 | (1 << 16)
    caught = sum(1 for s in cases if shp.fuse_quad(s, 0, 1) is None)

    # And the tessellation predicate itself, on a dart -- the one shape whose
    # two diagonals are not interchangeable. With the reflex corner at index 1
    # the `0-2` split falls outside the quad and Blender flips it; rotated so
    # the reflex corner is at 0, the same four points split cleanly. A fused
    # pair can never be either (consistent winding across a shared edge puts the
    # two apexes on opposite sides of it), which is exactly why the predicate
    # needs a control of its own.
    dart = [(0, 4, 0), (1, 1, 0), (4, 0, 0), (0, 0, 0)]
    normal = (0, 0, -1)
    if not shp.quad_would_flip(dart, (0, 1, 2, 3), normal):
        return 0
    if shp.quad_would_flip(dart, (1, 2, 3, 0), normal):
        return 0
    return caught


def main(game_dir):
    stats = collections.Counter()
    failures = []
    dots = []
    radius_deltas = []
    degenerate = 0

    paths = sorted(
        os.path.join(dp, nm)
        for dp, _, ns in os.walk(game_dir)
        for nm in ns
        if nm.lower().endswith(".rif")
    )

    for path in paths:
        rel = os.path.relpath(path, game_dir)
        try:
            root = rif.load(path)
        except Exception as exc:  # noqa: BLE001
            failures.append((rel, repr(exc)))
            continue

        for shape in shp.iter_shapes(root):
            stats["shapes"] += 1
            nv = len(shape.verts)

            for poly_index, p in enumerate(shape.polys):
                stats["polys"] += 1
                if len(p.verts) != 3:
                    failures.append((rel, "polygon with %d verts" % len(p.verts)))
                for vi in p.verts:
                    if not 0 <= vi < nv:
                        failures.append((rel, "vertex index %d of %d" % (vi, nv)))
                if shape.uvs_for(p) is not None:
                    stats["polys with uvs"] += 1

                # The UV index survives `colour`, including the four shapes whose
                # table needs more than 16 bits and so carries the top of the
                # index in bits 12-15.
                uv_index = shape.uv_index_for(p)
                if uv_index <= shp.UV_INDEX_MAX:
                    again = shp.decode_uv_index(
                        shp.encode_colour(p.texture_index, uv_index), shape.extended_uv)
                    if again != uv_index:
                        failures.append((rel, "uv index %d re-encodes as %d"
                                         % (uv_index, again)))
                    else:
                        stats["uv indices re-encoded exactly"] += 1
                if shape.extended_uv:
                    # Those four shapes store one entry per polygon, in order,
                    # which is what makes the nibble readable as an index at all:
                    # ignoring it would silently wrap every index past 65,535
                    # onto some other polygon's UVs.
                    stats["polys in a >16-bit UV table"] += 1
                    stats["... whose uv index is its own position"] += uv_index == poly_index

            # (2) round-trip the geometry chunks
            bodies = shp.build_bodies(shape.verts, shape.polys, shape.uv_lists)
            for cid in (b"SHPRAWVT", b"SHPPOLYS", b"SHPUVCRD"):
                src = shape.chunk.find(cid)
                if src is None:
                    continue
                if bodies[cid] != src.body:
                    failures.append((rel, "%s re-encode differs (%d vs %d bytes)"
                                     % (cid.decode(), len(bodies[cid]), len(src.body))))
                else:
                    stats["geometry chunks re-encoded exactly"] += 1

            # (3) SHPCENTR. The integer centre is a hard invariant and must match
            # exactly -- that is what pins the truncate-toward-zero rounding.
            #
            # The radius is *not* asserted. It agrees to float32 precision for
            # about three quarters of the shapes and then drifts, by up to 3.2
            # absolute -- more than the sqrt(3)/2 a coordinate rounding could
            # explain, and geometrically identical shapes ship with different
            # values. So the shipped radius is partly stale authoring output and
            # is not a function of the final vertex list. Regenerating it from the
            # real vertices, as the exporter does, is the more correct value; the
            # spread is reported so a change in it gets noticed.
            src = shape.chunk.find(b"SHPCENTR")
            if src is not None and len(src.body) == 16:
                want = struct.unpack("<iiif", src.body)
                mine = struct.unpack("<iiif", shp.centre_body(shape.verts))
                if want[:3] != mine[:3]:
                    failures.append((rel, "SHPCENTR centre %r != %r" % (mine[:3], want[:3])))
                else:
                    stats["SHPCENTR centre exact"] += 1
                    if src.body == shp.centre_body(shape.verts):
                        stats["SHPCENTR byte-exact"] += 1
                    radius_deltas.append(abs(want[3] - mine[3]))

            # (3b) SHPMRGDT, which is the one chunk a generator can get fatally
            # wrong: `MergePolygonsInChunkShape` @ 0x005d7900 has no bounds check
            # anywhere, so a pairing that is not an exact involution is an
            # out-of-bounds heap write during level load. Two claims here -- that
            # every shipped shape satisfies the predicate (so it really is the
            # engine's contract and not a guess), and that the pair-id form the
            # scene stores reproduces the wire values exactly.
            src = shape.chunk.find(b"SHPMRGDT")
            if src is not None:
                n = len(shape.polys)
                wire = list(struct.unpack("<%di" % (len(src.body) // 4), src.body))
                broken = shp.merge_problems(wire, n)
                if broken:
                    failures.append((rel, "shipped SHPMRGDT is not walkable: %s" % broken[0]))
                else:
                    stats["SHPMRGDT valid as shipped"] += 1
                    again, unpaired = shp.merge_wire_from_pairs(
                        shp.merge_pairs_from_wire(wire, n))
                    if again != wire[:n] or unpaired:
                        failures.append(
                            (rel, "SHPMRGDT does not survive the pair-id form "
                                  "(%d unpaired)" % unpaired))
                    else:
                        stats["SHPMRGDT round-trips through pair ids"] += 1

            # (3c) Fusing each pair into the quad it stands for, which is what
            # the importer builds. The claim is that it is **lossless**: every
            # surviving polygon is covered exactly once, and Blender's own
            # tessellation of the quad gives back the two source triangles --
            # same corners, same UVs. A quad that came back split the other way
            # would change two face normals, and on level geometry the normal is
            # what decides walkability.
            ids = shp.merge_pairs_from_wire(wire, n) if src is not None else None
            faces, _lost = shp.plan_faces(shape, ids)
            plain, _plain_lost = shp.plan_faces(shape, None)
            covered = [i for f in faces for i in f.sources]
            if sorted(covered) != sorted(f.sources[0] for f in plain):
                failures.append((rel, "fusing changed which polygons survive"))
            elif len(covered) != len(set(covered)):
                failures.append((rel, "a polygon is in two faces"))
            for face in faces:
                stats["faces planned"] += 1
                if len(face.sources) == 1:
                    continue
                stats["pairs fused into a quad"] += 1
                for tri, source in zip(_tessellate(face.verts), reversed(face.sources)):
                    poly = shape.polys[source]
                    if tuple(poly.verts) not in _tri_rotations(tri):
                        failures.append((rel, "quad %r does not tessellate back to %r"
                                         % (face.verts, poly.verts)))
                        break
                    want = shape.uvs_for(poly)
                    if want is None:
                        if face.uvs is not None:
                            failures.append((rel, "quad invented UVs"))
                            break
                        continue
                    at = dict(zip(face.verts, face.uvs))
                    if any(at[v] != uv for v, uv in zip(poly.verts, want)):
                        failures.append((rel, "quad %r moved a UV" % (face.verts,)))
                        break
                else:
                    stats["quads that tessellate back exactly"] += 1
            stats["pairs the wire holds"] += sum(
                1 for k, v in collections.Counter(ids or ()).items()
                if k != shp.MERGE_NONE and v == 2)

            # (4) normal direction
            if shape.poly_normals and len(shape.poly_normals) == len(shape.polys):
                for p, ref in zip(shape.polys, shape.poly_normals):
                    mine = shp.face_normal(shape.verts, p.verts)
                    if mine == (0.0, 0.0, 0.0) or ref == (0.0, 0.0, 0.0):
                        degenerate += 1
                        continue
                    rn = math.sqrt(sum(c * c for c in ref))
                    if rn == 0.0:
                        degenerate += 1
                        continue
                    dots.append(sum(a * b / rn for a, b in zip(mine, ref)))

    print("shapes                        : %d" % stats["shapes"])
    print("polygons                      : %d (%d with usable UVs)"
          % (stats["polys"], stats["polys with uvs"]))
    print("geometry chunks re-encoded    : %d exact" % stats["geometry chunks re-encoded exactly"])
    print("uv indices re-encoded         : %d exact" % stats["uv indices re-encoded exactly"])
    print("   in a >16-bit UV table      : %d polys, %d indexed by their own position"
          % (stats["polys in a >16-bit UV table"],
             stats["... whose uv index is its own position"]))
    print("SHPCENTR regenerated          : %d centre exact, %d byte-exact"
          % (stats["SHPCENTR centre exact"], stats["SHPCENTR byte-exact"]))
    print("SHPMRGDT                      : %d walkable as shipped, %d exact through pair ids"
          % (stats["SHPMRGDT valid as shipped"],
             stats["SHPMRGDT round-trips through pair ids"]))
    print("   control (must be non-zero) : %d synthetic breakages detected"
          % _control())
    print("pairs fused into a quad       : %d of %d (%.2f%%), %d tessellate back exactly"
          % (stats["pairs fused into a quad"], stats["pairs the wire holds"],
             100.0 * stats["pairs fused into a quad"] / max(stats["pairs the wire holds"], 1),
             stats["quads that tessellate back exactly"]))
    print("   faces planned              : %d" % stats["faces planned"])
    print("   control (must be non-zero) : %d unfusable pairs refused" % _fuse_control())
    if radius_deltas:
        near = sum(1 for d in radius_deltas if d <= 1e-2)
        print("   radius vs shipped          : %d within 0.01, max delta %.3f (not asserted)"
              % (near, max(radius_deltas)))
    if dots:
        aligned = sum(1 for d in dots if d > 0.99)
        opposed = sum(1 for d in dots if d < -0.99)
        print("face normals vs SHPPNORM      : %d compared" % len(dots))
        print("   aligned  (dot > +0.99)     : %d (%.2f%%)"
              % (aligned, 100.0 * aligned / len(dots)))
        print("   opposed  (dot < -0.99)     : %d (%.2f%%)"
              % (opposed, 100.0 * opposed / len(dots)))
        print("   neither                    : %d" % (len(dots) - aligned - opposed))
        print("   degenerate skipped         : %d" % degenerate)
    print("failures                      : %d" % len(failures))
    for rel, why in failures[:15]:
        print("    %-45s %s" % (rel, why))
    if len(failures) > 15:
        print("    ... and %d more" % (len(failures) - 15))
    # The controls have to fire, or the SHPMRGDT and fusion checks are vacuous.
    return 1 if failures or _control() != 4 or _fuse_control() != 5 else 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
