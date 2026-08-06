"""What the engine actually *does* with a sheet, measured from its own draw calls.

Stage 1 guesses at two things the running game can simply be asked: whether a
surface emits light, and whether it is a cut-out. GkPlus's D3D8 capture layer keeps
a log of every draw of the last complete frame -- the stage-0 texture's ``.rim``
path plus ``ALPHABLENDENABLE`` / ``SRCBLEND`` / ``DESTBLEND`` / ``ZWRITEENABLE`` /
``ALPHATESTENABLE`` and the rest -- and it is built from the shadow state, so it
exists under ``GKPLUS_RENDERER=d3d8`` as well as under the Vulkan renderer.
``utils/rendertest/harvest-draws.ps1`` accumulates it across a whole session and
:func:`from_harvest` folds that into :data:`PROFILE_PATH`.

**The granularity does not line up, and pretending it does would be worse than not
doing this at all.** A draw binds one texture for a whole primitive batch, so every
number here is per *sheet*; ``gkpbr``'s materials are per *region within* a sheet. A
1024 atlas holding a lamp housing and eleven other things is not "emissive" because
one draw of it was additive. So none of this overrides a per-region answer: it is
handed to the model as measured context beside the images (see
:func:`classify.build_prompt`), and it decides what an expensive run spends on
first. What it must not do is paint a texel.

Three further limits, all of which the README states in full:

- **A render state persists between draws.** ``ALPHATESTENABLE`` is not an attribute
  of the texture bound at the time; it is whatever the last caller left it as. A
  sheet alpha-tested on 0.3% of its draws (``units/baddies3.rim``) is telling you
  about its *neighbours* in the queue. That is why every field here is a count and
  the fractions are reported rather than thresholded into a boolean.
- **``SRCBLEND``/``DESTBLEND`` mean nothing while ``ALPHABLENDENABLE`` is 0**, and
  the game leaves stale factors there constantly -- the single most-drawn sheet in
  the game sits at ``SRCALPHA -> ONE`` with blending *off* for 132,752 draws. So
  :data:`ADDITIVE_DEST` is only consulted for a draw that is actually blending.
- **Absence is much weaker evidence than presence.** "Never observed" means this run
  did not see it, and a run is a finite set of levels and camera positions. Nothing
  here drops a texture; it only sorts one down.
"""

import collections
import json
import os
import re

#: Beside the package, not in ``GKPBR_OUT``: it is a *measurement of the game*, like
#: the manifest is a measurement of the shipped ``.rif``, and it is checked in so a
#: classify run does not need a machine that can launch Gunlok.
PROFILE_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                            "render_profile.json")

#: D3D8 destination blend factors under which a draw *adds* to the framebuffer rather
#: than replacing it: ``ONE`` (2), ``SRCCOLOR`` (3), ``DESTCOLOR`` (9). The ordinary
#: transparency lerp is ``SRCALPHA -> INVSRCALPHA`` (5 -> 6) and is not in here.
ADDITIVE_DEST = frozenset((2, 3, 9))

#: The engine spells a bound texture the way whatever asked for it spelled it:
#: ``units\Command Wheel 01 512.RIM`` from a ``BMPNAMES`` table, but
#: ``bitmaps\\LEVEL01.rim`` -- with the backslash doubled -- when a ``.gls`` supplied
#: the name. The manifest's key is lowercase and forward-slashed, so both fold onto
#: it and the doubled separator has to collapse or ``bitmaps//level01.rim`` reads as
#: a texture nothing on disk matches.
_SEPARATORS = re.compile(r"[\\/]+")


def normalise(path):
    """An engine-spelled texture path folded onto the manifest's key convention."""
    return _SEPARATORS.sub("/", path).strip().lower()


def load(path=None):
    """The checked-in profile, or ``None`` when there is none.

    ``None`` rather than an empty dict, because "no profile on this machine" and "the
    profile saw nothing" are different and only the second is evidence.
    """
    path = path or PROFILE_PATH
    if not os.path.exists(path):
        return None
    with open(path) as fh:
        return json.load(fh)


def textures(profile):
    return (profile or {}).get("textures") or {}


def entry(profile, name):
    """The record for one manifest texture name, or ``None`` if it was never drawn."""
    return textures(profile).get(normalise(name))


def _fraction(entry, key):
    draws = entry.get("draws") or 0
    return (entry.get(key, 0) / draws) if draws else 0.0


def describe(entry):
    """The measured facts about one sheet, as prompt lines -- or ``[]`` for none.

    Phrased as counts and percentages of *observed draws*, never as a verdict. The
    model is being given evidence to weigh against the images, not an answer to copy:
    a sheet blended on every draw is probably a transparency sheet, and a sheet
    blended on half of them is a unit drawn once opaque and once in the translucent
    pass, and only the pictures can say which regions of it are which.
    """
    if not entry or not entry.get("draws"):
        return []
    draws = entry["draws"]
    lines = ["Measured in the running game (%d draws observed across %d level%s):"
             % (draws, len(entry.get("levels", [])),
                "" if len(entry.get("levels", [])) == 1 else "s")]
    lines.append("- alpha blending was ON for %.0f%% of them" % (100 * _fraction(entry, "blend")))
    additive = _fraction(entry, "additive")
    if additive:
        lines.append("- %.0f%% ADD to the framebuffer instead of replacing it "
                     "(destination factor ONE): that fraction of this sheet is drawn "
                     "as a glow or an effect sprite, not as a surface" % (100 * additive))
    else:
        lines.append("- none of them add to the framebuffer, so no part of this sheet "
                     "is drawn as an additive glow")
    lines.append("- the alpha test was ON for %.0f%% and depth writes were OFF for %.0f%%"
                 % (100 * _fraction(entry, "alpha_test"), 100 * _fraction(entry, "zwrite_off")))
    lines.append("These are per-sheet: a draw binds one texture for a whole batch, so "
                 "they say nothing about which region within it is which. Weigh them "
                 "against the images; do not let them override what you can see. Note "
                 "also that these are render states, which persist between draws, so a "
                 "small percentage is more likely to be a neighbouring draw's setting "
                 "than a property of this sheet.")
    return lines


# ---------------------------------------------------------------------------
# building the profile from a harvest
# ---------------------------------------------------------------------------

#: ``hvlevel`` tags a phase, not a level: ``level02``, ``level02/briefing``,
#: ``level02/fx``. The profile records the level, since "seen in level02" is the
#: useful fact and "seen while the briefing was up" is an artifact of the harness.
def _level_of(tag):
    return tag.split("/", 1)[0]


def from_harvest(harvests, run=None):
    """Raw ``harvest-draws.ps1`` dumps -> the profile written to disk.

    A dump is per-texture ``{d, p, s, f, L, v}`` with ``s`` keyed by the seven render
    states as a comma-joined string; this flattens that into the counts the rest of
    the module reads, and drops ``<unnamed>`` into the run header rather than the
    texture table -- a draw whose texture the capture layer could not name is a fact
    about the *run*, and filing it under a texture name nothing on disk matches would
    put it in front of every join.

    Several dumps sum, because a session is one process and the accumulator dies with
    it: reaching a screen that needs a fresh launch, or a level that wedged the last
    one, means another dump rather than a longer one. Every field here is a count or
    a set, so summing is the whole merge.
    """
    if isinstance(harvests, dict):
        harvests = [harvests]
    out = {}
    unnamed = 0
    totals = collections.Counter()
    levels = set()
    for harvest in harvests:
        unnamed += _fold_one(harvest, out)
        for key in ("frames", "missed", "draws", "notex"):
            totals[key] += harvest.get(key) or 0
        levels.update(_level_of(t) for t in (harvest.get("levels") or {}))

    for rec in out.values():
        rec["levels"] = sorted(rec["levels"])

    header = dict(run or {})
    header.setdefault("harvests", len(harvests))
    header.setdefault("frames_sampled", totals["frames"])
    header.setdefault("frames_missed_while_sampling", totals["missed"])
    header.setdefault("draws_observed", totals["draws"])
    header["draws_with_no_texture"] = totals["notex"]
    header["draws_texture_unnamed"] = unnamed
    header["levels"] = sorted(levels)
    return {"run": header, "textures": out}


def _fold_one(harvest, out):
    """One dump's textures into ``out``; returns its ``<unnamed>`` draw count."""
    unnamed = 0
    for raw, rec in harvest.get("t", {}).items():
        name = normalise(raw)
        if name in ("<unnamed>", ""):
            unnamed += rec.get("d", 0)
            continue
        cur = out.setdefault(name, {"draws": 0, "primitives": 0, "blend": 0,
                                    "additive": 0, "alpha_test": 0, "zwrite_off": 0,
                                    "levels": set()})
        cur["draws"] += rec.get("d", 0)
        cur["primitives"] += rec.get("p", 0)
        for state, n in (rec.get("s") or {}).items():
            blend, _src, dest, _z, zwrite, _cull, atest = (int(x) for x in state.split(","))
            if blend:
                cur["blend"] += n
                if dest in ADDITIVE_DEST:
                    cur["additive"] += n
            if atest:
                cur["alpha_test"] += n
            if not zwrite:
                cur["zwrite_off"] += n
        cur["levels"].update(_level_of(tag) for tag in (rec.get("L") or {}))
    return unnamed


def summarise(profile, manifest=None):
    """Counts worth printing, and the two joins that are worth being noisy about."""
    tex = textures(profile)
    rows = list(tex.values())
    out = {
        "observed": len(tex),
        "never_blended": sum(1 for r in rows if not r["blend"]),
        "always_blended": sum(1 for r in rows if r["blend"] == r["draws"]),
        "any_additive": sum(1 for r in rows if r["additive"]),
        "additive_only": sum(1 for r in rows if r["draws"] and r["additive"] == r["draws"]),
        "any_alpha_test": sum(1 for r in rows if r["alpha_test"]),
        "always_alpha_test": sum(1 for r in rows if r["alpha_test"] == r["draws"]),
    }
    if manifest is not None:
        keys = {normalise(k) for k in manifest}
        out["in_manifest_seen"] = len(keys & set(tex))
        out["in_manifest_unseen"] = len(keys - set(tex))
        out["drawn_outside_manifest"] = sorted(set(tex) - keys)
    return out
