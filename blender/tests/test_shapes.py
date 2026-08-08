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
    # The control has to fire, or the SHPMRGDT checks above are vacuous.
    return 1 if failures or _control() != 4 else 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
