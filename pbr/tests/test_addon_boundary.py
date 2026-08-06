"""The seam between this project and the Blender addon's decoders.

    uv run python tests/test_addon_boundary.py

``tests/test_pipeline.py`` is synthetic on purpose and imports only ``atlas``,
``derive``, ``images`` and ``metrics`` -- so nothing in it ever reached
``blender/io_scene_rif``, which is the one dependency that moves on its own. The
addon is a separate world with its own tests and its own reasons to change, and
``gkpbr`` imports four of its modules as loose files off ``sys.path``, where a
rename is an ``AttributeError`` at run time and nothing earlier.

It has already happened once. ``rim.Texture.fourcc`` became ``format`` the day
after this project landed, because a palettized ``.RIM`` carries no fourcc at all,
and ``inventory`` raised on its first texture from then until this test was
written -- with the whole suite still green.

So this one is deliberately **not** synthetic: it resolves a real ``.RIM`` out of
the install, decodes it, and builds a manifest record through the same function
``inventory`` calls. It skips when the game is not installed, and it must not be
made to pass by anything cheaper -- a fixture standing in for the addon would
reproduce exactly the blind spot it exists to remove.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from gkpbr import assets, classify, cli, derive  # noqa: E402

FAILURES = []

#: A handful of ``.rif``s rather than the install's 563, because ``collect`` decodes
#: every file it walks and this is a smoke test, not an inventory run. Five files
#: naming five textures between them is enough to cross the boundary in both
#: directions -- the geometry decoders and the texture one.
SAMPLE_DIR = ("RIF", "User Interface")

#: Every key a downstream stage indexes out of a manifest record. `classify`
#: reads the first group (`build_prompt`), `cli.cmd_maps` and `cli._patch_from_dict`
#: the second. Listed here rather than inferred, so adding a consumer that needs a
#: new field is a decision and not a KeyError on someone's re-run.
PROMPT_KEYS = ("name", "width", "height", "format", "alpha", "rif_count", "polys",
               "lum_mean", "sat_mean")
MAPS_KEYS = ("tiling", "patches")
PATCH_KEYS = ("index", "label", "box", "area_texels", "polys", "parts", "rif_count")


def check(name, ok, detail=""):
    print("  %-58s %s%s" % (name, "ok" if ok else "FAIL", "  " + detail if detail else ""))
    if not ok:
        FAILURES.append(name)


def skip(why):
    print("  SKIPPED: %s" % why)


# ---------------------------------------------------------------------------

def test_the_decoders_are_the_addons():
    """The four modules must come from ``blender/io_scene_rif``, not from anywhere else.

    They are imported as bare names off a ``sys.path`` entry, so a same-named module
    elsewhere on the path would satisfy the import and silently decode nothing. This
    is the cheap half of the boundary check and needs no install.
    """
    addon = os.path.normcase(os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
        "blender", "io_scene_rif"))
    for mod in (assets.rim, assets.rif, assets.bmpnames, assets.shp):
        where = os.path.normcase(os.path.dirname(os.path.abspath(mod.__file__)))
        check("%s comes from the addon" % mod.__name__, where == addon, where)


def test_a_real_texture_becomes_a_manifest_record():
    """Decode a shipped ``.RIM`` and build the record every later stage reads.

    This is the whole point of the file: `cli.texture_record` is the only code that
    touches a :class:`rim.Texture`'s fields, and `classify.build_prompt` is the only
    code that reads them back out of the manifest.
    """
    try:
        game = assets.find_install()
    except SystemExit:
        skip("no Gunlok install; set GUNLOK_DIR to run this")
        return
    sample = os.path.join(game, *SAMPLE_DIR)
    if not os.path.isdir(sample):
        skip("install at %s has no %s" % (game, os.path.join(*SAMPLE_DIR)))
        return
    print("  install: %s" % game)

    # ``rif_root`` rather than passing ``sample`` as the install: ``collect`` walks
    # ``<game>\RIF`` and nothing else, so that a mod under ``gkplus\mods`` cannot
    # contribute polygons to the manifest.
    refs = assets.collect(game, rif_root=sample)
    check("the sample .rifs name some textures", bool(refs), "%d names" % len(refs))
    if not refs:
        return

    index = assets.rim.TextureIndex(os.path.join(game, "Graphics"))
    name = tex = None
    for candidate in sorted(refs):
        got = assets.load_texture(game, candidate, index)
        if got is not None:
            name, tex = candidate, got
            break
    check("at least one of them resolves to a decodable .RIM on disk", tex is not None,
          str(name))
    if tex is None:
        return

    check("the decoded image is the size it declares",
          tex.width > 0 and tex.height > 0
          and len(tex.rgba) == tex.width * tex.height * 4,
          "%s %dx%d %s, %d bytes" % (name, tex.width, tex.height, tex.format,
                                     len(tex.rgba)))

    rec, albedo, labels = cli.texture_record(name, tex, refs[name])
    check("the record carries every field the prompt reads",
          all(k in rec for k in PROMPT_KEYS),
          "missing %s" % [k for k in PROMPT_KEYS if k not in rec])
    check("...and every field the maps stage reads",
          all(k in rec for k in MAPS_KEYS),
          "missing %s" % [k for k in MAPS_KEYS if k not in rec])
    check("the albedo matches the record's declared size",
          albedo.shape[:2] == (rec["height"], rec["width"]), str(albedo.shape))

    # The manifest is JSON on disk, so a numpy scalar that survives in memory is
    # still a broken run for everyone downstream.
    check("the record survives the JSON round trip the manifest takes",
          json.loads(json.dumps(rec)) == rec)

    for patch in rec["patches"]:
        check("each patch carries the fields _patch_from_dict rehydrates",
              all(k in patch for k in PATCH_KEYS),
              "missing %s" % [k for k in PATCH_KEYS if k not in patch])
        break  # they are built by one code path; one is the check, not a survey

    # And the read side, which is what actually broke: `build_prompt` indexes the
    # record directly, so a renamed field raises here rather than downstream.
    patches = [cli._patch_from_dict(p) for p in rec["patches"]]
    prompt = classify.build_prompt(rec, patches)
    check("build_prompt names the texture and its size",
          rec["name"] in prompt and ("%dx%d" % (rec["width"], rec["height"])) in prompt)
    check("...and says the storage format in words, not as a raw token",
          classify.FORMAT_PHRASES.get(rec["format"], rec["format"]) in prompt,
          "format %r" % rec["format"])
    check("...with one line per region plus the leftovers",
          all(('id "%d"' % p.label) in prompt for p in patches),
          "%d regions" % len(patches))

    # Labels and albedo have to agree for `derive` to paint into them at all, which
    # is a real pairing only a decoded texture can produce.
    rough = derive.roughness(albedo, labels, {})
    check("derive paints the real albedo at its own resolution",
          rough.shape == albedo.shape[:2], str(rough.shape))
    if labels is not None:
        check("the label image is the size of the sheet",
              labels.shape == albedo.shape[:2], str(labels.shape))


def test_an_old_manifest_still_classifies():
    """A ``manifest.json`` written before the addon's rename spells it ``fourcc``.

    Nothing else about such a record is stale, so it is read rather than rejected --
    regenerating one means walking all 563 ``.rif``s.
    """
    old = {"name": "units/baddies3.rim", "width": 1024, "height": 1024,
           "fourcc": "DXT1", "alpha": False}
    prompt = classify.build_prompt(old, [])
    check("an old record still names its format",
          classify.FORMAT_PHRASES["DXT1"] in prompt, prompt.splitlines()[1])

    # Neither spelling present is a record from nowhere; it must not raise, because
    # the prompt is the least important thing in the run.
    prompt = classify.build_prompt({"name": "x", "width": 8, "height": 8}, [])
    check("a record with neither spelling still builds a prompt", "8x8" in prompt)


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
