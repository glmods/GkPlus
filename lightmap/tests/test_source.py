"""Name handling: what may be trimmed, and what may never be.

The split matters because the two names have different owners. :func:`stem` is the
**engine's** key -- ``src/VkLighting.cpp`` probes ``graphics/<stem> lighting.dds``
-- so nothing may normalise it. :func:`slug` is a scratch directory name this tool
owns, and it has to survive a filesystem that refuses some of the characters
Gunlok's asset names actually end in.

    uv run python tests/test_source.py
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from gklightmap import source  # noqa: E402

#: Both ship in `Graphics\Ground`, and both broke the first bulk run.
TRAILING_SPACE = "Ground/outskirts robot ring .RIM"


def test_stem_is_never_trimmed():
    """Trimming here would look for a file the engine does not ask for."""
    assert source.stem(TRAILING_SPACE) == "ground/outskirts robot ring "
    assert source.stem("Ground/city wall conc transitions .RIM") == \
        "ground/city wall conc transitions "


def test_the_installed_name_keeps_the_space_in_the_middle():
    """`<stem> lighting.dds` puts it interior, which Windows stores happily."""
    name = source.stem(TRAILING_SPACE) + " lighting.dds"
    assert name == "ground/outskirts robot ring  lighting.dds"
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, os.path.basename(name))
        with open(path, "wb") as fh:
            fh.write(b"x")
        assert os.path.isfile(path), "the interior double space did not survive"


def test_slug_drops_what_the_filesystem_will_not_store():
    assert source.slug(TRAILING_SPACE) == "ground__outskirts robot ring"
    assert source.slug("Ground/cracks.RIM") == "ground__cracks"


def test_a_slug_directory_can_actually_be_written_into():
    """The real regression: makedirs succeeded and the next open did not."""
    with tempfile.TemporaryDirectory() as tmp:
        directory = os.path.join(tmp, source.slug(TRAILING_SPACE))
        os.makedirs(directory, exist_ok=True)
        target = os.path.join(directory, "albedo.png")
        with open(target, "wb") as fh:
            fh.write(b"x")
        assert os.path.isfile(target)


def test_the_untrimmed_form_still_fails_so_the_test_means_something():
    """Without the fix this is the exact error the batch reported."""
    with tempfile.TemporaryDirectory() as tmp:
        directory = os.path.join(tmp, "ground__outskirts robot ring ")
        os.makedirs(directory, exist_ok=True)
        try:
            with open(os.path.join(directory, "albedo.png"), "wb") as fh:
                fh.write(b"x")
        except FileNotFoundError:
            return                      # Windows: what the run hit
        # A filesystem that stores it is fine too; then there was nothing to fix
        # on that platform and the slug trim is merely harmless.
        assert os.path.isdir(directory)


def test_stem_and_slug_agree_on_an_ordinary_name():
    assert source.stem("Ground/BEIGE ROCK 1024.RIM") == "ground/beige rock 1024"
    assert source.slug("Ground/BEIGE ROCK 1024.RIM") == "ground__beige rock 1024"


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for test in tests:
        try:
            test()
        except Exception as exc:  # noqa: BLE001 - a harness reports rather than dies
            failed += 1
            print("FAIL %s: %s" % (test.__name__, exc))
        else:
            print("ok   %s" % test.__name__)
    print("%d/%d passed" % (len(tests) - failed, len(tests)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
