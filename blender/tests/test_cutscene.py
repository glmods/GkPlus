"""The cutscene chunk codecs, and the constraint their shape has to satisfy.

    python blender/tests/test_cutscene.py "<Gunlok dir>"

``test_schema.py`` already requires ``encode(decode(body)) == body`` for every
leaf chunk, so it covers these too. What it cannot see is the reason these are
parallel flat arrays rather than the list of records the format describes: a
decoded value has to survive Blender's ID properties, where
``scene._unpack_absorbed`` converts each one with a single ``list(val)``. A
nested list, or a list mixing ints with floats, round-trips fine in pure Python
and only fails once a ``.blend`` is involved -- which is exactly the kind of bug
that reaches the user instead of the test suite.

So this checks the storable shape directly, plus the two helpers no round trip
exercises: the `CUTSCDAT` name hash and the packed point time.

Layouts and evidence: rif_chunk_format.md, "The cutscene chunks".
"""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "io_scene_rif"))

import cutscene as cut  # noqa: E402
import rif  # noqa: E402
import schema  # noqa: E402

FAILURES = []
PASSES = [0]

CUTSCENE_IDS = (b"CUTSCDAT", b"CTUSRDAT", b"CTUSRHIE", b"CUTTRNAM",
                b"CTUSNDPR", b"CTUSSPPO", b"CUTPOINT", b"CUTEVENT",
                b"CUTTRFOV")
CONTAINERS = (b"CUTSHEAD", b"CUTSCUSR", b"CUTTRACK")


def check(cond, msg):
    if cond:
        PASSES[0] += 1
    else:
        print("  FAIL " + msg)
        FAILURES.append(msg)


def id_prop_safe(val):
    """Can Blender store this, and does ``list(val)`` bring it back unchanged?

    Allowed: a str, an int, a float, or a flat list whose entries are all int or
    all float. Bools are ints. Anything else -- a nested list, a tuple of tuples,
    a list mixing the two -- is not.
    """
    if isinstance(val, (str, int, float)):
        return True
    if not isinstance(val, list):
        return False
    if not val:
        return True  # an empty array is legal and occurs (a 0-record CUTEVENT)
    kinds = {type(v) for v in val}
    return kinds in ({int}, {float})


def cutscene_chunks(game_dir):
    for dp, _, ns in os.walk(game_dir):
        for nm in sorted(ns):
            if not nm.lower().endswith(".rif"):
                continue
            path = os.path.join(dp, nm)
            for c in rif.load(path).walk():
                if c.id in CUTSCENE_IDS or c.id in CONTAINERS:
                    yield os.path.basename(path), c


def absorbed_entries(root):
    """The ``[(path, props)]`` form ``scene._absorb`` would produce, without bpy.

    Only leaves get an entry; a container is implied by its children's paths,
    which is exactly what ``scene._emit_from`` rebuilds from.
    """
    out = []

    def walk(chunk, prefix):
        for i, kid in enumerate(chunk.children or ()):
            path = "%s%s:%d" % (prefix, kid.name, i)
            if kid.children is None:
                out.append((path, schema.decode(kid.id, kid.body)))
            else:
                walk(kid, path + "/")

    walk(root, "")
    return out


def check_model(game_dir):
    """Parse every shipped cutscene into the model and emit it back."""
    scenes = files = 0
    mismatch = []
    for dp, _, ns in os.walk(game_dir):
        for nm in sorted(ns):
            if not nm.lower().endswith(".rif"):
                continue
            root = rif.load(os.path.join(dp, nm))
            entries = absorbed_entries(root)
            mine, _rest = cut.split_absorbed(entries)
            if not mine:
                continue
            files += 1
            parsed = cut.parse(entries)
            scenes += len(parsed)

            want = {path: schema.encode(
                path.rpartition("/")[2].rpartition(":")[0].encode("latin-1"),
                props) for path, props in mine}
            got = {path: chunk.body for path, chunk in cut.emit(parsed)}
            if set(want) != set(got):
                only = sorted(set(want) ^ set(got))[:3]
                mismatch.append("%s: path sets differ, e.g. %s" % (nm, only))
                continue
            for path in want:
                if want[path] != got[path]:
                    mismatch.append("%s: %s body differs" % (nm, path))
                    break

    check(files and not mismatch,
          "model parse->emit reproduces every cutscene chunk in %d files "
          "(%d cutscenes); %d problem(s): %s"
          % (files, scenes, len(mismatch), mismatch[:3]))
    return scenes


def check_helpers():
    """Time conversion and the spline, on shapes no shipped file has to cover."""
    frames = [0, 3, 10, 11]
    durations = cut.durations_from_frames(frames, final_ms=0)
    check(durations == [120, 280, 40, 0],
          "durations_from_frames turns frame gaps into ticks: %s" % durations)

    pts = cut.pack_points([(1, 2, 3), (4, 5, 6), (7, 8, 9), (0, 0, 0)], frames)
    track = cut.Track()
    track.points = pts
    check(cut.point_frames(track) == frames,
          "point_frames inverts pack_points: %s" % cut.point_frames(track))

    # The phantom rule: the head reflects point 1 through point 0.
    q = cut.control_points([(0, 0, 0), (10, 0, 0), (20, 0, 0)])
    check(q[0] == (-10, 0, 0) and q[-1] == (30, 0, 0),
          "control_points reflects both ends: %s .. %s" % (q[0], q[-1]))
    check(len(cut.sample_path([(0, 0, 0), (10, 0, 0)], per_segment=4)) == 5,
          "sample_path walks every segment plus the final point")

    # A straight line stays straight under the spline, which is the property a
    # preview would visibly break if the basis were wrong.
    line = [(0, 0, 0), (10, 0, 0), (20, 0, 0), (30, 0, 0)]
    mid = cut.sample_segment(*cut.control_points(line)[0:4], 0.5)
    check(abs(mid[0] - 5.0) < 1e-9 and abs(mid[1]) < 1e-9,
          "Catmull-Rom keeps a straight line straight: %s" % (mid,))

    cs = cut.new_cutscene("test scene", prefix="REBENVDT:1/SPECLOBJ:0")
    check(cs.camera_position_track() is not None
          and cs.camera_target_track() is not None,
          "new_cutscene makes both halves of the camera")
    cam = cs.camera_position_track()
    check(any(props["kinds"] == [cut.EVENT_CONTROL]
              and props["payload"][0] == cut.CONTROL_END
              for _i, props in cam.tracks[0].events),
          "new_cutscene emits the end event, without which a cutscene never ends")


def main(game_dir):
    counts = {}
    unsafe = []
    times = []
    packed_ok = 0
    hashes = 0
    hash_total = 0
    kinds_seen = set()
    empty_containers = [0, 0]

    for fn, chunk in cutscene_chunks(game_dir):
        counts[chunk.id] = counts.get(chunk.id, 0) + 1

        if chunk.id in CONTAINERS:
            empty_containers[0] += 1
            empty_containers[1] += (len(chunk.body) == 0)
            continue

        props = schema.decode(chunk.id, chunk.body)
        if schema.encode(chunk.id, props) != chunk.body:
            check(False, "%s in %s: does not re-encode" % (chunk.id.decode(), fn))

        for key in props:
            if not id_prop_safe(props[key]):
                unsafe.append("%s.%s = %r" % (chunk.id.decode(), key,
                                              type(props[key])))

        if chunk.id == b"CUTSCDAT":
            hash_total += 1
            hashes += (schema.cutscene_name_hash(props["name"])
                       == list(props["name_hash"]))

        if chunk.id == b"CUTPOINT":
            pts = props["points"]
            for k in range(0, len(pts), 4):
                packed = pts[k + 3]
                ms = schema.point_time_ms(packed)
                times.append(ms)
                spare = (packed >> 24) & 0xff
                packed_ok += (schema.pack_point_time(ms, spare) == packed)

        if chunk.id == b"CUTEVENT":
            kinds_seen.update(props["kinds"])

    print("chunks seen:")
    for cid in sorted(counts):
        print("   %-8s %4d" % (cid.decode(), counts[cid]))
    print()

    check(not unsafe,
          "every decoded value is storable as an ID property (%d offenders: %s)"
          % (len(unsafe), unsafe[:4]))
    check(empty_containers[0] == empty_containers[1],
          "all %d container bodies are empty" % empty_containers[0])
    check(hash_total and hashes == hash_total,
          "cutscene_name_hash reproduces the stored id in %d/%d CUTSCDAT"
          % (hashes, hash_total))
    check(times and all(t % 40 == 0 for t in times),
          "all %d point times are a multiple of the 40 ms tick" % len(times))
    check(packed_ok == len(times),
          "pack_point_time inverts point_time_ms in %d/%d points"
          % (packed_ok, len(times)))
    check(kinds_seen and kinds_seen <= set(range(1, 14)),
          "CUTEVENT kinds are all in 1..13: %s" % sorted(kinds_seen))
    check(kinds_seen >= schema.EVENT_STRING_KINDS,
          "every string-carrying kind %s actually ships (seen %s)"
          % (sorted(schema.EVENT_STRING_KINDS), sorted(kinds_seen)))

    check_model(game_dir)
    check_helpers()

    # Three controls. A check nothing can violate proves nothing.
    before = len(FAILURES)
    body = (struct.pack("<I", 1) + struct.pack("<3iI", 1, 2, 3, 40)
            + struct.pack("<12i", *([0] * 12)))
    mutated = schema.decode(b"CUTPOINT", body)
    mutated["points"][3] += 40
    check(schema.encode(b"CUTPOINT", mutated) == body,
          "control: a mutated CUTPOINT must not re-encode equal")
    check(id_prop_safe([1, 2.0]),
          "control: a mixed int/float list must be rejected as unsafe")
    # The name hash is regenerated on emit, so renaming must change it. If this
    # ever passes, a renamed cutscene is shipping an id the engine will not match.
    renamed = cut.new_cutscene("a", prefix="P:0")
    renamed.name = "b"
    emitted = schema.decode(b"CUTSCDAT", cut.emit([renamed])[0][1].body)
    check(emitted["name_hash"] == schema.cutscene_name_hash("a"),
          "control: a renamed cutscene must NOT keep its old name hash")
    fired = len(FAILURES) - before
    print("\ncontrols fired: %d of 3" % fired)
    for _ in range(fired):
        FAILURES.pop()

    print("\n%d checks passed" % PASSES[0])
    if FAILURES:
        print("%d CHECK(S) FAILED" % len(FAILURES))
    else:
        print("all checks passed")
    return 1 if FAILURES or fired != 2 else 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
