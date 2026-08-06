"""The cache's honesty: a cached answer, checked against what produced it.

    uv run python tests/test_cache.py

Stage 1's JSON is what makes the pipeline re-runnable, and the claim rested on
"the input never changes". The input changes. Region ids are patch labels from
:func:`atlas.segment`, so ``MIN_AREA_TEXELS`` or the rasterizer moving re-points a
cached answer at different texels; the albedo comes from the addon's decoder, which
moved once already; and ``classify.SYSTEM`` has been edited once, which invalidated
every answer taken before it. None of that was detectable, because the cache key was
the **file name**.

So each case here is one input moving under a cached entry, and the property under
test is not "the hash differs" -- that is arithmetic -- but **what the pipeline does
about it**: no API call is made, the entry is left on disk, the input that moved is
named, and the run exits non-zero.

Nothing here reaches the network. :func:`classify.classify` takes a ``client`` for
exactly this reason, so the real request-building and response-parsing run against a
stub that counts calls, and a test that expects no spending fails loudly if the code
tries to spend.
"""

import contextlib
import io
import json
import os
import re
import shutil
import sys
import tempfile
import types as pytypes

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from gkpbr import atlas, cache, classify, cli, images  # noqa: E402

FAILURES = []

TEXTURE = "units/test.rim"
SLUG = "units__test"
SIZE = 64


def check(name, ok, detail=""):
    print("  %-58s %s%s" % (name, "ok" if ok else "FAIL", "  " + detail if detail else ""))
    if not ok:
        FAILURES.append(name)


def tail(out, n=150):
    """The end of a captured run, squeezed onto one line.

    These reports are several lines long and a raw slice of one into a detail column
    makes the harness output unreadable -- which matters, because the detail is what
    tells you *why* a check failed.
    """
    return " ".join(out.split())[-n:]


# ---------------------------------------------------------------------------
# A fake `inventory` output, and a fake model.
# ---------------------------------------------------------------------------

class StubClient:
    """Stands in for ``genai.Client``, answering every region the prompt asks for.

    It reads the ids back out of the rendered prompt rather than being told them, so
    a test that changes the segmentation gets an answer shaped like the new one --
    and ``omit`` can drop an id, which is how the MISSING reconciliation is provoked
    without hand-writing a materials file that no code path would have produced.
    """

    def __init__(self, omit=()):
        self.calls = []
        self.omit = {str(o) for o in omit}
        self.models = self

    def generate_content(self, *, model, contents, config):
        prompt = [c for c in contents if isinstance(c, str)][-1]
        ids = re.findall(r'id "(\d+)"', prompt) or ["0"]
        if "0" not in ids:
            ids.append("0")
        self.calls.append({"model": model, "prompt": prompt, "ids": ids})
        return pytypes.SimpleNamespace(text=json.dumps({"regions": [
            {"id": i, "material": "painted steel", "roughness": [0.3, 0.5],
             "roughness_drives": "constant", "metallic": 0.0, "height_scale": 0.1,
             "relief": "none, smooth", "delight": False,
             "emissive": {"present": False}, "confidence": 0.8}
            for i in ids if i not in self.omit]}))


def albedo_array(seed=0):
    rng = np.random.default_rng(seed)
    return rng.random((SIZE, SIZE, 3)).astype(np.float32)


def label_array(split=32):
    """Two regions, 1 above ``split`` and 2 below -- the shape `segment` produces."""
    labels = np.zeros((SIZE, SIZE), dtype=np.uint8)
    labels[:split, :] = 1
    labels[split:, :] = 2
    return labels


def record(name=TEXTURE, parts=("hull", "gun"), polys=(400, 120)):
    return {
        "name": name, "width": SIZE, "height": SIZE, "format": "DXT1", "alpha": False,
        "rif_count": 3, "polys": sum(polys), "tiling": False, "tiling_fraction": 0.0,
        "uv_samples": {}, "seam": [1.0, 1.0], "coverage": 1.0,
        "lum_mean": 120.0, "sat_mean": 0.2,
        "patches": [
            {"index": 0, "label": 1, "box": [0, 0, SIZE, 32], "area_texels": SIZE * 32,
             "polys": polys[0], "parts": [parts[0]], "rif_count": 3},
            {"index": 1, "label": 2, "box": [0, 32, SIZE, SIZE], "area_texels": SIZE * 32,
             "polys": polys[1], "parts": [parts[1]], "rif_count": 1},
        ],
    }


class Fixture:
    """An output directory as ``inventory`` would leave it, plus a stub model.

    ``cli.OUT`` is module state read through ``cli._paths``, so pointing it at a temp
    directory is the whole of the setup; nothing else in the CLI knows where it is.
    """

    def __init__(self, **kw):
        self.dir = tempfile.mkdtemp(prefix="gkpbr-cache-")
        self.manifest = {TEXTURE: record(**kw)}
        self.write_manifest()
        self.write_albedo(albedo_array())
        self.write_labels(label_array())
        self._out = cli.OUT
        self._classify = classify.classify
        cli.OUT = self.dir

    def write_manifest(self):
        cli._write_json(os.path.join(self.dir, "manifest.json"), self.manifest)

    def write_albedo(self, arr):
        path = os.path.join(self.dir, "albedo", SLUG + ".png")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        images.save(path, arr)

    def write_labels(self, labels):
        path = os.path.join(self.dir, "labels", SLUG + ".png")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        atlas.save_labels(path, labels)

    def materials_path(self, slug=SLUG):
        return os.path.join(self.dir, "materials", slug + ".json")

    def materials(self, slug=SLUG):
        with open(self.materials_path(slug)) as fh:
            return json.load(fh)

    def run(self, *argv, omit=()):
        """``classify`` with the network replaced. -> ``(rc, output, stub)``.

        The real :func:`classify.classify` still runs -- it builds the request parts
        and parses the response -- with only the client swapped, so this exercises the
        code the pipeline uses rather than a paraphrase of it.
        """
        stub = StubClient(omit=omit)

        def patched(*a, **kw):
            kw.setdefault("client", stub)
            return self._classify(*a, **kw)

        classify.classify = patched
        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                rc = cli.main(["classify", *argv])
        finally:
            classify.classify = self._classify
        return rc, buf.getvalue(), stub

    def run_maps(self, *argv):
        """``maps`` with no ``--generate``, so nothing in it can reach a model."""
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = cli.main(["maps", *argv])
        return rc, buf.getvalue()

    def close(self):
        cli.OUT = self._out
        classify.classify = self._classify
        shutil.rmtree(self.dir, ignore_errors=True)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def seeded(fx):
    """One clean classified entry to invalidate. Fails the test if it did not happen."""
    rc, out, stub = fx.run()
    if rc != 0 or len(stub.calls) != 1:
        check("fixture: a fresh run classifies once", False, "rc %d, %d calls\n%s"
              % (rc, len(stub.calls), tail(out)))
    return out


# ---------------------------------------------------------------------------

def test_a_fresh_run_records_what_it_was_answering():
    with Fixture() as fx:
        rc, out, stub = fx.run()
        check("a fresh texture is classified", rc == 0 and len(stub.calls) == 1,
              "rc %d, %d calls" % (rc, len(stub.calls)))
        data = fx.materials()
        check("the answer is stored under `regions`",
              sorted(data.get("regions", {})) == ["0", "1", "2"],
              str(sorted(data.get("regions", {}))))
        fp = data.get("fingerprint", {})
        check("...beside a fingerprint of every input that determined it",
              sorted(fp) == ["albedo", "labels", "model", "prompt", "schema", "system"],
              str(sorted(fp)))
        # The prompt digest must be of the string that was actually sent, not of one
        # rebuilt afterwards -- that is the whole reason `classify` accepts a rendered
        # prompt rather than building its own.
        check("the prompt digest is of the prompt the model saw",
              fp.get("prompt") == cache.digest(stub.calls[0]["prompt"]),
              fp.get("prompt", ""))
        check("the model is recorded verbatim, not hashed",
              fp.get("model") == classify.DEFAULT_MODEL, str(fp.get("model")))


def test_an_unchanged_rerun_spends_nothing():
    with Fixture() as fx:
        seeded(fx)
        rc, out, stub = fx.run()
        check("a second run makes no API call", not stub.calls, "%d calls" % len(stub.calls))
        check("...and does not fail the run", rc == 0, "rc %d" % rc)
        check("...and says the entry was cached", "cached 1" in out, tail(out))


def _invalidation(name, mutate, expect, *, fixture_kw=None):
    """One input moves under a cached entry; nothing may be bought over it."""
    with Fixture(**(fixture_kw or {})) as fx:
        seeded(fx)
        before = fx.materials()
        mutate(fx)
        rc, out, stub = fx.run()
        check("%s: no API call is made" % name, not stub.calls,
              "%d calls" % len(stub.calls))
        check("%s: the run exits non-zero" % name, rc == 1, "rc %d" % rc)
        check("%s: the report names the input that moved" % name, expect in out,
              tail(out))
        check("%s: the cached answer is left on disk untouched" % name,
              fx.materials() == before)
        return fx


def test_a_changed_albedo_invalidates():
    """The addon's decoder moves on its own; this is the case that proves it lands."""
    _invalidation("changed albedo", lambda fx: fx.write_albedo(albedo_array(seed=7)),
                  "the albedo image")


def test_a_changed_prompt_invalidates():
    """A re-segmentation reaches the model as a different prompt: new parts, new counts."""
    def mutate(fx):
        fx.manifest[TEXTURE] = record(parts=("hull", "turret"), polys=(400, 130))
        fx.write_manifest()

    _invalidation("changed prompt", mutate, "the prompt")


def test_a_changed_label_image_invalidates():
    """The failure the whole module is for: id 2 now denotes different texels.

    Nothing in the manifest need change for this -- a rasterizer edit repaints the
    label image while leaving the patch list identical -- so the prompt digest is not
    what catches it.
    """
    def mutate(fx):
        fx.write_labels(label_array(split=20))

    _invalidation("changed labels", mutate, "the region label image")


def test_a_changed_system_prompt_invalidates():
    """``SYSTEM`` has been edited once already, to stop `metallic` hedging."""
    with Fixture() as fx:
        seeded(fx)
        original = classify.SYSTEM
        classify.SYSTEM = original + "\n- Prefer the darker reading.\n"
        try:
            rc, out, stub = fx.run()
        finally:
            classify.SYSTEM = original
        check("changed system prompt: no API call is made", not stub.calls,
              "%d calls" % len(stub.calls))
        check("changed system prompt: named in the report",
              "the system prompt" in out and rc == 1, tail(out))


def test_refresh_stale_is_what_spends():
    """Reporting is the default; buying a new answer is a flag. Both must work."""
    with Fixture() as fx:
        seeded(fx)
        fx.write_albedo(albedo_array(seed=3))
        rc, out, stub = fx.run("--refresh-stale")
        check("--refresh-stale re-classifies the stale entry",
              len(stub.calls) == 1 and rc == 0, "rc %d, %d calls" % (rc, len(stub.calls)))
        rc, out, stub = fx.run()
        check("...and the entry is valid afterwards, spending nothing",
              rc == 0 and not stub.calls, "rc %d, %d calls" % (rc, len(stub.calls)))


def test_an_entry_with_no_fingerprint_is_unknown_rather_than_either():
    """The files already on disk. Neither valid nor invalid: no evidence exists."""
    with Fixture() as fx:
        seeded(fx)
        # Exactly the pre-fingerprint shape: the bare {id: spec} mapping.
        legacy = fx.materials()["regions"]
        cli._write_json(fx.materials_path(), legacy)

        rc, out, stub = fx.run()
        check("no fingerprint: nothing is bought over it", not stub.calls,
              "%d calls" % len(stub.calls))
        check("no fingerprint: the run does not fail", rc == 0, "rc %d" % rc)
        check("no fingerprint: it is reported as unknown, not as cached",
              "no fingerprint" in out and "unknown 1" in out, tail(out))
        check("no fingerprint: the answer is still readable",
              fx.materials() == legacy)

        rc, out, stub = fx.run("--adopt-cached")
        check("--adopt-cached stamps it without asking the model",
              rc == 0 and not stub.calls and "adopted 1" in out, tail(out))
        check("...and the answer itself is unchanged by the stamp",
              fx.materials()["regions"] == legacy)
        rc, out, stub = fx.run()
        check("...leaving an entry that reads as valid",
              rc == 0 and not stub.calls and "cached 1" in out, tail(out))


def test_the_region_reconciliation_runs_on_cached_entries():
    """It only ever ran on the fresh path, which is where it could not find anything.

    A cached answer is the only kind that can have been written against a *different*
    region list, so the MISSING/EXTRA check was installed on the one path where it
    had nothing to catch.
    """
    with Fixture() as fx:
        rc, out, stub = fx.run(omit=["2"])
        check("the seeded answer really is missing a region",
              "MISSING ['2']" in out, tail(out))

        rc, out, stub = fx.run()
        check("the same gap is reported again from the cache, with no API call",
              "MISSING ['2']" in out and not stub.calls, tail(out))


def test_an_unknown_entry_whose_ids_do_not_line_up_is_stale():
    """The one check that works without a fingerprint, used as evidence.

    An entry with no fingerprint is unknown -- but if its region ids do not reconcile
    against today's patch list, that is no longer unknown, it is the exact failure a
    fingerprint would have caught.
    """
    with Fixture() as fx:
        fx.run(omit=["2"])
        cli._write_json(fx.materials_path(), fx.materials()["regions"])
        rc, out, stub = fx.run()
        check("an unmatched pre-fingerprint entry is stale, not unknown",
              rc == 1 and "region ids" in out, tail(out))
        check("...and is still not re-bought without the flag", not stub.calls,
              "%d calls" % len(stub.calls))

        rc, out, stub = fx.run("--adopt-cached")
        check("--adopt-cached refuses to stamp one that visibly does not line up",
              "adopted" not in out and not stub.calls, tail(out))


def test_maps_says_so_when_it_paints_from_a_stale_answer():
    """`maps` is where a stale classification does its damage: `derive` paints by id.

    It is reported and not refused -- the maps are free to rewrite and blocking a
    `derive` iteration loop would be the wrong trade -- so what is checked is that the
    output still appears *and* that nobody can adopt it without having been told.
    """
    with Fixture() as fx:
        seeded(fx)
        rc, out = fx.run_maps()
        check("maps over a valid cache succeeds and says nothing about staleness",
              rc == 0 and "STALE" not in out, tail(out))

        fx.write_albedo(albedo_array(seed=9))
        rc, out = fx.run_maps()
        check("...and once the albedo has moved, names it and exits non-zero",
              rc == 1 and "STALE materials: the albedo image" in out, tail(out))
        check("...while still writing the map set",
              os.path.exists(os.path.join(fx.dir, "maps", SLUG, "roughness.png")))


def test_generated_artifacts_carry_the_same_check():
    """`maps` caches model output too, and it has the same problem.

    The prompt for a height map is built out of the **stage 1 answer**, so
    re-classifying a texture invalidates the height map generated from the old
    reading. Driven directly rather than through `cmd_maps`, which would need the
    image model as well.
    """
    with Fixture() as fx:
        png = os.path.join(fx.dir, "generated", SLUG + "_height.png")
        os.makedirs(os.path.dirname(png), exist_ok=True)
        fp = {"albedo": "aaaa", "prompt": "bbbb", "model": "m", "tiling": "no"}

        got, note, state = cli._reuse_generated(png, fp, False)
        check("nothing on disk means go and ask", state == "absent" and got is None, note or "")

        images.save(png, np.zeros((SIZE, SIZE), dtype=np.float32), gray=True)
        got, note, state = cli._reuse_generated(png, fp, False)
        check("a PNG with no fingerprint beside it is unknown, and is used",
              state == "unknown" and got is not None, note or "")

        cli._write_json(cli._fingerprint_path(png), fp)
        got, note, state = cli._reuse_generated(png, fp, False)
        check("a matching fingerprint reuses it", state == "cached" and got is not None,
              note or "")

        moved = dict(fp, prompt="cccc")
        got, note, state = cli._reuse_generated(png, moved, False)
        check("a moved prompt makes it stale", state == "stale", note or "")
        check("...and a stale artifact is NOT used, and does not licence a call",
              got is None and "regenerate" in (note or ""), note or "")

        got, note, state = cli._reuse_generated(png, fp, True)
        check("--regenerate ignores a matching one", state == "absent", note or "")


def test_the_harness_can_fail():
    """A harness that cannot report a failure proves nothing."""
    before = len(FAILURES)
    check("deliberate failure (expected FAIL on the line above)", False, "by design")
    ok = len(FAILURES) == before + 1
    FAILURES.pop()
    print("  %-58s %s" % ("harness detects a failure", "ok" if ok else "FAIL"))
    if not ok:
        FAILURES.append("harness cannot fail")


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in tests:
        print("%s:" % fn.__name__)
        fn()
    print()
    if FAILURES:
        print("%d FAILED: %s" % (len(FAILURES), ", ".join(FAILURES)))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
