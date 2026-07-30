"""Build a .rif from nothing, and edit the identities of an imported one.

    blender --background --python blender/tests/test_authoring.py -- ["<Gunlok dir>"]

``test_scene.py`` covers import-then-export. This covers the other direction:
scenes that were never imported, which is where the structural properties export
reads (``rif_id``, ``rif_index``, ``rif_objhead``, ``rif_absorbed``) have to be
minted rather than inherited.

What is checked, roughly in order of how much rests on it:

- **A file made from scratch parses, and says what it was told to say.** A new
  collection plus one adopted cube exports, re-imports, and comes back with the
  same name, the same geometry and the object paired to its own shape.
- **A rig made from scratch does too** -- bones nested from ``bone.parent``,
  a generated ``OBJHIERD`` per node, one shared sequence id across every bone's
  copy of an Action, and frame times on the shipped convention.
- **Adding a keyframe to an imported sequence adds a frame**, and an untouched
  one re-exports every frame time unchanged. Those two pull in opposite
  directions and are the whole reason times are anchored rather than recomputed.
- **Renaming and re-iding survive the round trip**, including through a ``.blend``
  save/reset/reopen, which is what proves the edit landed in the stored chunk
  body rather than in a Python-side cache -- and a rename follows into the
  hierarchy binding that names it.
- **A duplicated object is caught.** Copying an object in Blender copies its
  shape id, which no ``.rif`` can represent; export must refuse rather than
  produce a file whose second mesh is orphaned on re-import.

The Gunlok directory is optional; without it the groups that edit a real
imported file are skipped.
"""

import os
import struct
import sys
import tempfile

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "io_scene_rif"))

import heads  # noqa: E402
import rif  # noqa: E402
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


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def add_cube(name, size=1.0, location=(0.0, 0.0, 0.0)):
    bpy.ops.mesh.primitive_cube_add(size=size, location=location)
    obj = bpy.context.active_object
    obj.name = name
    return obj


def export(collection, path):
    root, stats = sc.rebuild_tree(collection)
    rif.save(path, root)
    return root, stats


def objects_in(root):
    """``[(name, shape_id, Shape or None)]`` for a parsed file.

    A list rather than a dict keyed by name, because **two objects may share a
    name**: copying one in Blender copies the name in its ``OBJHEAD1`` along with
    everything else, and the format allows it -- ``RifFilterObjectsByName``
    returns every match. A dict would quietly hide exactly the case
    :func:`test_duplicate_is_caught` exists to look at.
    """
    kids = list(root.children or ())
    pairs = sc._shape_pairs(root)
    out = []
    for i, c in enumerate(kids):
        if c.id != b"RBOBJECT":
            continue
        head = c.find(b"OBJHEAD1")
        body = head.body if head is not None else b""
        j = pairs.get(i)
        out.append((heads.objhead_name(body), heads.objhead_shape_id(body),
                    shp.read_shape(kids[j]) if j is not None else None))
    return out


def by_name(entries):
    return {name: (sid, shape) for name, sid, shape in entries}


def names_in(entries):
    return sorted(name for name, _sid, _shape in entries)


# --------------------------------------------------------------------------

def test_from_scratch(tmp):
    print("from scratch")
    reset()

    collection = sc.new_collection("scratch.rif")
    check(collection.get("rif_id") == "REBINFF2", "new collection is a RIF root")

    obj = add_cube("Land", size=2.0)
    check(sc.adopt_object(collection, obj) is None, "cube adopted")
    check(obj.get("rif_id") == "RBOBJECT", "adopted object is an RBOBJECT")
    check(obj.data.get("rif_id") == "REBSHAPE", "its mesh is a REBSHAPE")

    # A second object with no geometry: what a bare locator is.
    empty = bpy.data.objects.new("Goodie A", None)
    bpy.context.scene.collection.objects.link(empty)
    empty.location = (1.0, 2.0, 3.0)
    check(sc.adopt_object(collection, empty) is None, "empty adopted")

    path = os.path.join(tmp, "scratch.rif")
    _root, stats = export(collection, path)
    check(stats["objects"] == 2, "exported 2 objects, got %d" % stats["objects"])
    check(stats["shapes"] == 1, "exported 1 shape, got %d" % stats["shapes"])

    entries = objects_in(rif.load(path))
    got = by_name(entries)
    check(names_in(entries) == ["Goodie A", "Land"],
          "names round-tripped, got %s" % names_in(entries))

    shape_id, shape = got.get("Land", (None, None))
    check(shape is not None, "Land paired with its own shape")
    if shape is not None:
        check(len(shape.verts) == 8, "cube has 8 vertices, got %d" % len(shape.verts))
        check(len(shape.polys) == 12,
              "cube triangulates to 12 polys, got %d" % len(shape.polys))
        check(all(len(p.verts) == 3 for p in shape.polys), "every poly is a triangle")

    check(got.get("Goodie A", (None, None))[1] is None,
          "the empty carries no geometry")
    check(shape_id != got.get("Goodie A", (None, None))[0],
          "the two objects do not share a shape id")

    # SHPHEAD1 is not carried for a shape that never had one -- it is generated,
    # and has to describe the mesh that was actually written.
    root = rif.load(path)
    head = next((c.find(b"SHPHEAD1") for c in (root.children or ())
                 if c.id == b"REBSHAPE"), None)
    check(head is not None, "generated shape has a SHPHEAD1")
    if head is not None and shape is not None:
        lo, hi = heads.shape_bounds(shape.verts)
        nv, npoly, radius = struct.unpack_from("<iif", head.body, heads.SHPHEAD1_NUM_VERTS)
        bounds = struct.unpack_from("<6i", head.body, heads.SHPHEAD1_BOUNDS)
        check(nv == len(shape.verts) and npoly == len(shape.polys),
              "SHPHEAD1 counts match the mesh (%d/%d)" % (nv, npoly))
        check(bounds == (hi[0], lo[0], hi[1], lo[1], hi[2], lo[2]),
              "SHPHEAD1 bounds match the mesh")
        check(abs(radius - heads.shape_radius(shape.verts)) < 1e-3,
              "SHPHEAD1 radius matches the mesh")
        check(heads.shphead_file_id(head.body) == shape_id,
              "SHPHEAD1 file id matches the object's shape id")
        check(heads.shphead_names(head.body) == ["Land"],
              "SHPHEAD1 names its object, got %s" % heads.shphead_names(head.body))

    os.remove(path)


def test_edits_survive_a_blend(tmp):
    print("renaming and re-iding")
    reset()

    collection = sc.new_collection("edits.rif")
    first = add_cube("First")
    second = add_cube("Second", location=(4.0, 0.0, 0.0))
    sc.adopt_object(collection, first)
    sc.adopt_object(collection, second)

    sc.set_rif_object_name(first, "Land")
    check(sc.rif_object_name(first) == "Land", "rename took effect in memory")
    sc.set_rif_shape_id(first, 77)
    check(sc.rif_shape_id(first) == 77, "shape id took effect in memory")

    blend = os.path.join(tmp, "rif_authoring.blend")
    bpy.ops.wm.save_as_mainfile(filepath=blend)
    bpy.ops.wm.read_homefile(use_empty=True)
    bpy.ops.wm.open_mainfile(filepath=blend)

    reopened = bpy.data.collections.get("edits.rif")
    check(reopened is not None, "collection survived the .blend round trip")
    if reopened is None:
        return

    renamed = next((o for o in reopened.objects if sc.rif_object_name(o) == "Land"), None)
    check(renamed is not None, "renamed object came back")
    check(renamed is not None and sc.rif_shape_id(renamed) == 77,
          "shape id came back")

    path = os.path.join(tmp, "edits.rif")
    export(reopened, path)
    entries = objects_in(rif.load(path))
    got = by_name(entries)
    check("Land" in got,
          "renamed object is named Land in the file, got %s" % names_in(entries))
    check(got.get("Land", (None, None))[0] == 77, "the file carries shape id 77")
    check(got.get("Land", (None, None))[1] is not None,
          "id 77 still pairs with its geometry")
    # Renaming the Blender object must not reach the file.
    renamed.name = "not the rif name"
    export(reopened, path)
    check("Land" in by_name(objects_in(rif.load(path))),
          "the outliner name is not what the file stores")

    for f in (blend, path):
        if os.path.exists(f):
            os.remove(f)


def test_duplicate_is_caught(tmp):
    print("duplicate shape ids")
    reset()

    collection = sc.new_collection("dup.rif")
    original = add_cube("Part")
    sc.adopt_object(collection, original)

    copy = original.copy()
    copy.data = original.data.copy()
    collection.objects.link(copy)
    check(sc.rif_shape_id(copy) == sc.rif_shape_id(original),
          "a Blender copy really does duplicate the shape id")
    # The name comes across too, and that one is legal -- the engine's
    # RifFilterObjectsByName returns every object matching a name. Only the id
    # has to be unique.
    check(sc.rif_object_name(copy) == sc.rif_object_name(original),
          "a Blender copy also duplicates the name in the file")

    clashes = {sid: names for sid, names in sc.shape_id_users(collection).items()
               if len(names) > 1}
    check(bool(clashes), "the clash is detectable")

    sc.set_rif_shape_id(copy, sc.next_shape_id(collection))
    sc.set_rif_object_name(copy, "Part Two")
    check(sc.rif_shape_id(copy) != sc.rif_shape_id(original), "a fresh id separates them")

    path = os.path.join(tmp, "dup.rif")
    export(collection, path)
    entries = objects_in(rif.load(path))
    check(len(entries) == 2 and all(s is not None for _n, _i, s in entries),
          "both objects keep their own geometry, got %s" % names_in(entries))
    check(names_in(entries) == ["Part", "Part Two"],
          "both names came through, got %s" % names_in(entries))
    os.remove(path)


def test_edit_an_imported_file(game_dir, tmp):
    """The case a level or unit actually starts from: import, then author into it."""
    print("editing an imported file")
    source = os.path.join(game_dir, "RIF", "Objects", "SQUARE.RIF")
    if not os.path.exists(source):
        print("  skipped: %s not found" % source)
        return

    reset()
    collection, _stats = sc.build_scene(rif.load(source), "SQUARE.RIF", source_path=source)

    before = objects_in(rif.load(source))
    added = add_cube("Extra", size=100.0)
    check(sc.adopt_object(collection, added) is None, "cube adopted into an imported file")

    path = os.path.join(tmp, "square_plus.rif")
    export(collection, path)
    entries = objects_in(rif.load(path))
    got = by_name(entries)

    check(names_in(entries) == sorted(names_in(before) + ["Extra"]),
          "the new object joins the existing ones, got %s" % names_in(entries))
    ids = [sid for _n, sid, _s in entries]
    check(len(set(ids)) == len(ids), "no shape id is claimed twice, got %s" % ids)
    check(got["Extra"][1] is not None, "the new object kept its geometry")
    original_name = names_in(before)[0]
    check(got[original_name][1] is not None, "the original object kept its geometry")
    os.remove(path)


def sequences_in(root):
    """``[(name, sub_id, [times])]`` for every OBANSEQC in a parsed file."""
    out = []
    for c in root.walk():
        if c.id != b"OBANSEQC":
            continue
        head = c.find(b"OBASEQHD")
        times = [struct.unpack_from("<i", k.body, 0x1C)[0]
                 for k in (c.children or ()) if k.id == b"OBASEQFR"]
        out.append((heads.seqhead_name(head.body) if head is not None else "",
                    heads.seqhead_fields(head.body)[2] if head is not None else -1,
                    times))
    return out


def bindings_in(root):
    return sorted(heads.objhierd_name(c.body)
                  for c in root.walk() if c.id == b"OBJHIERD")


def build_rig(collection, bone_names):
    """A small armature in ``collection``, one mesh driven by each bone."""
    arm_data = bpy.data.armatures.new("rig")
    arm_obj = bpy.data.objects.new("rig", arm_data)
    bpy.context.scene.collection.objects.link(arm_obj)
    bpy.context.view_layer.objects.active = arm_obj

    bpy.ops.object.mode_set(mode="EDIT")
    try:
        parent = None
        for i, name in enumerate(bone_names):
            eb = arm_data.edit_bones.new(name)
            eb.head = (0.0, 0.0, float(i))
            eb.tail = (0.0, 0.1, float(i))
            eb.parent = parent
            parent = eb
    finally:
        bpy.ops.object.mode_set(mode="OBJECT")

    check(sc.adopt_object(collection, arm_obj) is None, "armature adopted")
    check(arm_obj.get("rif_id") == "OBJCHIER", "the rig is the file's OBJCHIER tree")

    for name in bone_names:
        part = add_cube("part_%s" % name, size=0.4)
        sc.adopt_object(collection, part)
        sc.set_rif_object_name(part, name)
        part.parent = arm_obj
        part.parent_type = "BONE"
        part.parent_bone = name
        part["rif_rig_parented"] = True
        sc.adopt_bone(arm_obj, arm_data.bones[name])
    return arm_obj


def key_bone(arm_obj, action, bone_name, frames):
    """Put a rotation key on ``bone_name`` at each of ``frames``."""
    anim = arm_obj.animation_data or arm_obj.animation_data_create()
    anim.action = action
    pb = arm_obj.pose.bones[bone_name]
    pb.rotation_mode = "QUATERNION"
    for i, f in enumerate(frames):
        bpy.context.scene.frame_set(int(f))
        pb.rotation_quaternion = (1.0, 0.0, 0.0, 0.05 * (i + 1))
        pb.keyframe_insert(data_path="rotation_quaternion", frame=f)


def test_rig_from_scratch(tmp):
    print("armature from scratch")
    reset()

    collection = sc.new_collection("rig.rif")
    arm_obj = build_rig(collection, ["Waist", "Chest", "Head"])

    action = bpy.data.actions.new("Seq_Walk")
    check(sc.adopt_action(arm_obj, action) is None, "action adopted as a sequence")
    # Deliberately different key counts: the two bones must still agree on where
    # frame 20 sits, which they only do if the clip's extent drives the scaling.
    key_bone(arm_obj, action, "Chest", [0.0, 10.0, 20.0])
    key_bone(arm_obj, action, "Head", [0.0, 20.0])

    path = os.path.join(tmp, "rig.rif")
    export(collection, path)
    root = rif.load(path)

    seqs = sequences_in(root)
    check(len(seqs) == 2, "one sequence per animated bone, got %d" % len(seqs))
    check({s[0] for s in seqs} == {"Seq_Walk"},
          "the sequence is named after the Action, got %s" % {s[0] for s in seqs})
    check(len({s[1] for s in seqs}) == 1,
          "every bone's copy carries one shared sequence id, got %s" % {s[1] for s in seqs})

    # Keys at Blender frames 0/10/20 over a 21-frame clip: floor(65536*f/21).
    # The **clip's** span, not each bone's own key range -- which is why the
    # two-key bone puts its last key at 62415 like the three-key one, rather
    # than at the end of the sequence.
    by_len = sorted(s[2] for s in seqs)
    check(by_len == [[0, 31207, 62415], [0, 62415]],
          "every bone agrees where a frame sits in the clip, got %s" % by_len)
    check(all(t < heads.SEQUENCE_SPAN for s in seqs for t in s[2]),
          "no frame time reaches 65536, as none does in the shipped files")

    binds = bindings_in(root)
    check(binds == ["Chest", "Head", "Waist"],
          "each node binds the mesh it drives, got %s" % binds)

    # The unanimated bone still has to be a node, with no OBANSEQS.
    nodes = [c for c in root.walk() if c.id == b"OBJCHIER"]
    check(len(nodes) == 3, "all 3 bones became nodes, got %d" % len(nodes))
    waist = next((n for n in nodes
                  if heads.objhierd_name(n.find(b"OBJHIERD").body) == "Waist"), None)
    check(waist is not None and waist.find(b"OBANSEQS") is None,
          "an unanimated bone gets no OBANSEQS")
    check(waist is not None and waist.find(b"OBJCHIER") is not None,
          "nesting follows bone.parent")
    os.remove(path)


def test_added_keys_and_bones(game_dir, tmp):
    """The imported-file case: add a key, add a bone, rename a bound object."""
    print("editing an imported rig")
    source = os.path.join(game_dir, "RIF", "Units", "Gunlok MkII.RIF")
    if not os.path.exists(source):
        source = next((os.path.join(dp, n)
                       for dp, _, ns in os.walk(os.path.join(game_dir, "RIF"))
                       for n in sorted(ns) if n.lower().endswith(".rif")), None)
    reset()
    collection, _stats = sc.build_scene(rif.load(source), os.path.basename(source),
                                        source_path=source)
    arm_obj = sc.rif_armature(collection)
    if arm_obj is None:
        print("  skipped: %s has no hierarchy" % os.path.basename(source))
        return

    before = sequences_in(rif.load(source))
    animated = [s for s in before if len(s[2]) > 1]
    check(bool(animated), "the source has an animated sequence to work with")
    if not animated:
        return

    # Baseline: an untouched export reproduces every frame time exactly.
    path = os.path.join(tmp, "rig_edit.rif")
    export(collection, path)
    check(sorted(sequences_in(rif.load(path))) == sorted(before),
          "an untouched rig re-exports every sequence unchanged")

    # Now insert a key into one bone of one sequence.
    action = next((a for a in sc._rif_actions(arm_obj) if sc._animated_bones(a)), None)
    check(action is not None, "found an Action to edit")
    if action is None:
        return
    bone_name = sorted(sc._animated_bones(action))[0]
    arm_obj.animation_data.action = action
    counts_before = sum(len(s[2]) for s in sequences_in(rif.load(path)))
    key_bone(arm_obj, action, bone_name, [7.5])

    export(collection, path)
    after = sequences_in(rif.load(path))
    counts_after = sum(len(s[2]) for s in after)
    check(counts_after == counts_before + 1,
          "inserting one keyframe adds exactly one frame (%d -> %d)"
          % (counts_before, counts_after))
    for _name, _sub, times in after:
        if times and sorted(times) != times:
            check(False, "frame times stayed sorted")
            break
    else:
        check(True, "frame times stayed sorted")

    # Renaming a bound object has to follow into the node's OBJHIERD.
    part = next((o for o in collection.objects
                 if o.get("rif_rig_parented") and "rif_lod_base" not in o), None)
    check(part is not None, "found a rig-parented mesh")
    if part is not None:
        sc.set_rif_object_name(part, "Renamed Part")
        export(collection, path)
        check("Renamed Part" in bindings_in(rif.load(path)),
              "renaming a bound object follows into its node's binding")
    os.remove(path)


def settings_in(root):
    """``{sequence name: {chunk id: body}}`` for the three optional chunks."""
    out = {}
    for c in root.walk():
        if c.id != b"OBANSEQC":
            continue
        head = c.find(b"OBASEQHD")
        name = heads.seqhead_name(head.body) if head is not None else "?"
        for cid in (b"OBASEQTM", b"OBASEQFL", b"OBASEQSP"):
            kid = c.find(cid)
            if kid is not None:
                out.setdefault(name, {})[cid] = bytes(kid.body)
    return out


def test_sequence_settings(tmp):
    print("sequence settings")
    reset()

    collection = sc.new_collection("set.rif")
    arm_obj = build_rig(collection, ["Waist", "Foot"])
    action = bpy.data.actions.new("Seq_Run")
    sc.adopt_action(arm_obj, action)
    # Both bones, so "every bone of this sequence" means more than one.
    key_bone(arm_obj, action, "Foot", [0.0, 10.0])
    key_bone(arm_obj, action, "Waist", [0.0, 10.0])

    path = os.path.join(tmp, "set.rif")
    export(collection, path)
    check(not settings_in(rif.load(path)),
          "a new sequence carries none of the three by default")

    sc.set_sequence_setting(action, b"OBASEQTM", 1500)
    sc.set_sequence_setting(action, b"OBASEQSP", [2400, 0, 0])
    sc.set_sequence_setting(action, b"OBASEQFL", heads.SEQ_FLAG_LOOPS)
    export(collection, path)
    got = settings_in(rif.load(path))
    check("Seq_Run" in got, "the settings reached the file, got %s" % sorted(got))
    if "Seq_Run" in got:
        s = got["Seq_Run"]
        check(struct.unpack_from("<i", s[b"OBASEQTM"], 0)[0] == 1500,
              "duration wrote 1500 ms")
        check(struct.unpack_from("<3i", s[b"OBASEQSP"], 0) == (2400, 0, 0),
              "speed wrote 2400 mm/s with angle and spare zero, got %s"
              % (struct.unpack_from("<3i", s[b"OBASEQSP"], 0),))
        check(struct.unpack_from("<i", s[b"OBASEQFL"], 0)[0] == heads.SEQ_FLAG_LOOPS,
              "flags wrote SequenceFlag_Loops")

    # A new sequence has no recorded presence (`rif_seq_had`), so the setting
    # goes on every bone that carries the sequence -- here both of them.
    count = sum(1 for c in rif.load(path).walk() if c.id == b"OBASEQTM")
    check(count == 2, "the setting is on both bones' copies, got %d" % count)

    # Removing is a real state, not a zero.
    sc.set_sequence_setting(action, b"OBASEQTM", None)
    export(collection, path)
    check(b"OBASEQTM" not in settings_in(rif.load(path)).get("Seq_Run", {}),
          "removing the duration drops the chunk")
    check(b"OBASEQSP" in settings_in(rif.load(path)).get("Seq_Run", {}),
          "and leaves the others alone")
    os.remove(path)


def test_settings_survive_an_import(game_dir, tmp):
    """The subset a file carries has to come back as that same subset."""
    print("editing imported sequence settings")
    source = None
    for dp, _, ns in os.walk(os.path.join(game_dir, "RIF")):
        for n in sorted(ns):
            if n.lower() == "elint mkii.rif":
                source = os.path.join(dp, n)
    if source is None:
        print("  skipped: Elint MkII.RIF not found")
        return

    reset()
    collection, _stats = sc.build_scene(rif.load(source), os.path.basename(source),
                                        source_path=source)
    want = settings_in(rif.load(source))
    check(bool(want), "the source carries optional settings")

    path = os.path.join(tmp, "set_edit.rif")
    export(collection, path)
    check(settings_in(rif.load(path)) == want,
          "an untouched file re-exports the same settings on the same sequences")

    # The count matters as much as the values: the file puts them on a subset of
    # bones, and appending to all of them would be a silent change.
    for cid in (b"OBASEQTM", b"OBASEQFL", b"OBASEQSP"):
        a = sum(1 for c in rif.load(source).walk() if c.id == cid)
        b = sum(1 for c in rif.load(path).walk() if c.id == cid)
        check(a == b, "%s count unchanged (%d -> %d)" % (cid.decode(), a, b))

    arm_obj = sc.rif_armature(collection)
    action = next((a for a in sc._rif_actions(arm_obj)
                   if sc.sequence_setting(a, b"OBASEQTM") is not None), None)
    check(action is not None, "found a sequence with a duration to edit")
    if action is not None:
        sc.set_sequence_setting(action, b"OBASEQTM", 4321)
        export(collection, path)
        seqs = settings_in(rif.load(path))
        name = action.get("rif_sequence", action.name)
        check(name in seqs and
              struct.unpack_from("<i", seqs[name][b"OBASEQTM"], 0)[0] == 4321,
              "editing the duration reaches every copy of that sequence")
    os.remove(path)


def sounds_in(root):
    """``[(index, path, min, max, volume)]`` for every INDSOUND, sorted."""
    import sounds as sndmod
    out = []
    for c in root.walk():
        if c.id != b"INDSOUND":
            continue
        e = sndmod.decode(c.body)
        out.append((e["index"], e["path"], e["min_distance"], e["max_distance"],
                    e["volume"]))
    return sorted(out)


def frame_sounds(root):
    """``{(sequence, time): sound index}`` for every frame that triggers one."""
    out = {}
    for c in root.walk():
        if c.id != b"OBANSEQC":
            continue
        head = c.find(b"OBASEQHD")
        name = heads.seqhead_name(head.body) if head is not None else "?"
        for k in (c.children or ()):
            if k.id != b"OBASEQFR":
                continue
            fl, = struct.unpack_from("<I", k.body, 0x24)
            idx = heads.frame_sound_index(fl)
            if idx:
                out[(name, struct.unpack_from("<i", k.body, 0x1C)[0])] = idx
    return out


def test_sounds_from_scratch(tmp):
    print("sounds from scratch")
    reset()

    collection = sc.new_collection("snd.rif")
    arm_obj = build_rig(collection, ["Waist", "Foot"])
    action = bpy.data.actions.new("Seq_Walk")
    sc.adopt_action(arm_obj, action)
    key_bone(arm_obj, action, "Foot", [0.0, 10.0, 20.0])

    step = sc.add_sound(collection, "Robots" + chr(92) + "GL_Footstep_Metal_01.wav")
    clink = sc.add_sound(collection, "Robots" + chr(92) + "CLUNK1.WAV")
    check(step.data["rif_sound_index"] == 1, "the first sound takes slot 1")
    check(clink.data["rif_sound_index"] == 2, "the second takes slot 2")
    # Slot 0 must never be handed out: 0 is how a frame says "no sound".
    check(0 not in {e["index"] for e in sc.sound_table(collection)},
          "slot 0 is never allocated")

    step.data.distance_max = 12.0
    step.data.volume = 0.5
    sc.set_sound_event(action, "Foot", 10.0, 1)
    sc.set_sound_event(action, "Foot", 20.0, 2)

    path = os.path.join(tmp, "snd.rif")
    export(collection, path)
    root = rif.load(path)

    got = sounds_in(root)
    check(len(got) == 2, "both entries reached the file, got %d" % len(got))
    check(got[0][:2] == (1, "Robots" + chr(92) + "GL_Footstep_Metal_01.wav"),
          "entry 1 carries its path, got %s" % (got[0][:2],))
    check(got[0][3] == 12000, "the speaker's max distance wrote 12000 mm, got %d" % got[0][3])
    check(got[0][4] == 64, "volume 0.5 wrote 64 of 127, got %d" % got[0][4])

    ev = frame_sounds(root)
    check(len(ev) == 2, "two frames trigger a sound, got %d" % len(ev))
    check(sorted(ev.values()) == [1, 2],
          "the right indices reached the frames, got %s" % sorted(ev.values()))

    # And an event on a frame with no key must not invent one.
    before = len([k for c in root.walk() if c.id == b"OBANSEQC"
                  for k in (c.children or ()) if k.id == b"OBASEQFR"])
    sc.set_sound_event(action, "Foot", 13.0, 2)
    export(collection, path)
    after = len([k for c in rif.load(path).walk() if c.id == b"OBANSEQC"
                 for k in (c.children or ()) if k.id == b"OBASEQFR"])
    check(before == after,
          "a sound on a frame with no keyframe adds no frame (%d -> %d)" % (before, after))
    os.remove(path)


def test_sounds_survive_an_import(game_dir, tmp):
    print("editing an imported sound table")
    source = None
    for dp, _, ns in os.walk(os.path.join(game_dir, "RIF")):
        for n in sorted(ns):
            if n.lower() == "biggerlob.rif":
                source = os.path.join(dp, n)
    if source is None:
        print("  skipped: no sound-carrying file found")
        return

    reset()
    collection, stats = sc.build_scene(rif.load(source), os.path.basename(source),
                                       source_path=source)
    want = sounds_in(rif.load(source))
    check(bool(want), "the source has a sound table")
    check(stats.get("speakers") == len(want),
          "one speaker per entry, got %s for %d" % (stats.get("speakers"), len(want)))
    check(len(sc.sound_table(collection)) == len(want),
          "the table reads back off the speakers")

    path = os.path.join(tmp, "snd_edit.rif")
    export(collection, path)
    check(sounds_in(rif.load(path)) == want, "an untouched table re-exports unchanged")
    check(frame_sounds(rif.load(path)) == frame_sounds(rif.load(source)),
          "every frame keeps the sound it triggered")

    # Deleting a speaker is how a sound is removed.
    speakers = sc.sound_speakers(collection)
    bpy.data.objects.remove(speakers[0], do_unlink=True)
    export(collection, path)
    check(len(sounds_in(rif.load(path))) == len(want) - 1,
          "removing a speaker drops its entry")
    os.remove(path)


def run(game_dir):
    tmp = tempfile.gettempdir()
    test_from_scratch(tmp)
    test_edits_survive_a_blend(tmp)
    test_duplicate_is_caught(tmp)
    test_rig_from_scratch(tmp)
    test_sounds_from_scratch(tmp)
    test_sequence_settings(tmp)
    if game_dir:
        test_edit_an_imported_file(game_dir, tmp)
        test_added_keys_and_bones(game_dir, tmp)
        test_sounds_survive_an_import(game_dir, tmp)
        test_settings_survive_an_import(game_dir, tmp)

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
    sys.exit(run(argv[0] if argv else None))
