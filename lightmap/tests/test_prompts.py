"""The prompt assembly, because its bytes are what a cached answer is keyed on.

A prompt is not just text here: the batch driver decides what to re-buy by
comparing today's rendered prompt against the one stored in ``meta.json``. So a
whitespace-only edit to a block that did not change is a real cost -- it invalidated
448 bump and roughness answers, about $11, in the change these tests were written
for. That is the property being pinned.

    uv run python tests/test_prompts.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from gklightmap import prompts  # noqa: E402


def test_a_map_without_context_renders_exactly_body_then_rules():
    """The byte-for-byte form. Change this and every stored answer goes stale."""
    for name in prompts.ORDER:
        if name in prompts.WANTS_CONTEXT:
            continue
        body, _ = prompts.MAPS[name]
        expected = body.strip() + "\n" + prompts.RULES.strip() + "\n"
        assert prompts.prompt_for(name, "ground/anything") == expected, name
        assert prompts.prompt_for(name) == expected, name


def test_the_asset_name_reaches_only_the_maps_that_want_it():
    for name in prompts.ORDER:
        with_name = prompts.prompt_for(name, "ground/gravel")
        if name in prompts.WANTS_CONTEXT:
            assert "ground/gravel" in with_name, name
        else:
            assert "ground/gravel" not in with_name, name


def test_passing_no_name_never_leaves_a_format_placeholder():
    for name in prompts.ORDER:
        text = prompts.prompt_for(name)
        assert "%s" not in text, name


def test_the_name_is_marked_as_evidence_rather_than_specification():
    """The one sentence stopping `s3 level 1k 38 rygb` from becoming a material."""
    text = prompts.prompt_for("metallic", "ground/s3 level 1k 38 rygb")
    assert "believe the image" in text


def test_metallic_still_says_black_is_the_default():
    """The paragraph the A/B credits with the 0.330 -> 0.036 move on matte ground."""
    text = prompts.prompt_for("metallic", "ground/concrete1024")
    assert "Black is the default answer" in text


def test_every_map_carries_the_shared_rules():
    for name in prompts.ORDER:
        text = prompts.prompt_for(name, "ground/gravel")
        assert "pixel-for-pixel aligned" in text, name
        assert "greyscale" in text, name


def test_the_channel_order_matches_the_packed_index():
    for index, name in enumerate(prompts.ORDER):
        assert prompts.MAPS[name][1] == index, name


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
