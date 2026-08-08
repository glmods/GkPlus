"""Author a cutscene in Blender from nothing, and read it back off the wire.

    blender --background --python blender/tests/test_cutscene_authoring.py -- ["<Gunlok dir>"]

The other cutscene tests prove an existing one survives a round trip. This one
proves the opposite direction: that the addon can *create* a cutscene the engine
could play, with no source file anywhere. What "could play" means is not a
matter of opinion -- four things are load-bearing and each is checked against the
chunk stream, not against the scene:

* a `CUTSCDAT` whose name hash matches the name, since `PLAY CUTSCENE` finds a
  cutscene by that name and the hash is derived from it;
* two participants, one with `is_camera == 0` (the camera position) and one with
  bit 0 of `flags` (what it looks at) -- the camera is not a chunk type;
* a `CUTPOINT` per track whose control points are the keyframes, in rif units;
* a control event, without which the cutscene never ends and the player is left
  with the camera locked.

Passing a Gunlok directory also re-imports a shipped level and edits it, which
is the case where a new keyframe has to interleave with authored ones.
"""

import os
import sys
import tempfile

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "io_scene_rif"))

import cutscene as cut  # noqa: E402
import rif  # noqa: E402
import schema  # noqa: E402
from io_scene_rif import scene as sc  # noqa: E402

FAILURES = []
PASSES = [0]


def check(cond, msg):
    if cond:
        PASSES[0] += 1
    else:
        print("  FAIL " + msg)
        FAILURES.append(msg)


def find_all(chunk, cid, out=None):
    out = [] if out is None else out
    for kid in (chunk.children or ()):
        if kid.id == cid:
            out.append(kid)
        find_all(kid, cid, out)
    return out


def export(collection):
    root, _stats = sc.rebuild_tree(collection)
    path = os.path.join(tempfile.gettempdir(), "cut_author.rif")
    rif.save(path, root)
    out = rif.load(path)
    os.remove(path)
    return out


def blend_round_trip(name):
    """Save, reset Blender, reopen -- the scene must stand on its own."""
    path = os.path.join(tempfile.gettempdir(), "cut_author.blend")
    bpy.ops.wm.save_as_mainfile(filepath=path)
    bpy.ops.wm.read_homefile(use_empty=True)
    bpy.ops.wm.open_mainfile(filepath=path)
    os.remove(path)
    return bpy.data.collections.get(name)


def author_from_scratch():
    print("authoring a cutscene in an empty file")
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = sc.new_collection("authored.rif")

    root = sc.add_cutscene(collection, "test scene")
    check(root is not None, "add_cutscene made a cutscene")

    # Key a path on the camera track: a square, one second between points.
    parts = [o for o in collection.objects
             if o.get("rif_cut_role") == sc.CUT_PARTICIPANT]
    check(len(parts) == 2, "a new cutscene has two participants, got %d" % len(parts))
    cam_track = None
    for part in parts:
        for child in part.children:
            if child.type == "CAMERA":
                cam_track = child
    check(cam_track is not None, "one of them carries a Camera")
    if cam_track is None:
        return None, None

    path = [(0, 0, 2), (4, 0, 2), (4, 4, 2), (0, 4, 2)]
    for i, loc in enumerate(path):
        cam_track.location = loc
        cam_track.keyframe_insert("location", frame=i * cut.FPS)

    sc.add_cutscene_event(root, "CONSOLE", "SAY authored by Blender", 1.5)
    made = sc.preview_cutscene_path(root)
    check(made >= 1, "the preview drew %d path(s)" % made)

    collection = blend_round_trip(collection.name)
    check(collection is not None, "the collection survived a .blend round trip")
    return collection, path


def check_wire(tree, path, scale):
    """Everything that decides whether the engine could play this."""
    heads = find_all(tree, b"CUTSHEAD")
    check(len(heads) == 1, "exactly one CUTSHEAD was written, got %d" % len(heads))
    if not heads:
        return

    data = find_all(tree, b"CUTSCDAT")
    check(len(data) == 1, "exactly one CUTSCDAT")
    props = schema.decode(b"CUTSCDAT", data[0].body)
    check(props["name"] == "test scene",
          "the cutscene name reached the file: %r" % props["name"])
    check(list(props["name_hash"]) == schema.cutscene_name_hash("test scene"),
          "its id is the hash of that name, so PLAY CUTSCENE can match it")

    users = [schema.decode(b"CTUSRDAT", c.body)
             for c in find_all(tree, b"CTUSRDAT")]
    check(sum(1 for u in users if u["is_camera"] == 0) == 1,
          "exactly one participant is the camera position (is_camera == 0)")
    check(sum(1 for u in users if u["flags"] & 1) == 1,
          "exactly one is the look-at target (flags bit 0)")

    points = [schema.decode(b"CUTPOINT", c.body)
              for c in find_all(tree, b"CUTPOINT")]
    check(len(points) == 2, "one CUTPOINT per track, got %d" % len(points))
    keyed = max(points, key=lambda p: len(p["points"]))
    got = [tuple(keyed["points"][k:k + 3]) for k in range(0, len(keyed["points"]), 4)]
    want = [sc.to_rif(p, scale, True) for p in path]
    check(got == want, "the keyframes became the control points, in rif units:\n"
                       "      got  %s\n      want %s" % (got, want))

    times = [schema.point_time_ms(keyed["points"][k * 4 + 3])
             for k in range(len(got))]
    check(times[:-1] == [1000] * (len(got) - 1),
          "one second between keys became 1000 ms per interval: %s" % times)

    events = [schema.decode(b"CUTEVENT", c.body) for c in find_all(tree, b"CUTEVENT")]
    kinds = [k for e in events for k in e["kinds"]]
    check(cut.EVENT_CONTROL in kinds,
          "a control event was written, so the cutscene can end: %s" % kinds)
    check(cut.EVENT_CONSOLE in kinds, "the console event was written too")
    console = next((e for e in events if cut.EVENT_CONSOLE in e["kinds"]), None)
    check(console is not None and console["strings"] == "SAY authored by Blender",
          "the console command survived: %r"
          % (console["strings"] if console else None))


def edit_shipped(game_dir):
    """Insert a keyframe into a shipped path and confirm the rest is untouched."""
    src = os.path.join(game_dir, "RIF", "Levels", "level01.RIF")
    if not os.path.exists(src):
        print("  (no level01.RIF, skipping the edit case)")
        return
    print("editing a shipped cutscene: level01.RIF")
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, _ = sc.build_scene(rif.load(src), "level01.RIF", source_path=src)

    roots = sc.cutscene_roots(collection)
    check(len(roots) == 4, "level01 imported 4 cutscenes, got %d" % len(roots))
    names = sorted(r.get("rif_cut_name", "") for r in roots)
    check(names == ["end_of_level", "escape intro", "first contact", "the bug"],
          "with their shipped names: %s" % names)

    target = None
    for root in roots:
        for part in root.children:
            for track in part.children:
                if len(sc.track_frames(track)) >= 3:
                    target = target or track
    check(target is not None, "found a track with at least three control points")
    if target is None:
        return

    before = len(sc.track_frames(target))
    frames = sc.track_frames(target)
    new_frame = frames[0] + 1
    check(new_frame not in frames, "the inserted frame is genuinely new")
    target.location = (1.0, 2.0, 3.0)
    target.keyframe_insert("location", frame=new_frame)

    tree = export(collection)
    counts = len(find_all(tree, b"CUTPOINT"))
    check(counts == 34, "level01 still writes 34 CUTPOINT chunks, got %d" % counts)

    after = len(sc.track_frames(target))
    check(after == before + 1,
          "the edited track gained exactly one control point (%d -> %d)"
          % (before, after))


def main(game_dir):
    collection, path = author_from_scratch()
    if collection is not None:
        scale = collection.get("rif_scale", sc.DEFAULT_SCALE)
        check_wire(export(collection), path, scale)

    if game_dir:
        edit_shipped(game_dir)

    # The wire checks must be able to fail.
    before = len(FAILURES)
    check(schema.cutscene_name_hash("test scene")
          == schema.cutscene_name_hash("other scene"),
          "control: two different names must not share a hash")
    fired = len(FAILURES) - before
    print("\ncontrol fired: %s" % bool(fired))
    for _ in range(fired):
        FAILURES.pop()

    print("\n%d checks passed" % PASSES[0])
    if FAILURES:
        print("%d CHECK(S) FAILED" % len(FAILURES))
        for f in FAILURES:
            print("   %s" % f)
    else:
        print("all checks passed")
    return 1 if FAILURES or not fired else 0


if __name__ == "__main__":
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    sys.exit(main(argv[0] if argv else ""))
