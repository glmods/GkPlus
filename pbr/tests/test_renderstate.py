"""The render-state profile: what it folds, what it says, and what it must not do.

    uv run python tests/test_renderstate.py

The profile is evidence from the running game -- ``render.frame_draws()``, merged
across a whole session by ``utils/rendertest/harvest-draws.ps1`` -- and its whole
risk is granularity. A draw binds one texture for a batch, so every number in it is
per *sheet*, while ``gkpbr``'s materials are per *region within* a sheet. So the
checks here are in two groups: that the fold is arithmetically right (including the
one trap that makes it wrong-looking), and that the result reaches the model as
context and reaches ``derive`` not at all.

The trap, first, because it is the reason :func:`renderstate.from_harvest` is not a
one-liner: **``SRCBLEND``/``DESTBLEND`` mean nothing while ``ALPHABLENDENABLE`` is
0**, and the game leaves stale factors in them constantly. ``units/plates 2
1024.rim`` -- the game's third most-drawn sheet, and the HUD -- sits at
``SRCALPHA -> ONE`` with blending off for 134,325 of its draws. Counting those as
additive would make the HUD the most emissive thing Gunlok owns.

Nothing here reaches the network or the game.
"""

import copy
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from gkpbr import classify, cli, renderstate  # noqa: E402

FAILURES = []


def check(name, ok, detail=""):
    print("  %-58s %s%s" % (name, "ok" if ok else "FAIL", "  " + detail if detail else ""))
    if not ok:
        FAILURES.append(name)


# One texture drawn four ways, which is every case the fold has to tell apart:
#   opaque, with stale SRCALPHA->ONE factors sitting in the shadow state;
#   the ordinary transparency lerp;
#   a real additive draw, depth writes off;
#   the same but with the alpha test on.
# state key is "blend,src,dst,z,zwrite,cull,atest", the harness's own encoding.
HARVEST = {
    "frames": 1000, "missed": 3, "draws": 40, "notex": 7,
    "levels": {"level02": 600, "level02/fx": 380, "level01": 20},
    "t": {
        "Ground\\Test Sheet.RIM": {
            "d": 20, "p": 200,
            "s": {"0,5,2,1,1,3,0": 10,     # opaque, stale additive factors
                  "1,5,6,1,1,3,0": 5,      # lerp
                  "1,5,2,1,0,3,0": 3,      # additive, no z-write
                  "1,2,2,1,0,3,1": 2},     # additive, alpha test on
            "f": {"0x112": 20},
            "L": {"level02": 12, "level02/fx": 8},
            "v": {"0,0 640x480": 20},
        },
        "bitmaps\\\\Level01.rim": {
            "d": 5, "p": 10, "s": {"0,5,6,1,1,3,0": 5}, "f": {}, "L": {"level01": 5},
            "v": {},
        },
        "<unnamed>": {
            "d": 15, "p": 30, "s": {"0,5,6,1,1,3,0": 15}, "f": {}, "L": {"level02": 15},
            "v": {},
        },
    },
}

RECORD = {"name": "ground/test sheet.rim", "width": 256, "height": 256,
          "format": "DXT1", "alpha": False, "rif_count": 3, "polys": 900,
          "lum_mean": 120.0, "sat_mean": 0.2, "patches": []}


def test_normalise():
    """The engine spells a path however the caller that asked for it spelled one."""
    check("backslashes fold to forward",
          renderstate.normalise("units\\Command Wheel 01 512.RIM")
          == "units/command wheel 01 512.rim")
    # A .gls writes bitmap "bitmaps\\LEVEL02.rim" with the backslash DOUBLED, and the
    # engine passes it through, so the draw log holds `bitmaps\\LEVEL01.rim`. Without
    # the collapse this reads as a texture nothing on disk matches, and the level-map
    # bitmaps drop silently out of every join -- the same doubled-separator trap the
    # README already records for the gl.exe/scripts name search.
    check("a doubled separator collapses",
          renderstate.normalise("bitmaps\\\\LEVEL01.rim") == "bitmaps/level01.rim")
    check("already-normal names are unchanged",
          renderstate.normalise("ground/cracks.rim") == "ground/cracks.rim")


def test_fold_counts():
    profile = renderstate.from_harvest(copy.deepcopy(HARVEST))
    e = renderstate.entry(profile, "ground/test sheet.rim")
    check("the sheet is found under the manifest's key", e is not None)
    check("draws and primitives sum", e["draws"] == 20 and e["primitives"] == 200,
          "%s/%s" % (e["draws"], e["primitives"]))
    check("blended draws counted", e["blend"] == 10, str(e["blend"]))
    # THE trap: 10 opaque draws carry SRCALPHA->ONE in the shadow state and must not
    # be counted. Only the 3 + 2 that are actually blending are additive.
    check("stale blend factors under ALPHABLENDENABLE=0 are not additive",
          e["additive"] == 5, str(e["additive"]))
    check("alpha-tested draws counted", e["alpha_test"] == 2, str(e["alpha_test"]))
    check("depth-write-off draws counted", e["zwrite_off"] == 5, str(e["zwrite_off"]))


def test_fold_levels_and_unnamed():
    profile = renderstate.from_harvest(copy.deepcopy(HARVEST))
    e = renderstate.entry(profile, "ground/test sheet.rim")
    # `hvlevel` tags a phase; "seen in level02" is the fact worth keeping and "seen
    # while the /fx battery ran" is an artifact of the harness.
    check("phase tags collapse to the level", e["levels"] == ["level02"],
          str(e["levels"]))
    # A draw whose texture the capture layer could not name is a fact about the run.
    # Filing it under a texture name puts a row nothing on disk matches at the top of
    # every join, where it looks like a finding.
    check("<unnamed> is not a texture row",
          renderstate.entry(profile, "<unnamed>") is None)
    check("<unnamed> draws are in the run header",
          profile["run"]["draws_texture_unnamed"] == 15)
    check("untextured draws are in the run header",
          profile["run"]["draws_with_no_texture"] == 7)
    check("the run's level list is the levels, not the phases",
          profile["run"]["levels"] == ["level01", "level02"],
          str(profile["run"]["levels"]))


def test_two_harvests_sum():
    """A session is one process: reaching a second screen means a second dump."""
    one = copy.deepcopy(HARVEST)
    two = {"frames": 10, "missed": 1, "draws": 4, "notex": 2,
           "levels": {"level04": 10},
           "t": {"Ground/Test Sheet.rim": {"d": 4, "p": 40,
                                           "s": {"1,5,2,1,0,3,0": 4}, "f": {},
                                           "L": {"level04": 4}, "v": {}}}}
    merged = renderstate.from_harvest([one, two])
    e = renderstate.entry(merged, "ground/test sheet.rim")
    check("draws sum across dumps", e["draws"] == 24, str(e["draws"]))
    check("additive sums across dumps", e["additive"] == 9, str(e["additive"]))
    check("levels union across dumps", e["levels"] == ["level02", "level04"],
          str(e["levels"]))
    check("the run header sums too",
          merged["run"]["frames_sampled"] == 1010
          and merged["run"]["frames_missed_while_sampling"] == 4,
          str(merged["run"]["frames_sampled"]))
    check("a bare dict is still accepted",
          renderstate.from_harvest(copy.deepcopy(HARVEST))["run"]["harvests"] == 1)


def test_describe_is_evidence_not_a_verdict():
    profile = renderstate.from_harvest(copy.deepcopy(HARVEST))
    text = "\n".join(renderstate.describe(renderstate.entry(profile, "ground/test sheet.rim")))
    check("names the additive fraction", "25%" in text, text[:90])
    check("says the numbers are per-sheet", "per-sheet" in text)
    check("warns that render states persist between draws", "persist between draws" in text)
    check("does not tell the model what to answer",
          "emissive" not in text.lower() and "roughness" not in text.lower())
    check("nothing to say about an unobserved sheet",
          renderstate.describe(None) == [] and renderstate.describe({"draws": 0}) == [])
    # The other branch: a sheet no draw of which ever added has to say so, because
    # that is the useful half of the evidence -- it is what makes "not emissive" the
    # prior for 272 of the 287 sheets.
    flat = {"draws": 4, "blend": 0, "additive": 0, "alpha_test": 0, "zwrite_off": 0,
            "levels": ["level02"]}
    check("an all-opaque sheet says so",
          "none of them add" in "\n".join(renderstate.describe(flat)))


def test_prompt_carries_it_and_the_fingerprint_follows():
    profile = renderstate.from_harvest(copy.deepcopy(HARVEST))
    obs = renderstate.entry(profile, RECORD["name"])
    bare = classify.build_prompt(RECORD, [])
    withit = classify.build_prompt(RECORD, [], None, obs)
    # A texture with no observation has to produce exactly the prompt it produced
    # before the profile existed, or adding the profile invalidates the cache for
    # every sheet the run never reached -- and charges for it.
    check("no observation leaves the prompt byte-identical",
          classify.build_prompt(RECORD, [], None, None) == bare)
    check("an observation adds the measured block", "Measured in the running game" in withit)
    check("the block is above the region instructions",
          withit.index("Measured in") < withit.index("one image"))

    fp_bare = classify.fingerprint(b"albedo", None, bare, "m")
    fp_with = classify.fingerprint(b"albedo", None, withit, "m")
    # Deliberately no new fingerprint key: the observation is *in* the prompt, so
    # re-harvesting the game goes stale by the same route a re-segmentation does, and
    # `cache.explain` already has words for it.
    check("re-harvesting shows up as a prompt change",
          fp_bare["prompt"] != fp_with["prompt"])
    check("and adds no fingerprint key", set(fp_bare) == set(fp_with))


class _Args:
    def __init__(self, **kw):
        self.level = None
        self.textures = []
        self.game = None
        self.seen_only = False
        self.__dict__.update(kw)


def test_selection_ranks_and_filters(tmp_profile):
    manifest = {"a/one.rim": {}, "a/two.rim": {}, "a/three.rim": {}}
    # `_select` reads the checked-in profile through `renderstate.load`, so the test
    # points that at a temporary one rather than at the repo's.
    real = renderstate.PROFILE_PATH
    renderstate.PROFILE_PATH = tmp_profile
    try:
        check("ranked by observed draws, most-drawn first",
              cli._select(_Args(), manifest) == ["a/two.rim", "a/one.rim", "a/three.rim"],
              str(cli._select(_Args(), manifest)))
        check("--seen-only drops the unobserved",
              cli._select(_Args(seen_only=True), manifest)
              == ["a/two.rim", "a/one.rim"])
        # Naming a texture is an instruction, not a suggestion: being told the sheet
        # you asked for was skipped for not having been observed would be absurd.
        check("an explicit name is never filtered or reordered",
              cli._select(_Args(textures=["a/Three.rim", "a/one.rim"], seen_only=True),
                          manifest) == ["a/three.rim", "a/one.rim"])
    finally:
        renderstate.PROFILE_PATH = real


def test_the_checked_in_profile():
    """The shipped measurement, checked for the properties every consumer assumes."""
    profile = renderstate.load()
    if profile is None:
        check("render_profile.json is present", False, "no profile checked in")
        return
    tex = renderstate.textures(profile)
    check("render_profile.json parses and has sheets", len(tex) > 100, str(len(tex)))
    check("every key is already normalised",
          all(k == renderstate.normalise(k) for k in tex))
    bad = [k for k, e in tex.items()
           if e["blend"] > e["draws"] or e["additive"] > e["blend"]
           or e["alpha_test"] > e["draws"]]
    check("no count exceeds the draw count it is a subset of", not bad, str(bad[:3]))
    check("the run says what produced it",
          bool(profile.get("run", {}).get("note")))
    # The three the previous item left open, and the whole reason for the harvest.
    # Presence, not absence: each is bound as a stage-0 texture on world geometry.
    for name in ("bitmaps/lava.rim", "bitmaps/oil.rim", "bitmaps/swamp.rim"):
        e = tex.get(name)
        check("%s was observed drawn" % name, bool(e and e["draws"]),
              "in %s" % (e["levels"] if e else "nothing"))


def test_the_harness_can_fail():
    """A harness that cannot report a failure proves nothing."""
    before = len(FAILURES)
    check("deliberate failure (expected FAIL on the line above)", False, "by design")
    ok = len(FAILURES) == before + 1
    FAILURES.pop()
    print("  %-58s %s" % ("harness detects a failure", "ok" if ok else "FAIL"))
    if not ok:
        FAILURES.append("harness cannot fail")


def _write_tmp_profile(path):
    with open(path, "w") as fh:
        json.dump({"run": {"note": "synthetic"},
                   "textures": {"a/one.rim": {"draws": 10, "primitives": 10, "blend": 0,
                                              "additive": 0, "alpha_test": 0,
                                              "zwrite_off": 0, "levels": ["l"]},
                                "a/two.rim": {"draws": 99, "primitives": 99, "blend": 0,
                                              "additive": 0, "alpha_test": 0,
                                              "zwrite_off": 0, "levels": ["l"]}}}, fh)


def main():
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        tmp_profile = os.path.join(tmp, "profile.json")
        _write_tmp_profile(tmp_profile)
        for name, fn in sorted(globals().items()):
            if not name.startswith("test_"):
                continue
            print("%s:" % name)
            if fn.__code__.co_argcount:
                fn(tmp_profile)
            else:
                fn()
    print()
    if FAILURES:
        print("%d FAILED: %s" % (len(FAILURES), ", ".join(FAILURES)))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
