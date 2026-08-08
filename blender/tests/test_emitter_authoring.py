"""The two sound systems, exercised where they differ.

    blender --background --python blender/tests/test_emitter_authoring.py -- ["<Gunlok dir>"]

``INDSOUND`` and ``DUMOBJTX`` share nothing, and the addon now models them as two
different things -- a table on the collection, and Speakers in the level. This is
the test that both survive a ``.blend`` and that authoring either produces
something the engine could read.

Four claims, each checked against the chunk stream rather than the scene:

* **A shipped level's emitters come back byte for byte** through import ->
  ``.blend`` -> reset -> reopen -> export. That is not a formatting nicety: the
  1,097 shipped texts are irregular enough (a trailing CRLF, two of them, a
  directive run split across lines) that anything reformatting from parsed values
  would rewrite every one of them.
* **Editing one edits only it.** Dragging a Speaker's max distance rewrites that
  emitter's ``R`` argument and leaves every other byte -- and every other
  emitter -- alone.
* **An authored dummy is a shape the engine can load**: top level, with a
  ``DUMOBJDT``, with a non-empty name, and with uppercase directives, which is
  the trap that would otherwise be silent (the jump table @ 0x00481c10 skips a
  lowercase one without a word).
* **The gates refuse what the shipped data never contains** -- and a dummy with
  no ``DUMOBJDT`` is an access violation during level load, so it is refused
  before anything is written rather than discovered in the game.

Passing a Gunlok directory runs the first two; without one, only the authoring
half runs.
"""

import os
import sys
import tempfile

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "io_scene_rif"))

import emitters as em  # noqa: E402
import rif  # noqa: E402
import schema  # noqa: E402
from io_scene_rif import scene as sc  # noqa: E402

FAILURES = []
PASSES = [0]

#: Levels worth using: level01 is the one every other in-game measurement is on,
#: junkyard is the densest at 87 emitters, and level06 carries the single shipped
#: text whose directives are split across two lines with a leading space.
LEVELS = ("level01.RIF", "junkyard.RIF", "level06.RIF")

#: A file with an INDSOUND table, so the other system is exercised too.
TABLE_FILE = "biggerlob.rif"


def check(cond, msg):
    if cond:
        PASSES[0] += 1
    else:
        print("  FAIL " + msg)
        FAILURES.append(msg)


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def find(game_dir, name):
    for dp, _dirs, ns in os.walk(game_dir):
        for nm in ns:
            if nm.lower() == name.lower():
                return os.path.join(dp, nm)
    return None


def emitters_in(tree):
    """``{dummy name: (position, DUMOBJTX body)}`` for every emitter in a file."""
    out = {}
    for c in tree.walk():
        if c.id != b"DUMMYOBJ":
            continue
        tx = c.find(b"DUMOBJTX")
        dt = c.find(b"DUMOBJDT")
        if tx is None or dt is None:
            continue
        import struct
        out[sc._trailing_name(dt.body, sc.DUMOBJDT_NAME)] = (
            tuple(struct.unpack_from("<3i", dt.body, sc.DUMOBJ_LOCATION)),
            bytes(tx.body))
    return out


def sounds_in(tree):
    return sorted(bytes(c.body) for c in tree.walk() if c.id == b"INDSOUND")


def dummies_in(tree):
    """Every top-level dummy: ``(name, has DUMOBJDT, has DUMOBJTX, depth)``."""
    out = []

    def walk(chunk, depth):
        for kid in chunk.children or ():
            if kid.id == b"DUMMYOBJ":
                dt = kid.find(b"DUMOBJDT")
                out.append((sc._trailing_name(dt.body, sc.DUMOBJDT_NAME) if dt else None,
                            dt is not None, kid.find(b"DUMOBJTX") is not None, depth))
            if kid.children is not None:
                walk(kid, depth + 1)

    walk(tree, 0)
    return out


def export(collection, stem="emitter_test"):
    root, stats = sc.rebuild_tree(collection)
    path = os.path.join(tempfile.gettempdir(), stem + ".rif")
    rif.save(path, root)
    out = rif.load(path)
    os.remove(path)
    return out, stats


def blend_round_trip(name):
    """Save, reset Blender, reopen -- the scene must stand on its own."""
    path = os.path.join(tempfile.gettempdir(), "emitter_test.blend")
    bpy.ops.wm.save_as_mainfile(filepath=path)
    bpy.ops.wm.read_homefile(use_empty=True)
    bpy.ops.wm.open_mainfile(filepath=path)
    os.remove(path)
    return bpy.data.collections.get(name)


# --------------------------------------------------------------------------


def test_shipped_levels(game_dir):
    """Import -> .blend -> reset -> reopen -> export, byte for byte."""
    for level in LEVELS:
        path = find(os.path.join(game_dir, "RIF"), level)
        if path is None:
            print("  skipped %s: not in the install" % level)
            continue
        print("round trip: %s" % level)

        want = emitters_in(rif.load(path))
        check(bool(want), "%s: has emitters to compare (%d)" % (level, len(want)))

        reset()
        name = os.path.basename(path)
        _coll, stats = sc.build_scene(rif.load(path), name, source_path=path)
        check(stats.get("emitters") == len(want),
              "%s: %d emitter(s) imported, expected %d"
              % (level, stats.get("emitters"), len(want)))

        reopened = blend_round_trip(name)
        check(reopened is not None, "%s: collection survived the .blend round trip" % level)
        if reopened is None:
            continue

        speakers = [o for o in sc.emitter_objects(reopened) if o.type == "SPEAKER"]
        check(len(speakers) == len(want),
              "%s: %d emitter(s) are Speakers after the reopen, expected %d"
              % (level, len(speakers), len(want)))

        tree, out_stats = export(reopened, "emitter_%s" % level)
        got = emitters_in(tree)
        check(out_stats["emitters"] == len(want),
              "%s: export wrote %d emitter(s), expected %d"
              % (level, out_stats["emitters"], len(want)))
        check(got == want,
              "%s: every emitter keeps its name, position and DUMOBJTX bytes" % level)
        if got != want:
            for key in sorted(set(want) | set(got)):
                if want.get(key) != got.get(key):
                    print("     %r: %r -> %r" % (key, want.get(key), got.get(key)))
                    break

        # The dummy inventory as a whole, not only the emitters: a marker that
        # lost its DUMOBJDT is the crash this addon must never write.
        before, after = dummies_in(rif.load(path)), dummies_in(tree)
        check(sorted(before) == sorted(after),
              "%s: all %d dummies keep their name, data chunk, text and depth"
              % (level, len(before)))
        check(all(d[1] for d in after),
              "%s: every exported dummy carries a DUMOBJDT" % level)
        check(all(d[3] == 0 for d in after),
              "%s: every exported dummy is at the file root" % level)


def test_editing_one_emitter(game_dir):
    """A Speaker edit rewrites one directive and nothing else."""
    path = find(os.path.join(game_dir, "RIF"), LEVELS[0])
    if path is None:
        return
    print("editing one emitter")
    reset()
    name = os.path.basename(path)
    coll, _stats = sc.build_scene(rif.load(path), name, source_path=path)
    want = emitters_in(rif.load(path))

    target = sc.emitter_objects(coll)[0]
    key = target["rif_name"]
    before = sc.emitter_text(target)
    target.data.distance_max = 123.0

    tree, _s = export(coll, "emitter_edit")
    got = emitters_in(tree)

    text = schema.decode(b"DUMOBJTX", got[key][1])["text"]
    check("R123" in text, "the edited emitter writes R123, got %r" % text)
    check(em.wav(text) == em.wav(before),
          "and its .wav is untouched (%r)" % em.wav(text))
    check(em.kind(text) == em.kind(before),
          "and line 1 keeps its own spelling (%r)" % em.kind(text))
    others = {k: v for k, v in got.items() if k != key}
    check(others == {k: v for k, v in want.items() if k != key},
          "every other emitter is byte-identical")

    # A directive the text did not carry is appended rather than the whole text
    # being rebuilt -- and only when it differs from what the engine would have
    # used anyway, which is what keeps "no radius" distinct from "radius 0".
    # Deliberately not `target`: it already carries an uncommitted R edit, and
    # comparing an appended directive against a text that also gained one proves
    # nothing. (This test failed exactly that way when it reused it.)
    plain = next((o for o in sc.emitter_objects(coll)
                  if o is not target and "I" not in em.values(sc.emitter_text(o))), None)
    if plain is not None:
        was = sc.emitter_text(plain)
        plain.data.distance_reference = 7.0
        now = sc.emitter_text_from_speaker(plain)
        check(now.startswith(was.rstrip("\r\n")) or now.startswith(was),
              "adding a directive keeps the existing text as a prefix: %r -> %r"
              % (was, now))
        check("I7" in now, "and appends it uppercase, got %r" % now)
        plain.data.distance_reference = 0.0
        check(sc.emitter_text_from_speaker(plain) == was,
              "and setting it back to the engine's default writes nothing")


def test_table_round_trip(game_dir):
    """The other system: the INDSOUND table on the collection."""
    path = find(os.path.join(game_dir, "RIF"), TABLE_FILE)
    if path is None:
        print("  skipped: no %s in the install" % TABLE_FILE)
        return
    print("round trip: %s (INDSOUND table)" % TABLE_FILE)
    want = sounds_in(rif.load(path))
    check(bool(want), "%s: has an INDSOUND table (%d)" % (TABLE_FILE, len(want)))

    reset()
    name = os.path.basename(path)
    _coll, stats = sc.build_scene(rif.load(path), name, source_path=path)
    check(stats.get("sounds") == len(want),
          "%d table entr(ies) imported, expected %d" % (stats.get("sounds"), len(want)))

    reopened = blend_round_trip(name)
    check(reopened is not None, "the collection survived the .blend round trip")
    if reopened is None:
        return
    entries = sc.sound_table(reopened)
    check(len(entries) == len(want),
          "the table is still %d entr(ies) after the reopen, got %d"
          % (len(want), len(entries)))
    # No Speaker anywhere: these have no position, and giving them one was the
    # whole thing this change removed.
    check(not [o for o in reopened.objects if o.type == "SPEAKER"],
          "an INDSOUND table creates no Speakers")

    tree, _s = export(reopened, "emitter_table")
    check(sounds_in(tree) == want, "the table rebuilds byte for byte")

    # Editing goes through the active row, which is what the UIList selects.
    reopened[sc.SOUND_ACTIVE_PROP] = 0
    sc.set_sound_field(reopened, "max_distance", 12345)
    tree, _s = export(reopened, "emitter_table")
    check(len(sounds_in(tree)) == len(want), "an edit changes no entry count")
    check(sounds_in(tree) != want, "and does reach the file")
    check(sum(1 for a, b in zip(sorted(sounds_in(tree)), sorted(want)) if a != b) <= 1,
          "and reaches exactly one entry")


def test_authoring_from_scratch():
    """A locator and an emitter built with no source file anywhere."""
    print("authoring from nothing")
    reset()
    coll = sc.new_collection("locators.rif")

    marker = bpy.data.objects.new("Goodie A1", None)
    bpy.context.scene.collection.objects.link(marker)
    why = sc.adopt_dummy(coll, marker)
    check(why is None, "an empty adopts as a locator (%s)" % why)
    marker.location = (1.0, 2.0, 3.0)

    obj, why = sc.add_emitter(coll, "GL_Wind03.wav", "SD_wind_test")
    check(obj is not None, "an emitter is created (%s)" % why)
    if obj is None:
        return
    obj.location = (4.0, 5.0, 6.0)
    obj.data.distance_max = 40.0
    obj.data.pitch = em.pitch_to_factor(2.0)

    errors, _warnings = sc.dummy_problems(coll)
    check(not errors, "an authored file has no dummy problems (%s)" % errors[:1])

    reopened = blend_round_trip("locators.rif")
    check(reopened is not None, "the authored collection survives a .blend round trip")
    if reopened is None:
        return

    tree, stats = export(reopened, "emitter_author")
    check(stats["emitters"] == 1, "one emitter written, got %d" % stats["emitters"])

    got = dummies_in(tree)
    check(len(got) == 2, "two dummies written, got %d" % len(got))
    check(all(d[3] == 0 for d in got), "both are at the file root")
    check(all(d[1] for d in got), "both carry a DUMOBJDT -- without one the level "
                                  "load dereferences NULL")
    check(sorted(d[0] for d in got) == ["Goodie A1", "SD_wind_test"],
          "with the names they were given, got %r" % sorted(d[0] for d in got))
    check([d[2] for d in sorted(got)] == [False, True],
          "and only the emitter carries a DUMOBJTX")

    text = schema.decode(b"DUMOBJTX", emitters_in(tree)["SD_wind_test"][1])["text"]
    check(em.is_emitter(text), "line 1 makes it a sound, got %r" % text)
    check(em.wav(text) == "GL_Wind03.wav", "line 2 names the wav, got %r" % text)
    # The trap: a lowercase directive parses fine to a reader and is skipped by
    # the engine in silence. All 1,540 shipped ones are uppercase.
    check(all(letter.isupper() for letter, _a, _s, _e in em.tokens(text)),
          "every directive is uppercase, got %r" % text)
    vals = em.values(text)
    check(vals.get("R") == 40.0, "R is the speaker's max distance, got %r" % vals)
    check(vals.get("P") == 2.0, "P is the speaker's pitch in semitones, got %r" % vals)
    check(not em.problems(text), "and the engine would act on all of it (%s)"
          % em.problems(text))

    # The position has to be the dummy's own, in rif units.
    where = emitters_in(tree)["SD_wind_test"][0]
    scale = reopened.get("rif_scale", sc.DEFAULT_SCALE)
    check(where == sc.to_rif((4.0, 5.0, 6.0), scale, True),
          "the emitter is at the Speaker's position, got %r" % (where,))


def test_gates():
    """The measured refusals, each of which the shipped data never violates."""
    print("the authoring gates")
    reset()
    coll = sc.new_collection("gates.rif")

    mesh = bpy.data.objects.new("mesh", bpy.data.meshes.new("m"))
    bpy.context.scene.collection.objects.link(mesh)
    check(sc.adopt_dummy(coll, mesh) is not None,
          "a mesh is refused as a dummy -- a locator has no geometry")

    blank = bpy.data.objects.new("blank", None)
    bpy.context.scene.collection.objects.link(blank)
    check(sc.adopt_dummy(coll, blank, "  ") is not None,
          "an empty name is refused: it is stored as NULL and never resolves")

    parent = bpy.data.objects.new("parent", None)
    bpy.context.scene.collection.objects.link(parent)
    sc.adopt_object(coll, parent)
    child = bpy.data.objects.new("child", None)
    bpy.context.scene.collection.objects.link(child)
    child.parent = parent
    check(sc.adopt_dummy(coll, child) is not None,
          "a nested dummy is refused: the collector never recurses")

    ok = bpy.data.objects.new("Flag_1", None)
    bpy.context.scene.collection.objects.link(ok)
    check(sc.adopt_dummy(coll, ok) is None, "a named top-level empty is accepted")

    # And the one that has to be caught at export, because it is about what was
    # written rather than what was asked for.
    del ok["rif_dumobjdt"]
    errors, _w = sc.dummy_problems(coll)
    check(any("DUMOBJDT" in e for e in errors),
          "a dummy that lost its DUMOBJDT is refused before export, got %r" % errors)

    ok["rif_dumobjdt"] = [0] * 13
    twin = bpy.data.objects.new("Flag_1.001", None)
    bpy.context.scene.collection.objects.link(twin)
    sc.adopt_dummy(coll, twin, "Flag_1")
    errors, warnings = sc.dummy_problems(coll)
    check(not errors, "a duplicate name is not an error (%r)" % errors)
    check(any("named" in w for w in warnings),
          "but it is a warning: the console takes the first match, triggers the "
          "last -- got %r" % warnings)

    # An RBOBJECT is still what a `for "<rif object>"` spawn point needs, so
    # `adopt_object` must keep refusing to make dummies out of speakers.
    spk = bpy.data.objects.new("spk", bpy.data.speakers.new("spk"))
    bpy.context.scene.collection.objects.link(spk)
    check(sc.adopt_object(coll, spk) is not None,
          "adopt_object refuses a Speaker and points at the emitter operator")


def test_ui_registers():
    """The panel is the editing surface, so "it registers" is part of the claim.

    The table is a plain ID property, not a registered ``CollectionProperty`` --
    which is what lets every other test drive :mod:`scene` with the addon
    unregistered -- so it is absent from ``bl_rna.properties`` and reachable only
    through the bracket form. ``template_list`` accepts that form and then
    **crashes Blender 5.2** on it (a null dereference in
    ``RNA_property_collection_length``), so the panel draws its rows itself and
    selection goes through an operator. That is what this checks; drawing needs a
    real region and was confirmed against a running Blender by hand.
    """
    print("registration and the table's RNA binding")
    reset()
    import io_scene_rif

    try:
        io_scene_rif.register()
    except Exception as exc:  # noqa: BLE001
        check(False, "the addon registers: %r" % exc)
        return
    try:
        coll = sc.new_collection("ui.rif")
        sc.add_sound(coll, "Robots" + chr(92) + "GL_click08.wav")
        sc.add_sound(coll, "Robots" + chr(92) + "CLUNK1.WAV")

        check(sc.SOUND_TABLE_PROP not in coll.bl_rna.properties,
              "the table stays an ID property, so scene.py needs no registration")
        check(len(sc.sound_table(coll)) == 2,
              "two entries, got %d" % len(sc.sound_table(coll)))

        # Selection is an operator because the rows are drawn by hand.
        bpy.ops.scene.rif_select_sound(index=1, collection=coll.name)
        check(int(coll[sc.SOUND_ACTIVE_PROP]) == 1,
              "the select operator moves the active row")

        # The accessors the panel draws, which read and write the active row.
        check(coll.rif_sound_slot == 2, "the panel reads slot 2, got %d" % coll.rif_sound_slot)
        coll.rif_sound_max_distance = 4321
        check(sc.sound_table(coll)[1]["max_distance"] == 4321,
              "and writing one reaches the table")
        coll.rif_sound_path = "environ" + chr(92) + "x.wav"
        check(sc.sound_table(coll)[1]["path"] == "environ" + chr(92) + "x.wav",
              "including the path")

        obj, why = sc.add_emitter(coll, "GL_Wind03.wav", "SD_ui")
        check(obj is not None, "an emitter is created with the addon registered (%s)" % why)
        if obj is not None:
            obj.rif_emitter_wav = "t_hum.wav"
            check(em.wav(sc.emitter_text(obj)) == "t_hum.wav",
                  "and its wav accessor rewrites line 2, got %r" % sc.emitter_text(obj))
    finally:
        io_scene_rif.unregister()


def test_controls():
    """Every check above must be able to fail; these are the ones that prove it."""
    print("controls")
    before = len(FAILURES)
    check(em.retext("Sound\r\nx.wav\r\nR20", values_by_letter={"R": 99})
          == "Sound\r\nx.wav\r\nR20",
          "control: a changed directive must not leave the text alone")
    check(em.is_emitter("Music\r\nx.wav"),
          "control: a non-Sound line 1 must not read as an emitter")
    check(not em.problems("Sound\r\nx.wav\r\nv20"),
          "control: a lowercase directive must be reported")
    fired = len(FAILURES) - before
    for _ in range(fired):
        FAILURES.pop()
    print("  controls fired: %d of 3" % fired)
    return fired == 3


def main(game_dir):
    if game_dir:
        test_shipped_levels(game_dir)
        test_editing_one_emitter(game_dir)
        test_table_round_trip(game_dir)
    else:
        print("no Gunlok directory given; running the authoring half only\n")
    test_authoring_from_scratch()
    test_gates()
    test_ui_registers()
    controls = test_controls()

    print("\n%s" % ("-" * 60))
    print("%d checks passed" % PASSES[0])
    if FAILURES:
        print("%d CHECK(S) FAILED" % len(FAILURES))
        for f in FAILURES[:20]:
            print("   %s" % f)
    else:
        print("all checks passed")
    if not controls:
        print("CONTROLS DID NOT ALL FIRE -- the checks above prove nothing")
    return 1 if FAILURES or not controls else 0


if __name__ == "__main__":
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    sys.exit(main(argv[0] if argv else ""))
