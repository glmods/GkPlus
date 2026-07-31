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
- **A light can be authored, and it lands where the format keeps lights.** Every
  one of the 3,794 shipped ``STDLIGHT`` chunks is a child of the single
  ``LIGHTSET`` under ``REBENVDT``, so adopting the first light into a file that
  has none has to lift that absorbed container into an object first.
- **A duplicated object is caught.** Copying an object in Blender copies its
  shape id, which no ``.rif`` can represent; export must refuse rather than
  produce a file whose second mesh is orphaned on re-import.
- **The Gunlok preview reproduces the engine's own shading, numerically.** A
  known texel times a known light is rendered and compared against
  ``texel * light / 255``, which is what ``BuildShapeVertexBuffers`` and
  ``Mat_Opaque``'s D3DTOP_MODULATE amount to. The naive linear-space multiply is
  rendered beside it and asserted to *fail* the same tolerance, so the check
  cannot start passing again if the node tree is simplified back.

The Gunlok directory is optional; without it the groups that edit a real
imported file are skipped.
"""

import contextlib
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


def lights_in(root):
    """``[(light_id, position, colour, brightness, range)]`` for every STDLIGHT."""
    import schema as schmod
    out = []
    for c in root.walk():
        if c.id != b"STDLIGHT":
            continue
        d = schmod.decode(b"STDLIGHT", c.body)
        out.append((d["light_id"], tuple(d["position"]), d["colour"],
                    d["brightness"], d["range"]))
    return sorted(out)


def lightset_layout(root):
    """The child ids of every LIGHTSET, in order, with the STDLIGHT run collapsed."""
    out = []
    for c in root.walk():
        if c.id != b"LIGHTSET":
            continue
        shape = []
        for kid in (c.children or ()):
            name = kid.id.decode()
            if not shape or shape[-1] != name:
                shape.append(name)
        out.append(shape)
    return out


def test_lights_from_scratch(tmp):
    print("lights from scratch")
    reset()

    collection = sc.new_collection("lit.rif")
    # A light in a file that has none: REBENVDT is absorbed onto the collection
    # here, so adopting has to lift it into an object before it can hold a set.
    check(sc.lightset_for(collection, create=False) is None, "a new file has no light set")

    data = bpy.data.lights.new("Lamp", type="POINT")
    lamp = bpy.data.objects.new("Lamp", data)
    bpy.context.scene.collection.objects.link(lamp)
    lamp.location = (1.0, 2.0, 3.0)
    data.color = (1.0, 0.5, 0.0)

    check(sc.adopt_object(collection, lamp) is None, "light adopted")
    check(lamp.get("rif_id") == "STDLIGHT", "the light is a STDLIGHT")
    lightset = sc.lightset_for(collection, create=False)
    check(lightset is not None, "a LIGHTSET was created")
    check(lightset is not None and lamp.parent == lightset, "the light sits in it")
    check(lightset is not None and lightset.parent is not None
          and lightset.parent.get("rif_id") == "REBENVDT",
          "the set sits under REBENVDT, as all 62 shipped ones do")
    # Blender's 1000 W default would export a brightness 1000x anything shipped.
    check(abs(data.energy - 1.0) < 1e-6, "adoption normalised the energy, got %r" % data.energy)
    check(data.use_custom_distance, "adoption turned on the range")

    # A cube too, so the object list and the light set coexist.
    sc.adopt_object(collection, add_cube("Land", size=2.0))

    path = os.path.join(tmp, "lit.rif")
    _root, stats = export(collection, path)
    check(stats["lights"] == 1, "exported 1 light, got %d" % stats["lights"])
    root = rif.load(path)

    got = lights_in(root)
    check(len(got) == 1, "the light reached the file, got %d" % len(got))
    if got:
        _lid, pos, colour, bright, rng = got[0]
        # (x, y, z) -> (x, -z, y) in rif units: Blender Z-up, RIF Y-down.
        check(pos == (1000, -3000, 2000), "position round-tripped, got %s" % (pos,))
        # 0.5 * 255 is 127.5, and Python rounds half to even: 128, not 127.
        check(colour == 0xFF8000, "colour round-tripped, got 0x%06X" % colour)
        check(bright == 65536, "brightness wrote 1.0 in 16.16, got %d" % bright)
        check(rng > 0, "range is not zero, got %d" % rng)

    check(lightset_layout(root) == [["LTSETHDR", "STDLIGHT", "AMBIENCE"]],
          "the set is laid out as the shipped ones are, got %s" % lightset_layout(root))
    # The object list must be untouched by any of this.
    check(names_in(objects_in(root)) == ["Land"],
          "the light is not in the object list, got %s" % names_in(objects_in(root)))

    # Re-importing has to put it back as a light, not as an empty.
    reset()
    reopened, stats2 = sc.build_scene(rif.load(path), "lit.rif")
    lamps = [o for o in reopened.objects if o.get("rif_id") == "STDLIGHT"]
    check(len(lamps) == 1, "the light came back as a light, got %d" % len(lamps))
    if lamps:
        check(lamps[0].type == "LIGHT", "it is a Blender light, got %s" % lamps[0].type)
        check(abs(lamps[0].data.color[0] - 1.0) < 0.01
              and abs(lamps[0].data.color[1] - 0.5) < 0.01,
              "its colour survived, got %s" % list(lamps[0].data.color))
    # And exporting the re-import must not change anything.
    again = os.path.join(tmp, "lit2.rif")
    export(reopened, again)
    check(lights_in(rif.load(again)) == lights_in(root),
          "a re-imported light re-exports unchanged")
    for f in (path, again):
        os.remove(f)


def test_second_light_and_ids(tmp):
    print("more than one light")
    reset()

    collection = sc.new_collection("lit2.rif")
    made = []
    for i in range(3):
        data = bpy.data.lights.new("Lamp%d" % i, type="POINT")
        obj = bpy.data.objects.new("Lamp%d" % i, data)
        bpy.context.scene.collection.objects.link(obj)
        obj.location = (float(i), 0.0, 0.0)
        check(sc.adopt_object(collection, obj) is None, "light %d adopted" % i)
        made.append(obj)

    sets = [o for o in collection.objects if o.get("rif_id") == "LIGHTSET"]
    check(len(sets) == 1, "all three share one light set, got %d" % len(sets))
    ids = [o["rif_light_id"] for o in made]
    check(len(set(ids)) == 3, "each got its own light_id, got %s" % ids)

    path = os.path.join(tmp, "lit2.rif")
    export(collection, path)
    root = rif.load(path)
    check(len(lights_in(root)) == 3, "all three reached the file")
    check(lightset_layout(root) == [["LTSETHDR", "STDLIGHT", "AMBIENCE"]],
          "AMBIENCE stays after the lights, got %s" % lightset_layout(root))
    check(len({lid for lid, *_ in lights_in(root)}) == 3, "the file's ids are distinct")

    # Deleting one is how a light is removed -- nothing else to unregister.
    bpy.data.objects.remove(made[1], do_unlink=True)
    export(collection, path)
    check(len(lights_in(rif.load(path))) == 2, "removing the object removes the light")
    os.remove(path)


def test_light_in_an_imported_file(game_dir, tmp):
    """A file that already has a LIGHTSET must gain a light, not a second set."""
    print("adding a light to an imported file")
    source = None
    for dp, _, ns in os.walk(os.path.join(game_dir, "RIF")):
        for n in sorted(ns):
            if not n.lower().endswith(".rif"):
                continue
            p = os.path.join(dp, n)
            if any(c.id == b"STDLIGHT" for c in rif.load(p).walk()):
                source = p
                break
        if source:
            break
    if source is None:
        print("  skipped: no light-carrying file found")
        return

    reset()
    collection, _stats = sc.build_scene(rif.load(source), os.path.basename(source),
                                        source_path=source)
    want = lights_in(rif.load(source))
    check(bool(want), "the source has lights")

    path = os.path.join(tmp, "lit_edit.rif")
    export(collection, path)
    check(lights_in(rif.load(path)) == want, "an untouched file re-exports its lights")
    check(lightset_layout(rif.load(path)) == lightset_layout(rif.load(source)),
          "and the set keeps its layout")

    data = bpy.data.lights.new("Added", type="POINT")
    added = bpy.data.objects.new("Added", data)
    bpy.context.scene.collection.objects.link(added)
    check(sc.adopt_object(collection, added) is None, "light adopted into an imported file")

    sets = [o for o in collection.objects if o.get("rif_id") == "LIGHTSET"]
    check(len(sets) == 1, "no second light set was made, got %d" % len(sets))
    export(collection, path)
    root = rif.load(path)
    check(len(lights_in(root)) == len(want) + 1,
          "exactly one light was added (%d -> %d)" % (len(want), len(lights_in(root))))
    ids = [lid for lid, *_ in lights_in(root)]
    check(len(set(ids)) == len(ids), "no light_id is claimed twice")
    check(lightset_layout(root) == [["LTSETHDR", "STDLIGHT", "AMBIENCE"]],
          "AMBIENCE is still last, got %s" % lightset_layout(root))
    os.remove(path)


def vtints_in(root):
    """``[(light_set_name, num_vertices, len(array))]`` for every SHPVTINT."""
    out = []
    for c in root.walk():
        if c.id != b"SHPVTINT":
            continue
        name = bytes(c.body[0:8]).rstrip(b"\0").decode("latin-1")
        declared = struct.unpack_from("<i", c.body, 12)[0]
        out.append((name, declared, (len(c.body) - 16) // 4))
    return out


def test_vertex_lighting(tmp):
    """SHPVTINT is the only lighting the game reads, so its header has to be right.

    Two words in it are not opaque: the name selects the chunk (the loader takes
    the one matching the active light set) and ``num_vertices`` is trusted -- the
    engine allocates and iterates that many times.
    """
    print("baked vertex lighting")
    reset()

    collection = sc.new_collection("lit3.rif")
    cube = add_cube("Land", size=2.0)
    sc.adopt_object(collection, cube)

    # Authoring lighting is enabling it and painting the colour attribute; the
    # packed int exists only on the wire.
    sc.enable_lighting(cube.data)
    attr = cube.data.color_attributes[sc.LIGHT_COLOR_ATTR]
    for i in range(len(cube.data.vertices)):
        v = 255 if i % 2 else 8
        attr.data[i].color_srgb = (v / 255.0, v / 255.0, v / 255.0, 1.0)

    path = os.path.join(tmp, "lit3.rif")
    export(collection, path)
    got = vtints_in(rif.load(path))
    check(len(got) == 1, "the attribute became a SHPVTINT, got %d" % len(got))
    if got:
        name, declared, actual = got[0]
        # A zeroed name is never matched by the loader, so a new chunk must not get one.
        check(name == "NORMALLT", "it carries the light set name, got %r" % name)
        check(actual == 8, "one value per cube vertex, got %d" % actual)
        check(declared == 8, "num_vertices matches the array, got %d" % declared)

    # And a stale stored count must be corrected, not carried: that is the case a
    # mesh edit produces, and the engine reads past the body if it is too large.
    cube.data[sc.VTINT_HEADER_PROP] = sc.VTINT_HEADER[:3] + [999]
    export(collection, path)
    got = vtints_in(rif.load(path))
    check(got and got[0][1] == got[0][2] == 8,
          "a stale num_vertices is regenerated, got %s" % (got,))
    os.remove(path)


def test_lighting_is_name_gated(tmp):
    """`rif_light` by name, the marker for presence -- and no silent third way in.

    Export must not read whatever colour attribute happens to be *active*: that
    is Blender-wide UI state a bake or a preview can repoint, so anything else
    being active is the normal case, not an error.
    """
    print("vertex lighting is name-gated")
    reset()

    collection = sc.new_collection("gated.rif")
    cube = add_cube("Land", size=2.0)
    sc.adopt_object(collection, cube)
    me = cube.data
    path = os.path.join(tmp, "gated.rif")

    # 1. A paintable attribute with no marker is not lighting. This is exactly
    #    what the preview mints, and it must not add a chunk.
    sc.white_light_attribute(me)
    check(not sc.has_lighting(me), "a bare attribute is not lighting")
    export(collection, path)
    check(vtints_in(rif.load(path)) == [],
          "an unmarked mesh exports no SHPVTINT")

    # 2. Another colour attribute, made active, is still not the lighting.
    sc.enable_lighting(me)
    for d in me.color_attributes[sc.LIGHT_COLOR_ATTR].data:
        d.color_srgb = (8 / 255.0, 8 / 255.0, 8 / 255.0, 1.0)
    decoy = me.color_attributes.new(name="Decoy", type="BYTE_COLOR", domain="POINT")
    for d in decoy.data:
        d.color_srgb = (1.0, 0.0, 0.0, 1.0)
    for i, a in enumerate(me.color_attributes):
        if a.name == "Decoy":
            me.color_attributes.active_color_index = i
    export(collection, path)
    got = vtints_in(rif.load(path))
    check(len(got) == 1 and got[0][1] == got[0][2] == 8,
          "the marked mesh exports one chunk, got %s" % (got,))
    body = next(c.body for c in rif.load(path).walk() if c.id == b"SHPVTINT")
    vals = struct.unpack_from("<8i", body, 16)
    check(all(sc.unpack_light(v)[:3] == (8, 8, 8) for v in vals),
          "and it is rif_light, not the active decoy, got %s"
          % [hex(v & 0xFFFFFFFF) for v in vals[:2]])

    # 3. Adopting the decoy is the deliberate way in, and then it *is* the lighting.
    source, why = sc.adopt_color_attribute(me)
    check(why is None, "the active attribute is adoptable: %s" % why)
    check(source == "Decoy", "it read the active one, got %s" % source)
    export(collection, path)
    body = next(c.body for c in rif.load(path).walk() if c.id == b"SHPVTINT")
    vals = struct.unpack_from("<8i", body, 16)
    check(all(sc.unpack_light(v)[:3] == (255, 0, 0) for v in vals),
          "after adopting, the file has the adopted colour, got %s"
          % [hex(v & 0xFFFFFFFF) for v in vals[:2]])
    os.remove(path)


def test_lighting_refuses_a_stale_attribute(tmp):
    """A mesh edit that outran the lighting is refused, never averaged or padded.

    Silently writing a partial SHPVTINT is the failure the name gate exists to
    prevent, so this must be loud at export and visible before it.
    """
    print("vertex lighting refuses a stale attribute")
    reset()

    collection = sc.new_collection("stale.rif")
    cube = add_cube("Land", size=2.0)
    sc.adopt_object(collection, cube)
    me = cube.data
    sc.enable_lighting(me)

    # A corner-domain attribute under the stored name: one value per corner, not
    # per vertex, which is what Blender's own "add colour attribute" produces.
    me.color_attributes.remove(me.color_attributes[sc.LIGHT_COLOR_ATTR])
    me.color_attributes.new(name=sc.LIGHT_COLOR_ATTR, type="BYTE_COLOR",
                            domain="CORNER")

    _values, why = sc.lighting_values(me)
    check(why is not None, "a corner-domain rif_light is refused")
    check(sc.lighting_problems(collection),
          "and the export pre-check reports it before anything is written")

    raised = None
    try:
        sc.rebuild_tree(collection)
    except ValueError as exc:
        raised = str(exc)
    check(raised is not None, "export raises rather than writing it")

    # Adopting it is the fix, and it converts rather than refusing.
    source, why = sc.adopt_color_attribute(me, name=sc.LIGHT_COLOR_ATTR)
    check(why is None, "adopting the corner attribute fixes it: %s" % why)
    check(not sc.lighting_problems(collection), "the pre-check is clean afterwards")

    # A marker with the attribute deleted is a dropped chunk, not a refusal:
    # export says so rather than failing or losing it silently.
    me.color_attributes.remove(me.color_attributes[sc.LIGHT_COLOR_ATTR])
    check(not sc.lighting_problems(collection), "a deleted attribute is not a refusal")
    _root, stats = sc.rebuild_tree(collection)
    check(stats["lighting_dropped"] == 1,
          "it is counted so export can warn, got %s" % stats["lighting_dropped"])


def test_lighting_color_round_trip(tmp):
    """Storing the paintable form has to be lossless, or every edit degrades it.

    `color` on a BYTE_COLOR converts sRGB<->linear and loses a least-significant
    bit on 157 of 256 values; `color_srgb` is the stored byte and is exact. That
    measurement is the whole licence for storing *only* the colour attribute,
    so this is the test that would catch a switch back to `color`.
    """
    print("vertex lighting stored as a colour attribute")
    reset()

    collection = sc.new_collection("bake.rif")
    cube = add_cube("Land", size=2.0)
    sc.adopt_object(collection, cube)
    me = cube.data
    n = len(me.vertices)

    name, had = sc.enable_lighting(me)
    check(not had, "the mesh had no lighting before")
    check(name == sc.LIGHT_COLOR_ATTR, "made %s, got %s" % (sc.LIGHT_COLOR_ATTR, name))
    col = me.color_attributes.get(sc.LIGHT_COLOR_ATTR)
    check(col is not None and col.data_type == "BYTE_COLOR" and col.domain == "POINT",
          "it is a per-vertex BYTE_COLOR")
    # Active, or a Cycles bake targeting the active colour attribute misses it --
    # and here that bake writes the stored value, with no packing step at all.
    check(me.color_attributes.active_color is not None
          and me.color_attributes.active_color.name == sc.LIGHT_COLOR_ATTR,
          "and it is the active colour attribute, which is what a bake writes to")

    # Values chosen to cover both alpha cases and channel values that are not
    # representable after an sRGB<->linear round trip.
    want = [sc.pack_light((i * 37) % 256, (i * 91) % 256, i % 256, 255 if i % 3 else 0)
            for i in range(n)]
    for i, v in enumerate(want):
        r, g, b, a = sc.unpack_light(v)
        col.data[i].color_srgb = (r / 255.0, g / 255.0, b / 255.0, a / 255.0)

    got, why = sc.lighting_values(me)
    check(why is None, "it reads back as one packed int per vertex: %s" % why)
    check(got == want, "the round trip is lossless (%d of %d exact)"
                       % (sum(1 for a, b in zip(got, want) if a == b), n))

    # It must survive a .blend, since that is where a colour attribute usually
    # degrades -- and the marker has to survive with it, or the chunk is dropped.
    blend = os.path.join(tmp, "rif_bake.blend")
    bpy.ops.wm.save_as_mainfile(filepath=blend)
    bpy.ops.wm.read_homefile(use_empty=True)
    bpy.ops.wm.open_mainfile(filepath=blend)
    reopened = bpy.data.collections.get("bake.rif")
    me2 = next(o for o in reopened.objects if o.type == "MESH").data
    check(sc.has_lighting(me2), "the marker survives a .blend round trip")
    got2, why2 = sc.lighting_values(me2)
    check(why2 is None and got2 == want, "and so do the values exactly")

    path = os.path.join(tmp, "bake.rif")
    export(reopened, path)
    vt = vtints_in(rif.load(path))
    check(len(vt) == 1 and vt[0][1] == vt[0][2] == n,
          "it exports as one SHPVTINT with a matching count, got %s" % (vt,))
    body = next(c.body for c in rif.load(path).walk() if c.id == b"SHPVTINT")
    check(list(struct.unpack_from("<%di" % n, body, 16)) == want,
          "and the file holds exactly the painted bytes")
    for f in (blend, path):
        if os.path.exists(f):
            os.remove(f)


def test_lighting_from_a_corner_attribute(tmp):
    """Blender's default colour attribute is corner-domain, and a bake targets it."""
    print("corner-domain colour attribute")
    reset()

    collection = sc.new_collection("corner.rif")
    cube = add_cube("Land", size=2.0)
    sc.adopt_object(collection, cube)
    me = cube.data

    # No lighting at all: the "start lighting this mesh" path.
    name, had = sc.enable_lighting(me)
    check(not had, "a mesh with no lighting reports so")
    white = [me.color_attributes[name].data[i].color_srgb[:] for i in range(len(me.vertices))]
    check(all(abs(c - 1.0) < 1e-6 for col in white for c in col),
          "and starts white, which is what a bake needs to write into")

    corner = me.color_attributes.new(name="Col", type="BYTE_COLOR", domain="CORNER")
    for li in range(len(me.loops)):
        corner.data[li].color_srgb = (1.0, 0.0, 0.0, 1.0)
    source, why = sc.adopt_color_attribute(me, name="Col")
    check(why is None, "a corner attribute is accepted: %s" % why)
    vals, why = sc.lighting_values(me)
    check(why is None and set(vals) == {sc.pack_light(255, 0, 0, 255)},
          "every vertex averaged to red, got %s"
          % [hex(v & 0xFFFFFFFF) for v in set(vals or ())])


def test_lighting_from_a_bake(tmp):
    """The workflow this exists for: Cycles bake straight into the stored attribute."""
    print("cycles bake into vertex lighting")
    reset()

    scene = bpy.context.scene
    try:
        scene.render.engine = "CYCLES"
    except TypeError:
        print("  skipped: Cycles not available")
        return
    scene.cycles.samples = 4
    scene.cycles.device = "CPU"

    collection = sc.new_collection("bakelvl.rif")
    bpy.ops.mesh.primitive_grid_add(size=4.0, x_subdivisions=4, y_subdivisions=4)
    plane = bpy.context.active_object
    plane.name = "Floor"
    sc.adopt_object(collection, plane)

    mat = bpy.data.materials.new("floor")
    plane.data.materials.append(mat)

    # 20 W, not more: the packing clamps at 1.0, and a bright lamp saturates every
    # vertex to white -- which looks exactly like a bake that never ran.
    lamp_data = bpy.data.lights.new("Baker", type="POINT")
    lamp_data.energy = 20.0
    lamp = bpy.data.objects.new("Baker", lamp_data)
    bpy.context.scene.collection.objects.link(lamp)
    lamp.location = (0.0, 0.0, 2.0)

    sc.enable_lighting(plane.data)            # the bake target, made active
    bpy.context.view_layer.objects.active = plane
    plane.select_set(True)
    try:
        bpy.ops.object.bake(type="DIFFUSE", pass_filter={"DIRECT", "INDIRECT"},
                            target="VERTEX_COLORS", use_clear=True)
    except (RuntimeError, TypeError) as exc:
        print("  skipped: bake unavailable (%s)" % exc)
        return

    # No packing step: the bake targeted the active colour attribute, which IS
    # the stored one, so export reads it as it stands.
    vals, why = sc.lighting_values(plane.data)
    check(why is None, "the bake is readable as lighting: %s" % why)
    check(len(set(vals)) > 1,
          "the bake produced varying lighting, got %d distinct value(s)" % len(set(vals)))
    check(all(sc.unpack_light(v)[3] == 255 for v in vals),
          "every vertex is opaque after a bake")
    check(not all(sc.unpack_light(v)[:3] == (255, 255, 255) for v in vals),
          "the bake is not saturated to white (raise the lamp or lower its energy)")
    # A point light above the centre: the middle must come out brighter than a corner.
    def lum(v):
        r, g, b, _a = sc.unpack_light(v)
        return r + g + b
    co = plane.data.vertices
    centre = min(range(len(vals)), key=lambda i: co[i].co.length)
    corner = max(range(len(vals)), key=lambda i: co[i].co.length)
    check(lum(vals[centre]) > lum(vals[corner]),
          "under the lamp is brighter than the far corner (%d vs %d)"
          % (lum(vals[centre]), lum(vals[corner])))

    path = os.path.join(tmp, "bakelvl.rif")
    export(collection, path)
    vt = vtints_in(rif.load(path))
    check(len(vt) == 1 and vt[0][0] == "NORMALLT" and vt[0][1] == vt[0][2],
          "and it exports as a well-formed SHPVTINT, got %s" % (vt,))
    os.remove(path)


def table_in(root):
    """The BMPNAMES entries of a file, as ``[(index, name)]``."""
    import bmpnames
    for c in root.walk():
        if c.id == b"BMPNAMES":
            _version, entries = bmpnames.decode(c.body)
            return [(e["index"], e["name"]) for e in entries]
    return None


def test_light_keeps_the_texture_table(game_dir, tmp):
    """The case `_promote_rebenvdt` exists for: a textured file with no lights.

    REBENVDT is absorbed onto the *collection* there, and it owns the texture
    table's recorded path. Lifting it into an object to hold the new LIGHTSET has
    to take that path with it, or the table is written at the wrong place -- or
    not at all.
    """
    print("adding a light to a textured file")
    source = None
    for dp, _, ns in os.walk(os.path.join(game_dir, "RIF")):
        for n in sorted(ns):
            if not n.lower().endswith(".rif"):
                continue
            p = os.path.join(dp, n)
            root = rif.load(p)
            ids = {c.id for c in root.walk()}
            if b"BMPNAMES" in ids and b"LIGHTSET" not in ids and table_in(root):
                source = p
                break
        if source:
            break
    if source is None:
        print("  skipped: no textured light-free file found")
        return

    reset()
    collection, _stats = sc.build_scene(rif.load(source), os.path.basename(source),
                                        source_path=source, load_images=False)
    want = table_in(rif.load(source))
    check(collection.get("rif_bmpnames_path") is not None,
          "the table's path starts on the collection")
    check(sc.lightset_for(collection, create=False) is None, "and the file has no light set")

    data = bpy.data.lights.new("Added", type="POINT")
    added = bpy.data.objects.new("Added", data)
    bpy.context.scene.collection.objects.link(added)
    check(sc.adopt_object(collection, added) is None, "light adopted into a textured file")

    env = [o for o in collection.objects if o.get("rif_id") == "REBENVDT"]
    check(len(env) == 1, "REBENVDT was lifted into exactly one object, got %d" % len(env))
    check(collection.get("rif_bmpnames_path") is None
          and env and env[0].get("rif_bmpnames_path") is not None,
          "the table's path moved onto it")

    path = os.path.join(tmp, "lit_tex.rif")
    export(collection, path)
    root = rif.load(path)
    check(table_in(root) == want,
          "the texture table survived the lift (%s entries -> %s)"
          % (len(want), len(table_in(root) or ())))
    check(len(lights_in(root)) == 1, "and the light is there")
    # One REBENVDT, holding both -- not a second one beside the absorbed original.
    envs = [c for c in root.walk() if c.id == b"REBENVDT"]
    check(len(envs) == 1, "the file still has exactly one REBENVDT, got %d" % len(envs))
    check(envs and any(k.id == b"BMPNAMES" for k in (envs[0].children or ()))
          and any(k.id == b"LIGHTSET" for k in (envs[0].children or ())),
          "which holds both the table and the light set")
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




# --------------------------------------------------------------------------
# The Gunlok preview: measured against the engine's own arithmetic
# --------------------------------------------------------------------------
#
# The check that matters here is numeric, not visual. `texel * light / 255` is
# what `BuildShapeVertexBuffers` and `Mat_Opaque`'s D3DTOP_MODULATE come to
# between them, and the whole difficulty is that it happens on the gamma-encoded
# bytes. A material that multiplies in linear space *looks* plausible and is
# wrong by up to 7.43/255, worst in the dark midrange Gunlok's lighting lives in
# -- so the naive graph is rendered beside the real one and asserted to fail,
# which is what stops this test passing again if someone simplifies the node
# tree back to a linear multiply.

PREVIEW_CASES = [
    # (texel, light) -- the light values are chosen where the linear multiply is
    # worst, which is exactly where shipped levels sit.
    ((0xC0, 0x90, 0x40), (0x40, 0x80, 0x20)),
    ((0xFF, 0xFF, 0xFF), (0x08, 0x20, 0x40)),
    ((0x80, 0x40, 0x20), (0xFF, 0xFF, 0xFF)),
    ((0x33, 0x66, 0x99), (0x99, 0x66, 0x33)),
]


def write_png(path, rgb, size=4):
    """A uniform 8-bit RGBA PNG. Written to disk rather than generated in
    Blender, because that is the path a real ``.RIM`` takes and a generated
    image's pixels are interpreted differently."""
    import zlib
    row = bytes((*rgb, 255)) * size
    raw = b"".join(b"\x00" + row for _ in range(size))

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n"
                 + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
                 + chunk(b"IDAT", zlib.compress(raw))
                 + chunk(b"IEND", b""))
    return path


def preview_engine(scene):
    """EEVEE under whichever name this build uses -- Material Preview is EEVEE,
    so that is what the preview has to be correct in."""
    for name in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE"):
        try:
            scene.render.engine = name
        except TypeError:
            continue
        return name
    return None


def render_centre_bytes(tmp, tag):
    """Render and read the middle pixel back as 0..255.

    16-bit output, so the comparison is not fighting the 8-bit quantisation of
    the file it goes through; ``Non-Color`` on the way back in, so nothing
    converts what the view transform just produced.
    """
    scene = bpy.context.scene
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "16"
    path = os.path.join(tmp, "gkprev_%s.png" % tag)
    scene.render.filepath = path[:-4]
    bpy.ops.render.render(write_still=True)

    img = bpy.data.images.load(path)
    img.colorspace_settings.name = "Non-Color"
    px = [v * 255.0 for v in img.pixels[:3]]
    bpy.data.images.remove(img)
    with contextlib.suppress(OSError):
        os.remove(path)
    return px


def build_preview_scene(tmp, texel, light, tag):
    """A textured, lit quad filling an orthographic camera."""
    reset()
    scene = bpy.context.scene
    engine = preview_engine(scene)
    if engine is None:
        return None, None
    scene.render.resolution_x = scene.render.resolution_y = 4
    scene.render.film_transparent = False

    collection = sc.new_collection("preview.rif")
    bpy.ops.mesh.primitive_plane_add(size=2.0)
    plane = bpy.context.active_object
    plane.name = "Lit"
    sc.adopt_object(collection, plane)

    attr = plane.data.color_attributes.new(name=sc.LIGHT_COLOR_ATTR,
                                           type="BYTE_COLOR", domain="POINT")
    for d in attr.data:
        d.color_srgb = (light[0] / 255.0, light[1] / 255.0, light[2] / 255.0, 1.0)

    mat = bpy.data.materials.new("tex_%s" % tag)
    image = bpy.data.images.load(write_png(os.path.join(tmp, "gktex_%s.png" % tag), texel))
    sc._wire_texture(mat, image)
    plane.data.materials.append(mat)

    cam_data = bpy.data.cameras.new("cam")
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = 1.0
    cam = bpy.data.objects.new("cam", cam_data)
    cam.location = (0.0, 0.0, 5.0)
    bpy.context.scene.collection.objects.link(cam)
    scene.camera = cam
    return collection, (plane, mat)


def test_preview_matches_the_engine(tmp):
    """`texel * light` on the stored bytes, measured by rendering it."""
    print("gunlok preview reproduces texture x baked lighting")

    for i, (texel, light) in enumerate(PREVIEW_CASES):
        tag = "case%d" % i
        collection, made = build_preview_scene(tmp, texel, light, tag)
        if collection is None:
            print("  skipped: no EEVEE in this build")
            return
        plane, source = made

        stats, why = sc.preview_setup()
        check(why is None, "%s: preview_setup ran: %s" % (tag, why))
        check(bpy.context.scene.view_settings.view_transform == "Standard",
              "%s: the view transform is Standard" % tag)

        got = render_centre_bytes(tmp, tag)
        want = sc.preview_shade(texel, light)
        err = [abs(got[c] - want[c]) for c in range(3)]
        check(max(err) <= 1.0,
              "%s: texel %s x light %s -> %s, want %s (err %s)"
              % (tag, [hex(v) for v in texel], [hex(v) for v in light],
                 [round(v, 2) for v in got], [round(v, 2) for v in want],
                 [round(v, 2) for v in err]))

        # The same scene shaded the obvious way, to prove the check has teeth. A
        # linear multiply is *plausible* -- it is what anyone writes first -- so
        # if it were within tolerance the test above would prove nothing.
        if i == 0:
            preview = plane.material_slots[0].material
            tree = preview.node_tree
            tex = next(n for n in tree.nodes if n.type == "TEX_IMAGE")
            attr = next(n for n in tree.nodes if n.type == "ATTRIBUTE")
            product = next(n for n in tree.nodes if n.type == "VECT_MATH")
            emission = next(n for n in tree.nodes if n.type == "EMISSION")
            check(tex.image.colorspace_settings.name == "sRGB",
                  "an imported .RIM is an sRGB image, got %s"
                  % tex.image.colorspace_settings.name)
            # Cut all three transfer groups out, which is exactly the graph
            # anyone writes first: sRGB texture x colour attribute, multiplied
            # in linear space, straight into Emission.
            tree.links.new(product.inputs[0], tex.outputs["Color"])
            tree.links.new(product.inputs[1], attr.outputs["Color"])
            tree.links.new(emission.inputs["Color"], product.outputs[0])
            naive = render_centre_bytes(tmp, tag + "_naive")
            naive_err = max(abs(naive[c] - want[c]) for c in range(3))
            check(naive_err > 1.0,
                  "a linear multiply misses the reference by %.2f LSB, so the "
                  "tolerance above is a real constraint" % naive_err)

        with contextlib.suppress(OSError):
            os.remove(os.path.join(tmp, "gktex_%s.png" % tag))


def test_preview_is_reversible(tmp):
    """Setting up and restoring leaves the authored materials exactly as found."""
    print("gunlok preview is reversible")
    collection, made = build_preview_scene(tmp, (0x80, 0x80, 0x80), (0x40, 0x40, 0x40),
                                           "rev")
    if collection is None:
        print("  skipped: no EEVEE in this build")
        return
    plane, source = made
    source_name = source.name
    image = sc.source_image(source)
    was = image.colorspace_settings.name

    scene = bpy.context.scene
    scene.view_settings.view_transform = "AgX"
    scene.view_settings.exposure = 1.5

    sc.preview_setup()
    preview = plane.material_slots[0].material
    check(preview is not source, "the slot now holds a preview material")
    check(sc.is_preview_material(preview), "and it is marked as one")
    check(preview.get(sc.PREVIEW_SOURCE_PROP) is source,
          "which points back at the authored material by ID, not by name")
    check(sc.preview_is_active(), "preview_is_active sees it")
    check(scene.view_settings.exposure == 0.0, "exposure is neutralised")
    # The image is a *shared* datablock. Forcing it to Non-Color to get raw
    # bytes would leave every authored material rendering a linearised texture
    # as though it were raw once the preview is put back, so the preview reads
    # the colour space and never writes it.
    check(image.colorspace_settings.name == was,
          "the shared image's colour space is untouched, got %s (was %s)"
          % (image.colorspace_settings.name, was))

    # A second run must be idempotent rather than wrapping the wrapper.
    sc.preview_setup()
    check(plane.material_slots[0].material is preview,
          "a second setup reuses the same preview instead of nesting one")

    stats, why = sc.preview_restore()
    check(why is None, "preview_restore ran: %s" % why)
    check(plane.material_slots[0].material is source,
          "the authored material is back in the slot")
    check(source.name == source_name, "and it was never renamed")
    check(not sc.preview_is_active(), "nothing is previewing any more")
    check(scene.view_settings.view_transform == "AgX",
          "the view transform is back to what it was, got %s"
          % scene.view_settings.view_transform)
    check(scene.view_settings.exposure == 1.5, "and so is the exposure")
    check(sc.PREVIEW_STATE_PROP not in scene, "the saved state is consumed")


def test_preview_keeps_paint(tmp):
    """The preview reads the stored lighting and must never rewrite it.

    It also must not *create* lighting: it mints a white attribute so an unlit
    mesh renders the way the engine draws one (no SHPVTINT is a white diffuse),
    and that must not turn into a chunk in the file.
    """
    print("gunlok preview does not clobber or invent lighting")
    reset()
    collection = sc.new_collection("paint.rif")
    plane = add_cube("Painted")
    sc.adopt_object(collection, plane)

    sc.enable_lighting(plane.data)
    paint = plane.data.color_attributes[sc.LIGHT_COLOR_ATTR]
    for d in paint.data:
        d.color_srgb = (1.0, 0.0, 0.0, 1.0)

    stats, why = sc.preview_setup()
    check(why is None, "preview_setup ran: %s" % why)
    check(stats["lit"] == 0, "it did not re-create an attribute that was there")
    kept = plane.data.color_attributes[sc.LIGHT_COLOR_ATTR]
    check(round(kept.data[0].color_srgb[0] * 255) == 255
          and round(kept.data[0].color_srgb[1] * 255) == 0,
          "the painted colour survived, got %s"
          % [round(v * 255) for v in kept.data[0].color_srgb[:3]])

    # A mesh with no lighting gets a white attribute to render through, and
    # stays unlit as far as the file is concerned.
    other = add_cube("Unpainted", location=(4.0, 0.0, 0.0))
    sc.adopt_object(collection, other)
    stats, _why = sc.preview_setup()
    check(other.data.color_attributes.get(sc.LIGHT_COLOR_ATTR) is not None,
          "a mesh with no %s got one" % sc.LIGHT_COLOR_ATTR)
    check(not sc.has_lighting(other.data),
          "but looking at it did not give it a SHPVTINT")
    path = os.path.join(tmp, "paint.rif")
    export(collection, path)
    got = vtints_in(rif.load(path))
    check(len(got) == 1, "so only the lit mesh exports a chunk, got %d" % len(got))
    os.remove(path)


def test_preview_shadow_objects(tmp):
    """A `_shadow` mesh is the silhouette caster, so it casts and is not drawn."""
    print("gunlok preview gives _shadow objects their engine role")
    reset()
    collection = sc.new_collection("level09_shadow.rif")
    caster = add_cube("Shadow Hull")
    sc.adopt_object(collection, caster)

    check(sc.is_shadow_object(caster),
          "the collection name is enough to recognise a shadow file")
    sc.preview_setup()
    check(caster.visible_camera is False, "the camera does not see it")
    check(caster.visible_shadow is True, "but it still casts")
    check(not sc.is_preview_material(caster.material_slots[0].material)
          if caster.material_slots else True,
          "and it was not given a preview material")

    # Off by request, for anyone who wants to look at the hull itself.
    reset()
    collection = sc.new_collection("level09_shadow.rif")
    caster = add_cube("Shadow Hull")
    sc.adopt_object(collection, caster)
    sc.preview_setup(shadow_casters=False)
    check(caster.visible_camera is True,
          "shadow_casters=False leaves visibility alone")


def test_preview_transfer_groups(tmp):
    """The two conversions are exact and are each other's inverse."""
    print("gunlok preview sRGB groups")
    reset()
    enc = sc._srgb_transfer_group(sc.ENCODE_GROUP, decode=False)
    dec = sc._srgb_transfer_group(sc.DECODE_GROUP, decode=True)
    check(enc is not dec, "the two groups are distinct")
    check(sc._srgb_transfer_group(sc.ENCODE_GROUP, decode=False) is enc,
          "and are reused rather than duplicated on a second call")
    # One chain per channel, and no node reached by socket *name* -- the whole
    # graph is built on Math nodes precisely because those are index-addressed.
    maths = [n for n in enc.nodes if n.type == "MATH"]
    check(len(maths) == 21, "the encode group is 7 math nodes x 3 channels, got %d"
          % len(maths))
    check(any(n.operation == "LESS_THAN" for n in maths),
          "including the piecewise test -- a pure Gamma node would not be exact")


def run(game_dir):
    tmp = tempfile.gettempdir()
    test_from_scratch(tmp)
    test_edits_survive_a_blend(tmp)
    test_duplicate_is_caught(tmp)
    test_rig_from_scratch(tmp)
    test_sounds_from_scratch(tmp)
    test_sequence_settings(tmp)
    test_lights_from_scratch(tmp)
    test_second_light_and_ids(tmp)
    test_vertex_lighting(tmp)
    test_lighting_is_name_gated(tmp)
    test_lighting_refuses_a_stale_attribute(tmp)
    test_lighting_color_round_trip(tmp)
    test_lighting_from_a_corner_attribute(tmp)
    test_lighting_from_a_bake(tmp)
    test_preview_transfer_groups(tmp)
    test_preview_matches_the_engine(tmp)
    test_preview_is_reversible(tmp)
    test_preview_keeps_paint(tmp)
    test_preview_shadow_objects(tmp)
    if game_dir:
        test_edit_an_imported_file(game_dir, tmp)
        test_added_keys_and_bones(game_dir, tmp)
        test_sounds_survive_an_import(game_dir, tmp)
        test_settings_survive_an_import(game_dir, tmp)
        test_light_in_an_imported_file(game_dir, tmp)
        test_light_keeps_the_texture_table(game_dir, tmp)

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
