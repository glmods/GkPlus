"""The preview path, and the de-light restriction the pictures forced.

    uv run python tests/test_preview.py

Two things are checked here, and they are related by the same measurement.

**The preview** (:mod:`gkpbr.preview`) is what turns a generated PNG into pixels
in the running game: pack it as a ``.RIM`` and drop it into a mod so the engine
loads it in place of the sheet it came from. Nothing here launches Gunlok or runs
``rimutil`` -- what is checkable without either is where the file goes, what
encoding it goes through, and that the cleanup really deletes it. That last one
is not a formality: a leftover mod silently changes what the game draws in every
later session and the game looks *fine*, which is the failure commit 6655629
spent a session chasing.

**The de-light restriction** (:func:`derive.delight_mask`) is here rather than in
``test_pipeline.py`` because it exists for the same reason the preview does --
somebody looked. Baked lighting in Gunlok's set is a per-region property of a
minority of regions, so de-lighting a whole sheet because one pod on it is
shaded takes contrast off the twenty flat-lit photographic plates beside it.

Nothing here reaches the network, the game, or the install.
"""

import os
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from gkpbr import derive, preview  # noqa: E402

FAILURES = []


def check(name, ok, detail=""):
    print("  %-58s %s%s" % (name, "ok" if ok else "FAIL", "  " + detail if detail else ""))
    if not ok:
        FAILURES.append(name)


class FakeIndex:
    """A ``rim.TextureIndex`` with one entry, spelled the way the install spells it."""

    def __init__(self, root, mapping):
        self.root = root
        self._m = {k.lower(): v for k, v in mapping.items()}

    def resolve(self, name):
        return self._m.get(name.replace("\\", "/").lstrip("/").lower())


def test_mod_path_is_under_graphics():
    got = preview.mod_relative_path("ground/city ruins ground 1_a.rim")
    check("a manifest key joins straight onto Graphics/",
          got == "Graphics/ground/city ruins ground 1_a.rim", got)
    # A backslash-spelled name is the same file: the engine spells a bound texture
    # however whatever asked for it spelled one (renderstate.normalise has the
    # same trap), so this must not produce a directory called
    # "Ground\city ruins...".
    got = preview.mod_relative_path("Ground\\rock2.RIM")
    check("a backslash name becomes a real subdirectory",
          got == "Graphics/Ground/rock2.RIM", got)


def test_mod_path_prefers_the_shipped_casing():
    index = FakeIndex(r"C:\game\Graphics",
                      {"ground/city ruins ground 1_a.rim":
                       r"C:\game\Graphics\Ground\city ruins ground 1_a.RIM"})
    got = preview.mod_relative_path("ground/city ruins ground 1_a.rim", index)
    check("the index supplies the install's own casing",
          got == "Graphics/Ground/city ruins ground 1_a.RIM", got)
    # A name the index cannot resolve is still a usable path -- the VFS folds
    # case, so the lowercase form works; it just is not the pretty one.
    got = preview.mod_relative_path("ground/not shipped.rim", index)
    check("an unresolvable name still yields a path",
          got == "Graphics/ground/not shipped.rim", got)


def test_target_path_lands_in_the_mods_tree():
    got = preview.target_path(r"C:\game", "ground/rock2.rim", "m")
    parts = got.replace("/", os.sep).split(os.sep)
    check("path goes <game>/gkplus/mods/<mod>/Graphics/...",
          parts[-6:] == ["gkplus", "mods", "m", "Graphics", "ground", "rock2.rim"], got)


def test_normal_maps_do_not_go_through_s3tc():
    # The measurement behind this is in preview.DEFAULT_FORMATS: DXT1 and DXT3
    # are the same picture for a map with no alpha, and both cost 2.53 deg of
    # normal-vector error against 1.15 for `body` on a 16-bit surface.
    check("normal defaults to body", preview.format_for("normal") == "body")
    check("roughness defaults to dxt1", preview.format_for("roughness") == "dxt1")
    check("an explicit format wins", preview.format_for("normal", "dxt3") == "dxt3")
    check("an unknown map name still answers", preview.format_for("nonsense") == "dxt1")


def test_remove_deletes_the_whole_mod_and_only_it():
    with tempfile.TemporaryDirectory() as game:
        mine = os.path.join(game, "gkplus", "mods", preview.DEFAULT_MOD, "Graphics")
        theirs = os.path.join(game, "gkplus", "mods", "someone-elses", "RIF")
        os.makedirs(mine)
        os.makedirs(theirs)
        open(os.path.join(mine, "a.RIM"), "w").close()
        open(os.path.join(theirs, "b.rif"), "w").close()

        gone = preview.remove(game)
        check("remove reports what it deleted", gone is not None)
        check("the preview mod is gone",
              not os.path.exists(os.path.join(game, "gkplus", "mods",
                                              preview.DEFAULT_MOD)))
        check("another mod is untouched", os.path.exists(os.path.join(theirs, "b.rif")))
        check("removing twice is not an error", preview.remove(game) is None)


def test_pack_asks_rimutil_for_the_right_thing():
    """The argv, without running anything: `compress` is the whole contract."""
    import subprocess

    seen = {}

    class Result:
        returncode = 0
        stdout = "DXT1 8x8, 32 bytes"
        stderr = ""

    real = subprocess.run
    subprocess.run = lambda argv, **kw: (seen.update(argv=argv), Result())[1]
    try:
        with tempfile.TemporaryDirectory() as tmp:
            dest = os.path.join(tmp, "deep", "Graphics", "Ground", "x.RIM")
            said = preview.pack("rimutil.exe", "in.png", dest, "body")
            # Inside the context manager: rimutil writes the file itself, so the
            # only thing pack owes it is the directory, and checking after the
            # TemporaryDirectory is gone would always fail.
            check("the destination directory is created",
                  os.path.isdir(os.path.dirname(dest)))
    finally:
        subprocess.run = real

    argv = seen.get("argv", [])
    check("argv is compress in -> out with the format",
          argv[:3] == ["rimutil.exe", "compress", "in.png"] and argv[4:] ==
          ["--format", "body"], " ".join(argv))
    check("stdout comes back for the operator", said.startswith("DXT1"))


def test_pack_raises_rather_than_writing_nothing_quietly():
    import subprocess

    class Bad:
        returncode = 1
        stdout = ""
        stderr = "DXT needs both dimensions to be a multiple of 4"

    real = subprocess.run
    subprocess.run = lambda argv, **kw: Bad()
    try:
        with tempfile.TemporaryDirectory() as tmp:
            try:
                preview.pack("rimutil.exe", "in.png", os.path.join(tmp, "x.RIM"), "dxt1")
                raised = ""
            except RuntimeError as exc:
                raised = str(exc)
    finally:
        subprocess.run = real
    check("a rimutil failure raises with its own message",
          "multiple of 4" in raised, raised[:60])


# --- the de-light restriction ------------------------------------------------

#: Big enough that ``derive.high_pass(sigma=24)`` has something to blur: at 32
#: the box window is wider than the image, every low-frequency estimate collapses
#: to the global mean, and the de-lighter's gain comes out at 1.0 everywhere --
#: so a restriction test on a small sheet passes for the wrong reason.
SHEET = 256


def _two_regions():
    """One sheet, region 1 on the left, region 2 on the right."""
    labels = np.zeros((SHEET, SHEET), dtype=np.int32)
    labels[:, :SHEET // 2] = 1
    labels[:, SHEET // 2:] = 2
    return labels


def test_delight_mask_selects_only_the_regions_that_asked():
    labels = _two_regions()
    materials = {"1": {"delight": True}, "2": {"delight": False}}
    mask = derive.delight_mask(labels, materials)
    half = SHEET // 2
    check("mask is the asking region only",
          mask is not None and mask[:, :half].all() and not mask[:, half:].any())

    none_ask = derive.delight_mask(labels, {"1": {}, "2": {}})
    check("nothing asking gives an all-false mask",
          none_ask is not None and not none_ask.any())

    both = derive.delight_mask(labels, {"1": {"delight": True}, "2": {"delight": True}})
    check("everything asking collapses to None (whole sheet)", both is None)

    check("an unsegmented sheet is None", derive.delight_mask(None, {"0": {}}) is None)


def test_delight_leaves_the_regions_that_did_not_ask_byte_identical():
    """The whole point: a flat-lit photographic plate must come out untouched."""
    rng = np.random.default_rng(7)
    albedo = rng.random((SHEET, SHEET, 4)).astype(np.float32)
    # A strong vertical gradient over the whole sheet, so an unrestricted
    # de-lighter would visibly move every pixel.
    albedo[..., :3] *= np.linspace(0.3, 1.0, SHEET)[:, None, None].astype(np.float32)
    labels = _two_regions()
    half = SHEET // 2

    out = derive.delight(albedo, labels, {"1": {"delight": True}, "2": {}})
    moved_left = float(np.abs(out[:, :half, :3] - albedo[:, :half, :3]).max())
    moved_right = float(np.abs(out[:, half:, :3] - albedo[:, half:, :3]).max())
    check("the asking region is de-lit", moved_left > 0.01, "%.4f" % moved_left)
    check("the other region is untouched", moved_right == 0.0, "%.6f" % moved_right)

    everywhere = derive.delight(albedo, labels,
                                {"1": {"delight": True}, "2": {"delight": True}})
    check("both asking still de-lights both",
          float(np.abs(everywhere[:, half:, :3] - albedo[:, half:, :3]).max()) > 0.01)


def test_apply_where_is_a_hard_edge_and_keeps_the_base():
    base = np.zeros((4, 4, 3), dtype=np.float32)
    repl = np.ones((4, 4, 3), dtype=np.float32)
    mask = np.zeros((4, 4), dtype=bool)
    mask[:2] = True
    got = derive.apply_where(base, repl, mask)
    check("replacement inside, base outside",
          got[:2].min() == 1.0 and got[2:].max() == 0.0)
    check("a None mask takes the replacement whole",
          derive.apply_where(base, repl, None).min() == 1.0)
    check("the base is not mutated", base.max() == 0.0)


def test_the_harness_can_fail():
    """CLAUDE.md's rule: a harness that cannot fail proves nothing."""
    before = len(FAILURES)
    check("deliberately failing assertion", 1 == 2)
    ok = len(FAILURES) == before + 1
    if ok:
        FAILURES.pop()
    print("  %-58s %s" % ("harness detects a failure", "ok" if ok else "FAIL"))
    if not ok:
        FAILURES.append("harness cannot fail")


def main():
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_"):
            continue
        print("%s:" % name)
        fn()
    print()
    if FAILURES:
        print("%d FAILED: %s" % (len(FAILURES), ", ".join(FAILURES)))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
