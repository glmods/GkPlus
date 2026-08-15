"""Import to a scene, export from the scene alone, compare semantically.

    blender --background --python blender/tests/test_scene.py -- "<Gunlok dir>" [N|all]

Byte-exactness is explicitly not the bar. What has to hold is that the file coming
out means the same thing as the file going in: the same objects with the same
transforms, each with the same geometry, the same per-face and per-vertex data,
the same lights, the same animation, and the same textures.

The important part is the second phase: the scene is saved to a .blend, Blender is
reset, the .blend is reopened, and the export runs from that with the source file
never touched. If the scene were not self-contained that phase is what fails --
which is exactly what the texture checks are watching for, since a Blender image
filled through ``pixels`` rather than packed comes back from that round trip
blank.

The texture table *is* held to byte-exactness, unlike everything else here: it is
carried whole rather than regenerated, so an import/export cycle that touches no
material has no reason to disturb a single byte of it.
"""

import array
import collections
import os
import struct
import sys
import tempfile

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "io_scene_rif"))

import bmpnames  # noqa: E402
import heads  # noqa: E402
import rif  # noqa: E402
import rim  # noqa: E402
import schema  # noqa: E402
import shapes as shp  # noqa: E402
from io_scene_rif import scene as sc  # noqa: E402

FAILURES = []
PASSES = [0]


def check(cond, msg):
    if cond:
        PASSES[0] += 1
    else:
        print("  FAIL " + msg)
        FAILURES.append(msg)


def _uv_key(shape, poly):
    """This polygon's texture and UVs, as a rotation-independent key.

    **Which corner a polygon starts at is not meaning.** A triangle is the same
    triangle read from any of its three corners -- same winding, same surface,
    same UV on every vertex -- and the exporter does not preserve the source's
    choice: a fused quad tessellates back into its two triangles starting at the
    shared edge, wherever they began in the file. So the corners are rotated to
    start at the lowest **vertex index**, which is exact (no float tie can pick
    a different anchor) and still tells a genuine winding reversal apart, since
    reversing is not a rotation.
    """
    uvs = shape.uvs_for(poly) or ()
    if len(uvs) == len(poly.verts) and uvs:
        k = poly.verts.index(min(poly.verts))
        uvs = tuple(uvs[k:]) + tuple(uvs[:k])
    return (poly.texture_index,) + tuple(c for uv in uvs for c in uv)


def _merge_pairs(chunk, num_polys):
    """How many polygon pairs this shape's ``SHPMRGDT`` actually names."""
    merge = chunk.find(b"SHPMRGDT")
    if merge is None:
        return 0
    wire = list(struct.unpack("<%di" % (len(merge.body) // 4), merge.body))
    return sum(1 for pid in shp.merge_pairs_from_wire(wire, num_polys)
               if pid != shp.MERGE_NONE) // 2


def summarize(root):
    """The meaning of a file, as a comparable structure."""
    kids = list(root.children or ())
    pairs = sc._shape_pairs(root)

    objects = []
    for i, c in enumerate(kids):
        if c.id != b"RBOBJECT":
            continue
        head = c.find(b"OBJHEAD1")
        data = (list(struct.unpack_from("<%di" % (len(head.body) // 4), head.body, 0))
                if head else [])
        entry = {
            "name": sc._objhead_name(head.body if head is not None else b""),
            "location": tuple(data[sc.OH_LOCATION]) if len(data) >= 8 else None,
            "shape": None,
        }
        j = pairs.get(i)
        if j is not None:
            s = shp.read_shape(kids[j])
            if s is not None:
                # Two deliberate, disjoint losses, counted in pipeline order.
                # `collisions`: Blender cannot hold two faces on the same vertex
                # set, so import drops them. `degenerate`: export drops a
                # triangle whose corners coincide once quantized, because it
                # crashes the game's section-adjacency build (see
                # shapes.welds_degenerate) -- counted only among the faces that
                # survived import, so the two never double-count.
                welded = shp.weld_map(s.verts)
                seen = set()
                collisions = degenerate = 0
                for p in s.polys:
                    key = frozenset(p.verts)
                    if key in seen:
                        collisions += 1
                    elif shp.welds_degenerate(p.verts, welded):
                        degenerate += 1
                    seen.add(key)
                entry["shape"] = {
                    "nverts": len(s.verts),
                    "npolys": len(s.polys),
                    "collisions": collisions,
                    "degenerate": degenerate,
                    "verts": sorted(tuple(v) for v in s.verts),
                    "engine_types": collections.Counter(p.engine_type for p in s.polys),
                    "textures": collections.Counter(p.texture_index for p in s.polys),
                    # A UV is a texel coordinate scaled by the texture's size on
                    # the way in and back on the way out, so this is what proves
                    # the two halves agree.
                    #
                    # Kept at full precision and reconciled with a tolerance by
                    # `uv_residue` below. This used to round to a hundredth of a
                    # texel instead, to absorb the V-flip drift -- but rounding
                    # does not absorb drift, it only moves the cliff: a value
                    # sitting within 3e-5 of a `.xx5` boundary lands on either
                    # side of it and reads as a whole hundredth of difference.
                    # `Skeleton.RIF`'s skull has 11 such coordinates.
                    "uvs": collections.Counter(_uv_key(s, p) for p in s.polys),
                    # The merge pairing survives being built as quads and
                    # tessellated back -- pair for pair, not just "still
                    # walkable". A lost pair is a lost nav section.
                    "merge_pairs": _merge_pairs(kids[j], len(s.polys)),
                }
        objects.append(entry)
    objects.sort(key=lambda e: (e["name"], e["location"] or ()))

    lights = []
    for c in root.walk():
        if c.id != b"STDLIGHT" or len(c.body) < 84:
            continue
        p = schema.decode(b"STDLIGHT", c.body)
        lights.append((tuple(p["position"]), p["colour"], p["range"]))
    lights.sort()

    # Name, id and every frame time -- not just the frame count. Export
    # regenerates OBASEQHD and places each frame's `time` itself, so a count
    # alone would pass while the sequence was renamed, re-ided or re-timed.
    seqs = []
    for c in root.walk():
        if c.id != b"OBANSEQC":
            continue
        frames = [k for k in (c.children or ()) if k.id == b"OBASEQFR"]
        head = c.find(b"OBASEQHD")
        decoded = [schema.decode(b"OBASEQFR", k.body) for k in frames]
        times = tuple(d["time"] for d in decoded)
        # The whole flags word, so the sound index -- which is now split out of it
        # on import and spliced back on export -- has to come back bit for bit,
        # along with the residual nobody understands.
        flags = tuple(d["flags"] & 0xFFFFFFFF for d in decoded)
        # The three optional settings are now regenerated from Action properties
        # rather than carried, so which sequences have them -- and on which bones
        # -- has to come back exactly. Most carry none, and that is a state too.
        optional = tuple(bytes(c.find(cid).body) if c.find(cid) is not None else None
                         for cid in (b"OBASEQTM", b"OBASEQFL", b"OBASEQSP"))
        seqs.append((heads.seqhead_name(head.body) if head is not None else "",
                     heads.seqhead_fields(head.body)[2] if head is not None else -1,
                     len(frames), times, flags, optional))
    seqs.sort(key=lambda s: (s[0], s[1], s[2], s[3], s[4],
                             tuple(b"" if x is None else x for x in s[5])))

    # Both sound systems are held to byte-exactness, and they are counted apart
    # because they *are* apart -- an INDSOUND is a table entry an animation
    # keyframe names, a DUMOBJTX is a placement in the level. Neither is
    # regenerated from parsed values: the table is carried whole (uninitialised
    # path padding included) and the emitter text is spliced, so an export that
    # touched neither has no reason to disturb a byte of either.
    sound_bodies = sorted(bytes(c.body) for c in root.walk() if c.id == b"INDSOUND")

    # Paired with the dummy's own name and position, because an emitter that came
    # back with the right text at the wrong place is not the same file. The name
    # is the DUMOBJDT trailing string; the position is its int32 triple.
    emitters = []
    for c in root.walk():
        if c.id != b"DUMMYOBJ":
            continue
        tx = c.find(b"DUMOBJTX")
        if tx is None:
            continue
        dt = c.find(b"DUMOBJDT")
        emitters.append((
            sc._trailing_name(dt.body, sc.DUMOBJDT_NAME) if dt is not None else "",
            tuple(struct.unpack_from("<3i", dt.body, sc.DUMOBJ_LOCATION))
            if dt is not None else None,
            bytes(tx.body),
        ))
    emitters.sort()

    # OBJHIERD is regenerated from the object each node drives, so the bindings
    # are what proves a rename followed the rig instead of breaking it.
    bindings = sorted(heads.objhierd_name(c.body)
                      for c in root.walk() if c.id == b"OBJHIERD")

    # `SHPMRGDT` names the polygon each polygon pairs with, and the engine's
    # merge pass walks it with no bounds check at all -- so a table that is not
    # an exact involution is an out-of-bounds heap write during level load. This
    # is the check that catches a chunk being written from stale indices, which
    # is what crashed 15 of the 24 shipped levels before the pair-id form.
    unwalkable = []
    for c in root.walk():
        if c.id not in (b"REBSHAPE", b"SUBSHAPE"):
            continue
        merge, polys = c.find(b"SHPMRGDT"), c.find(b"SHPPOLYS")
        if merge is None or polys is None:
            continue
        wire = list(struct.unpack("<%di" % (len(merge.body) // 4), merge.body))
        why = shp.merge_problems(wire, len(polys.body) // 36)
        if why:
            unwalkable.append(why[0])

    table = next((c for c in root.walk() if c.id == b"BMPNAMES"), None)
    return {"objects": objects, "lights": lights, "sequences": seqs,
            "bindings": bindings, "sounds": sound_bodies, "emitters": emitters,
            "unwalkable_merge": unwalkable,
            "table": table.body if table is not None else None,
            "inventory": collections.Counter(c.id for c in root.walk())}


#: How far a UV coordinate may move through an import/export cycle before it
#: counts as a difference, in texels. The V flip is the only inexact step --
#: `1 - v/h` moves a fractional value to an exponent where float32 has fewer bits
#: left for it, while the scaling either side is exact because every texture in
#: the game is a power of two. Measured worst case across the shipped files is
#: **3.05e-05** texels (median 1.9e-05), so this is 30x headroom and still a
#: thousandth of a pixel.
UV_TOLERANCE = 1e-3


def uv_residue(got_uvs, want_uvs):
    """The UV sets in ``got`` that no set in ``want`` accounts for.

    Exact matches cancel in the multiset difference, which is nearly all of them.
    Whatever is left is then paired off against the *lost* sets within
    :data:`UV_TOLERANCE` -- so a polygon whose V drifted by a hundred-thousandth
    of a texel cancels, and one that genuinely gained a UV does not.

    Rounding the keys instead, which is what this replaced, cannot do that: it
    buys a tolerance everywhere except at the bucket boundaries, which is exactly
    where a small drift shows up.
    """
    gained = got_uvs - want_uvs
    if not gained:
        return gained
    lost = want_uvs - got_uvs
    out = collections.Counter()
    for key, count in gained.items():
        for other in list(lost):
            if lost[other] <= 0 or len(other) != len(key) or other[0] != key[0]:
                continue
            if all(abs(x - y) <= UV_TOLERANCE for x, y in zip(key[1:], other[1:])):
                take = min(count, lost[other])
                count -= take
                lost[other] -= take
                if not count:
                    break
        if count:
            out[key] = count
    return out


#: Chunk ids whose count is *expected* to change across a round trip, with why.
#: Everything else has to come back with exactly the same number of chunks --
#: a check that costs nothing and that the semantic comparisons cannot make,
#: since they walk the objects a file describes rather than the chunks in it.
#: `DUMOBJDT` was emitted twice per dummy for as long as this test existed
#: (6,847 across the shipped set) and every check here passed regardless: the
#: duplicate carried the same meaning, so nothing that compares meaning could
#: see it.
INVENTORY_EXPECTED = {
    # Discarded on purpose: 681 of 9,357 shipped shapes have none and every AvP
    # path guards the lookup, so it is regenerable-by-absence.
    b"SHPPCINF": "discarded deliberately",
}


def compare_inventory(name, want, got):
    ids = set(want["inventory"]) | set(got["inventory"])
    for cid in sorted(ids):
        a, b = want["inventory"][cid], got["inventory"][cid]
        if a == b or cid in INVENTORY_EXPECTED:
            continue
        check(False, "%s: %s count %d in, %d out"
              % (name, cid.decode("latin-1").rstrip("\0"), a, b))


def compare(name, want, got):
    compare_inventory(name, want, got)
    # The engine's merge pass has no bounds check, so this one is about not
    # writing a file that crashes the game rather than about fidelity. The
    # source is checked too, so a shipped file that violated it would be
    # reported as such rather than blamed on the exporter.
    check(not want["unwalkable_merge"],
          "%s: source SHPMRGDT is walkable (%s)"
          % (name, (want["unwalkable_merge"] or [""])[0]))
    check(not got["unwalkable_merge"],
          "%s: exported SHPMRGDT is walkable -- the engine's merge pass has no "
          "bounds check (%s)" % (name, (got["unwalkable_merge"] or [""])[0]))
    check(len(want["objects"]) == len(got["objects"]),
          "%s: %d objects out, %d in" % (name, len(got["objects"]), len(want["objects"])))
    for a, b in zip(want["objects"], got["objects"]):
        if a["name"] != b["name"]:
            check(False, "%s: object %r vs %r" % (name, b["name"], a["name"]))
            break
        if a["location"] != b["location"]:
            check(False, "%s: %s at %r, expected %r"
                  % (name, a["name"], b["location"], a["location"]))
            break
        sa, sb = a["shape"], b["shape"]
        if (sa is None) != (sb is None):
            check(False, "%s: %s shape presence differs" % (name, a["name"]))
            break
        if sa and (sa["nverts"] != sb["nverts"] or sa["verts"] != sb["verts"]):
            check(False, "%s: %s geometry differs (%d verts vs %d)"
                  % (name, a["name"], sb["nverts"], sa["nverts"]))
            break
        if sa and sb["npolys"] != sa["npolys"] - sa["collisions"] - sa["degenerate"]:
            check(False, "%s: %s has %d polys, expected %d (%d source - %d collisions "
                         "- %d degenerate)"
                  % (name, a["name"], sb["npolys"],
                     sa["npolys"] - sa["collisions"] - sa["degenerate"],
                     sa["npolys"], sa["collisions"], sa["degenerate"]))
            break
        # Nothing invented: every polygon that came back must have been in the
        # source, allowing for the faces Blender could not represent.
        if sa and (sb["engine_types"] - sa["engine_types"]):
            check(False, "%s: %s gained engine_types %r"
                  % (name, a["name"], dict(sb["engine_types"] - sa["engine_types"])))
            break
        if sa and (sb["textures"] - sa["textures"]):
            check(False, "%s: %s gained texture indices %r"
                  % (name, a["name"], dict(sb["textures"] - sa["textures"])))
            break
        # A shape that lost no face must come back with every merge pair it had.
        # A quad *is* a pair on the way out, so this is what says the fusion is
        # reversible rather than merely safe -- and a pair only goes missing
        # legitimately when the face it named went with it.
        if sa and not (sa["collisions"] or sa["degenerate"]):
            if sa["merge_pairs"] != sb["merge_pairs"]:
                check(False, "%s: %s exports %d merge pair(s), had %d"
                      % (name, a["name"], sb["merge_pairs"], sa["merge_pairs"]))
                break
        elif sa and sb["merge_pairs"] > sa["merge_pairs"]:
            check(False, "%s: %s invented %d merge pair(s)"
                  % (name, a["name"], sb["merge_pairs"] - sa["merge_pairs"]))
            break
        gained_uvs = uv_residue(sb["uvs"], sa["uvs"]) if sa else collections.Counter()
        if gained_uvs:
            sample = next(iter(gained_uvs))
            check(False, "%s: %s has %d UV set(s) that were not in the source, e.g. %r"
                  % (name, a["name"], sum(gained_uvs.values()), sample[:7]))
            break
    check(want["lights"] == got["lights"],
          "%s: %d lights match (%d in)" % (name, len(got["lights"]), len(want["lights"])))
    check(want["sequences"] == got["sequences"],
          "%s: sequences match by name, id, frame count, every frame time, every "
          "flags word and all three optional chunks (%d sequences)"
          % (name, len(want["sequences"])))
    check(want["sounds"] == got["sounds"],
          "%s: %d INDSOUND entr(ies) rebuild byte for byte" % (name, len(want["sounds"])))
    check(want["emitters"] == got["emitters"],
          "%s: %d ambient emitter(s) keep their name, position and DUMOBJTX text "
          "byte for byte" % (name, len(want["emitters"])))
    check(want["bindings"] == got["bindings"],
          "%s: %d hierarchy binding(s) match" % (name, len(want["bindings"])))
    check((want["table"] is None) == (got["table"] is None),
          "%s: texture table presence differs" % name)
    if want["table"] is not None and got["table"] is not None:
        check(want["table"] == got["table"],
              "%s: texture table rebuilt byte for byte (%d bytes out, %d in)"
              % (name, len(got["table"]), len(want["table"])))


def check_textures(name, collection, source_table):
    """The scene's own half: materials name their texture, images survived."""
    if source_table is None:
        return
    _version, entries = bmpnames.decode(source_table)
    by_index = {e["index"]: e["name"] for e in entries}

    seen = set()
    for obj in collection.objects:
        materials = getattr(obj.data, "materials", None) or ()
        for mat in materials:
            if mat is None or "rif_texture_index" not in mat or mat.name in seen:
                continue
            seen.add(mat.name)
            index = int(mat["rif_texture_index"])
            if index not in by_index:
                continue  # the 0xfff sentinel, or a _shadow mesh's junk index
            check(mat.get("rif_bmp_name") == by_index[index],
                  "%s: material %s names %r, table says %r"
                  % (name, mat.name, mat.get("rif_bmp_name"), by_index[index]))

    packed = [i for i in bpy.data.images if "rif_rim_path" in i]
    for image in packed:
        check(image.packed_file is not None, "%s: image %s is not packed" % (name, image.name))
        # Reading a pixel is the actual claim: it forces Blender to decode the
        # packed data, which is what an image filled through `pixels` instead of
        # packed cannot do after a .blend round trip -- it comes back empty.
        # (`has_data` alone proves nothing here, since a background Blender does
        # not load an image buffer until something asks for it.)
        try:
            pixel = image.pixels[0]
        except (IndexError, RuntimeError):
            pixel = None
        check(pixel is not None and image.has_data and image.size[0] > 0,
              "%s: image %s came back with no pixels" % (name, image.name))
    return len(seen), len(packed)


def pick(game_dir, limit):
    by_dir = {}
    for dp, _, ns in os.walk(game_dir):
        for nm in sorted(ns):
            if nm.lower().endswith(".rif"):
                by_dir.setdefault(os.path.basename(dp), []).append(os.path.join(dp, nm))
    if limit is None:
        return [p for g in by_dir.values() for p in g]
    out, groups, i = [], [list(g) for g in by_dir.values()], 0
    while len(out) < limit and any(groups):
        g = groups[i % len(groups)]
        if g:
            out.append(g.pop(0))
        i += 1
        if i > limit * 10:
            break
    return out


NEW_TEXTURE = "Units\\gkplus retexture test.RIM"


def check_retexture(path):
    """Retexturing is the point of exporting textures at all, so exercise it.

    Put a name the file never mentioned on one material and export: the table
    has to grow by exactly that entry, at a fresh index, and every polygon
    wearing that material has to move to it. This runs on a real file rather
    than a fixture because the interesting part is the *table*, and only a
    shipped file has one.
    """
    name = os.path.basename(path)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, _stats = sc.build_scene(rif.load(path), name, source_path=path)

    before = summarize(rif.load(path))["table"]
    check(before is not None, "%s: has a texture table to start from" % name)
    if before is None:
        return
    _v, entries = bmpnames.decode(before)
    highest = max(e["index"] for e in entries)

    target = None
    for obj in collection.objects:
        for mat in (getattr(obj.data, "materials", None) or ()):
            if mat is not None and mat.get("rif_bmp_name"):
                target = mat
                break
        if target:
            break
    check(target is not None, "%s: found a material naming a texture" % name)
    if target is None:
        return
    was = int(target["rif_texture_index"])
    target["rif_bmp_name"] = NEW_TEXTURE

    out_root, out_stats = sc.rebuild_tree(collection)
    out = summarize(out_root)
    _v2, after = bmpnames.decode(out["table"])

    check(len(after) == len(entries) + 1,
          "%s: retexture added one table entry (%d -> %d)"
          % (name, len(entries), len(after)))
    added = [e for e in after if e["name"] == NEW_TEXTURE]
    check(len(added) == 1, "%s: the new texture is in the table once" % name)
    check(out_stats["new_textures"] == 1,
          "%s: export reported one added texture (%d)" % (name, out_stats["new_textures"]))
    if not added:
        return
    check(added[0]["index"] == highest + 1,
          "%s: new entry took index %d, expected %d" % (name, added[0]["index"], highest + 1))
    check(added[0]["flags"] == bmpnames.DEFAULT_FLAGS,
          "%s: new entry carries the shipped flag value" % name)

    used = collections.Counter()
    for entry in out["objects"]:
        if entry["shape"]:
            used.update(entry["shape"]["textures"])
    check(used[added[0]["index"]] > 0,
          "%s: polygons moved onto the new texture index %d" % (name, added[0]["index"]))
    check(used[was] == 0,
          "%s: no polygon still names the index the material left (%d)" % (name, was))


def check_texture_export(path):
    """Writing the images back out, which is a second output beside the .rif.

    The three claims, in the order they can fail:

    - **An untouched image is recognised as untouched.** That is not a flag
      lookup -- it re-reads the pixels out of Blender and digests them -- so it
      only holds if ``pixels`` gives back exactly the bytes the ``.RIM`` decoded
      to, through a ``.blend`` save and reload. If Blender's colour management
      were in the way, every texture in the game would read as edited.
    - **An edited one is written**, at the path its ``BMPNAMES`` name spells,
      under the folder the export was pointed at.
    - **What lands there is what Blender held**, decoded back and compared pixel
      for pixel rather than merely being a file that exists.
    """
    name = os.path.basename(path)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc.build_scene(rif.load(path), name, source_path=path)

    # The reload is the point: an image that did not survive it has no pixels to
    # write, and would look like a texture that simply did not change.
    blend = os.path.join(tempfile.gettempdir(), "rif_texture_test.blend")
    bpy.ops.wm.save_as_mainfile(filepath=blend)
    bpy.ops.wm.read_homefile(use_empty=True)
    bpy.ops.wm.open_mainfile(filepath=blend)
    collection = bpy.data.collections.get(name)
    check(collection is not None, "%s: collection survived for the texture export" % name)
    if collection is None:
        return

    targets = [(mat, image, bmp) for mat, image, bmp in sc.texture_targets(collection)
               if image is not None]
    check(bool(targets), "%s: has a material with an image to write" % name)
    if not targets:
        return

    root = tempfile.mkdtemp(prefix="rif_tex_")
    stats = sc.write_textures(collection, root, changed_only=True)
    check(stats["written"] == 0 and not stats["failed"],
          "%s: nothing to write before an edit (%d written, %d unchanged, %s)"
          % (name, stats["written"], stats["unchanged"], stats["failed"][:1]))
    check(stats["unchanged"] == len(targets),
          "%s: all %d image(s) read back identical to their .RIM (%d did)"
          % (name, len(targets), stats["unchanged"]))

    # Edit one, the way painting would.
    _mat, image, bmp_name = targets[0]
    count = image.size[0] * image.size[1] * 4
    buf = array.array("f", bytes(4 * count))
    image.pixels.foreach_get(buf)
    buf[1::4] = array.array("f", [0.0]) * (count // 4)  # green to zero
    image.pixels.foreach_set(buf)
    image.update()

    stats = sc.write_textures(collection, root, changed_only=True)
    check(stats["written"] == 1 and not stats["failed"],
          "%s: the edited image is the only one written (%d written, %s)"
          % (name, stats["written"], stats["failed"][:1]))

    destination = os.path.join(root, bmp_name.replace("\\", os.sep))
    check(os.path.isfile(destination),
          "%s: written to %s, mirroring the game's layout"
          % (name, bmp_name.replace("\\", "/")))
    if not os.path.isfile(destination):
        return
    texture = rim.load(destination)
    _w, _h, rgba = sc.image_rgba(image)
    check(texture is not None and (texture.width, texture.height) == tuple(image.size),
          "%s: the written .RIM is %s, the image is %dx%d"
          % (name, texture and "%dx%d" % (texture.width, texture.height),
             image.size[0], image.size[1]))
    check(texture is not None and texture.rgba == rgba,
          "%s: the written .RIM holds exactly the pixels Blender had" % name)
    check(texture is not None and texture.rgba[1::4].count(0) == len(texture.rgba) // 4,
          "%s: and the edit is in it" % name)

    stats = sc.write_textures(collection, root, changed_only=False)
    check(stats["written"] == len(targets) and not stats["failed"],
          "%s: 'All' writes every named texture (%d of %d)"
          % (name, stats["written"], len(targets)))

    for dirpath, _dirs, names in os.walk(root):
        for nm in names:
            os.remove(os.path.join(dirpath, nm))
    if os.path.exists(blend):
        os.remove(blend)


def run(game_dir, limit):
    targets = pick(game_dir, limit)
    check(bool(targets), "found .rif files under %s" % game_dir)
    print("testing %d file(s)\n" % len(targets))

    tmp = tempfile.gettempdir()
    richest = (0, None)  # the most-textured file, which is the one worth writing back
    for path in targets:
        name = os.path.basename(path)
        want = summarize(rif.load(path))

        bpy.ops.wm.read_factory_settings(use_empty=True)
        # `source_path` is what lets the importer find the install's textures;
        # without it the table still imports and the images do not.
        collection, stats = sc.build_scene(rif.load(path), name, source_path=path)

        # Round-trip through a .blend, then export with no access to the source.
        blend = os.path.join(tmp, "rif_scene_test.blend")
        bpy.ops.wm.save_as_mainfile(filepath=blend)
        bpy.ops.wm.read_homefile(use_empty=True)
        bpy.ops.wm.open_mainfile(filepath=blend)

        reopened = bpy.data.collections.get(name)
        check(reopened is not None, "%s: collection survived the .blend round trip" % name)
        if reopened is None:
            continue

        textures = check_textures(name, reopened, want["table"]) or (0, 0)
        if textures[1] > richest[0]:
            richest = (textures[1], path)

        out_root, out_stats = sc.rebuild_tree(reopened)
        out_path = os.path.join(tmp, "rif_scene_out.rif")
        rif.save(out_path, out_root)

        got = summarize(rif.load(out_path))
        compare(name, want, got)

        print("  %-32s %3d obj  %3d shapes  %3d lights  %2d lost faces  "
              "%2d materials / %d table entries, %d image(s)"
              % (name, out_stats["objects"], out_stats["shapes"], out_stats["lights"],
                 stats["lost_faces"], textures[0], out_stats["textures"], textures[1]))
        for f in (blend, out_path):
            if os.path.exists(f):
                os.remove(f)

    textured = next((p for p in targets if summarize(rif.load(p))["table"]), None)
    if textured:
        print("\nretexture: %s" % os.path.basename(textured))
        check_retexture(textured)
    if richest[1]:
        # The one with the most images, not the first with a table: "writes every
        # named texture" says nothing about a file that names one.
        print("\ntexture export: %s (%d image(s))"
              % (os.path.basename(richest[1]), richest[0]))
        check_texture_export(richest[1])

    print("\n%s" % ("-" * 60))
    print("%d checks passed" % PASSES[0])
    if FAILURES:
        print("%d CHECK(S) FAILED" % len(FAILURES))
        for f in FAILURES[:20]:
            print("   %s" % f)
    else:
        print("all checks passed")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not argv:
        print(__doc__)
        sys.exit(2)
    count = None if len(argv) > 1 and argv[1] == "all" else (int(argv[1]) if len(argv) > 1 else 6)
    sys.exit(run(argv[0], count))
