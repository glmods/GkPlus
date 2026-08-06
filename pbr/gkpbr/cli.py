"""Stages, each writing to disk so any one of them can be re-run alone.

    uv run -m gkpbr.cli inventory              # what is used, and its regions
    uv run -m gkpbr.cli probe ground/cracks    # does the model hold registration?
    uv run -m gkpbr.cli classify               # stage 1, cached JSON per texture
    uv run -m gkpbr.cli maps                   # stages 2-4, the PNGs

``probe`` is the one to run first. It is the measurement the whole design rests on
and it costs a few cents: if the image model cannot keep a height map registered to
its input even at low frequencies, the generative half of the pipeline is dead and
the derived half is the product.
"""

import argparse
import collections
import json
import os
import sys


from . import (assets, atlas, cache, classify, derive, generate, images, levels,
               metrics, preview, renderstate)

OUT = os.environ.get("GKPBR_OUT") or os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "out")

MAP_NAMES = ("color", "roughness", "metallic", "normal", "emissive", "height")


def _paths(*parts):
    path = os.path.join(OUT, *parts)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    return path


def _slug(name):
    return name.replace("/", "__").rsplit(".", 1)[0]


def _read_json(path):
    with open(path) as fh:
        return json.load(fh)


def _write_json(path, data):
    with open(path, "w") as fh:
        json.dump(data, fh, indent=1)


def _read_bytes(path):
    with open(path, "rb") as fh:
        return fh.read()


def _select(args, manifest):
    """Which textures a stage runs on: explicit names, a level's closure, or all.

    A level's set is its asset *closure* -- its own terrain rif plus every rif named
    by the `.gsh` headers its `.gls` includes -- not a directory listing. See
    gkpbr/levels.py.
    """
    if getattr(args, "level", None):
        game = assets.find_install(args.game)
        info = levels.level_textures(game, args.level)
        names = sorted(t for t in info["textures"] if t in manifest)
        skipped = sorted(t for t in info["textures"] if t not in manifest)
        print("level %s: %d rifs -> %d textures (%d in the manifest)"
              % (args.level, len(info["per_rif"]), len(info["textures"]), len(names)))
        if info["missing_rifs"]:
            print("  rifs named but not on disk: %s" % info["missing_rifs"][:5])
        if skipped:
            print("  named but not in the manifest: %s" % skipped[:5])
    elif args.textures:
        # Named explicitly: the caller's order is the order, and neither the filter
        # nor the ranking below applies. Asking for one texture by name and being
        # told it was skipped for not having been observed would be absurd.
        return [n.lower() for n in args.textures]
    else:
        names = sorted(manifest)

    # Ordering, not filtering, by default. The profile is one run over a finite set
    # of levels and camera positions, so "never observed" is not "never drawn" -- but
    # a sheet the game demonstrably paints thousands of times a frame is worth
    # spending on first, and an interrupted run then leaves the useful half done.
    # `--seen-only` is the operator asserting they want the other half skipped.
    profile = renderstate.load()
    if profile is None:
        return names
    if getattr(args, "seen_only", False):
        before = len(names)
        names = [n for n in names if renderstate.entry(profile, n)]
        print("--seen-only: %d of %d were drawn in the profiled run" % (len(names), before))
    return sorted(names,
                  key=lambda n: (-(renderstate.entry(profile, n) or {}).get("draws", 0), n))


# ---------------------------------------------------------------------------
# inventory
# ---------------------------------------------------------------------------

def texture_record(name, tex, refs):
    """One manifest entry, with the albedo and label image it describes.

    Split out of :func:`cmd_inventory` so ``tests/test_addon_boundary.py`` can build
    a record the same way rather than a plausible imitation of one. This is the only
    code path that reads a :class:`rim.Texture`, and the addon renaming a field on it
    breaks the manifest for every stage downstream while nothing in the synthetic
    suite notices.
    """
    albedo = images.from_texture(tex)
    mask, labels, patches, stats = atlas.segment(refs, tex.width, tex.height)

    # The UVs decide this, not the pixels: a sheet the game samples with wrapping
    # UVs is a tiling surface and gets one material, and only a sheet sampled
    # inside sub-rectangles is segmented. See atlas.tiling_fraction.
    frac = atlas.tiling_fraction(stats)
    tiling = frac >= 0.5 or not patches
    if tiling:
        patches = []
        labels = None

    sx, sy = metrics.seam_energy(albedo)
    rec = {
        "name": name,
        "width": tex.width,
        "height": tex.height,
        # Named for the addon's field, not for a fourcc: a palettized ``.RIM`` has
        # no four-character code at all and reads ``BODY`` here. `classify` phrases
        # it for the prompt rather than printing the token.
        "format": tex.format,
        "alpha": tex.has_alpha,
        "rif_count": len({r.rif for r in refs}),
        "polys": len(refs),
        "tiling": bool(tiling),
        "tiling_fraction": round(frac, 3),
        "uv_samples": stats,
        "seam": [round(sx, 2), round(sy, 2)],
        "coverage": round(atlas.coverage(mask), 3),
        "lum_mean": round(float(metrics.luminance(albedo).mean()) * 255, 1),
        "sat_mean": round(float(derive.saturation(albedo).mean()), 3),
        "patches": [p.as_dict() for p in patches],
    }
    return rec, albedo, labels


def cmd_inventory(args):
    game = assets.find_install(args.game)
    print("install: %s" % game)
    wanted = {n.lower() for n in args.textures} or None
    refs = assets.collect(game, wanted)
    print("textures referenced by geometry: %d" % len(refs))

    index = assets.rim.TextureIndex(os.path.join(game, assets.TEXTURE_DIR))
    manifest = {}
    skipped = collections.Counter()
    covered = set()

    for name in sorted(refs):
        tex = assets.load_texture(game, name, index)
        if tex is None:
            skipped["no image on disk"] += 1
            continue
        path = index.resolve(name)
        covered.add(os.path.relpath(path, os.path.join(game, assets.TEXTURE_DIR))
                    .replace(os.sep, "/").lower())
        rec, albedo, labels = texture_record(name, tex, refs[name])
        manifest[name] = rec
        images.save(_paths("albedo", _slug(name) + ".png"), albedo)
        if labels is not None:
            atlas.save_labels(_paths("labels", _slug(name) + ".png"), labels)
        print("  %-40s %4dx%-4d %s tf %.2f %3d patches cov %.2f" % (
            name, tex.width, tex.height, "tiling" if rec["tiling"] else "atlas ",
            rec["tiling_fraction"], len(rec["patches"]), rec["coverage"]))

    _write_json(_paths("manifest.json"), manifest)
    n_atlas = sum(1 for r in manifest.values() if r["patches"])
    print()
    print("wrote %s" % _paths("manifest.json"))
    print("%d textures: %d tiling, %d atlases (%d regions total)" % (
        len(manifest), len(manifest) - n_atlas, n_atlas,
        sum(len(r["patches"]) for r in manifest.values())))
    for why, n in skipped.items():
        print("skipped %d: %s" % (n, why))
    if wanted is None:
        _account_for_the_rest(game, covered)


def _account_for_the_rest(game, covered):
    """Say out loud what is on disk and *not* in the manifest, and why.

    Silence here is what let 17 table-named textures go missing before (see the
    README). The mirror-image question -- 148 of the 513 shipped ``.RIM`` are named
    by no ``BMPNAMES`` table at all -- deserves the same treatment, because "gkpbr
    does not walk to it" and "the game never draws it on geometry" look identical
    from a manifest that simply omits both.

    They are not the same, and the split is measured: ``BMPNAMES`` is the only
    name/index binding a polygon has, so a texture no table names is unreachable
    from geometry by construction, and whatever draws it draws it as front-end art
    from a name spelled out in ``gl.exe`` or in a script. A PBR map set for a load
    screen or a level map image is meaningless, so these are reported, not adopted.
    """
    on_disk = assets.textures_on_disk(game)
    rest = [p for p in on_disk if p not in covered]
    routes = assets.name_routes(game, rest)
    by_route = collections.Counter(routes.values())

    print()
    print("%d .RIM on disk; %d named by a BMPNAMES table -> the manifest, %d by none"
          % (len(on_disk), len(covered), len(rest)))
    for route, n in by_route.most_common():
        sample = [p for p in rest if routes[p] == route][:3]
        print("  %3d  %-38s e.g. %s" % (n, route, ", ".join(sample)))
    print("  none of these can be reached from a polygon: BMPNAMES is the only "
          "name/index\n  binding in the format, so they are front-end art, or a "
          "surface the engine\n  names itself, or unused authoring leftovers.")
    _account_for_the_profile(covered, routes)


def _account_for_the_profile(covered, routes):
    """Join the checked-in render profile against what this walk decided to carry.

    Same principle as the paragraph above and the same failure it guards: a manifest
    that simply omits a texture looks identical whether the game never draws it or
    this walk cannot reach it. The profile settles that from the other direction --
    it is a record of what the engine actually bound -- so anything it saw and this
    walk dropped gets named out loud, and anything the walk carries that no observed
    frame ever bound gets counted.

    Both directions are reported and neither is acted on. A sheet drawn but not
    carried may be HUD art (``units/plates 2 1024.rim`` is, and is the most-drawn
    sheet in the game) or a surface the engine names for itself (``bitmaps/lava``);
    a sheet carried but never seen may simply not be in a level this run reached.
    """
    profile = renderstate.load()
    if profile is None:
        return
    seen = set(renderstate.textures(profile))
    carried = {renderstate.normalise(p) for p in covered}
    unseen = sorted(carried - seen)
    outside = sorted(seen - carried)
    print()
    print("render profile (%s): %d of the %d carried sheets were observed drawn, "
          "%d were not"
          % (profile.get("run", {}).get("source", "unrecorded"),
             len(carried & seen), len(carried), len(unseen)))
    print("  %d sheets were drawn that this walk does not carry:" % len(outside))
    for name in outside:
        print("    %9d draws  %-42s %s"
              % (renderstate.textures(profile)[name]["draws"], name,
                 routes.get(name, "(not a .RIM under Graphics)")))
    print("  neither direction is acted on here: absence in one run is not absence, "
          "and a\n  drawn sheet may be HUD art or a surface the engine names itself. "
          "See `observed`.")


# ---------------------------------------------------------------------------
# observed -- what the running game does with each sheet
# ---------------------------------------------------------------------------

def cmd_observed(args):
    """Print the render-state profile, or rebuild it from a harvest dump.

    The join against the manifest is the point of printing it at all, and it is
    printed in full for the same reason ``inventory`` prints the 148: a texture the
    game demonstrably draws and this pipeline has never heard of is exactly the class
    of thing a silent manifest hides.
    """
    if args.rebuild_from:
        harvest = [_read_json(p) for p in args.rebuild_from]
        run = {"source": ", ".join(os.path.basename(p) for p in args.rebuild_from)}
        if args.note:
            run["note"] = args.note
        if args.renderer:
            run["renderer"] = args.renderer
        profile = renderstate.from_harvest(harvest, run)
        with open(renderstate.PROFILE_PATH, "w") as fh:
            json.dump(profile, fh, indent=1, sort_keys=True)
        print("wrote %s" % renderstate.PROFILE_PATH)
    else:
        profile = renderstate.load()
    if profile is None:
        print("no profile; run `observed --from <harvest.json>`, and see "
              "utils/rendertest/harvest-draws.ps1 for how to produce one")
        return 1

    manifest = None
    if os.path.exists(_paths("manifest.json")):
        manifest = _read_json(_paths("manifest.json"))
    stats = renderstate.summarise(profile, manifest)
    run = profile.get("run", {})
    print("run: %s" % run.get("note", run.get("source", "unrecorded")))
    print("  %s frames sampled, %s missed while sampling, %s draws, %d levels"
          % (run.get("frames_sampled"), run.get("frames_missed_while_sampling"),
             run.get("draws_observed"), len(run.get("levels", []))))
    print("  levels: %s" % ", ".join(run.get("levels", [])))
    print()
    print("%d sheets observed: %d never blended, %d blended on every draw"
          % (stats["observed"], stats["never_blended"], stats["always_blended"]))
    print("%d ever drawn additively, %d ONLY additively; %d ever alpha-tested, %d always"
          % (stats["any_additive"], stats["additive_only"],
             stats["any_alpha_test"], stats["always_alpha_test"]))
    if manifest is not None:
        print("manifest join: %d of %d seen, %d not seen in this run"
              % (stats["in_manifest_seen"], len(manifest), stats["in_manifest_unseen"]))
        outside = stats["drawn_outside_manifest"]
        print("%d sheets were drawn that the manifest does not carry:" % len(outside))
        for name in outside:
            e = renderstate.textures(profile)[name]
            print("  %9d draws  %-42s in %s"
                  % (e["draws"], name, ", ".join(e["levels"][:4])))
    print()
    print("%-46s %9s %6s %6s %6s" % ("sheet", "draws", "blend", "add", "atest"))
    rows = sorted(renderstate.textures(profile).items(), key=lambda kv: -kv[1]["draws"])
    for name, e in rows[:args.top]:
        print("%-46s %9d %5.0f%% %5.0f%% %5.0f%%"
              % (name[:46], e["draws"], 100 * e["blend"] / e["draws"],
                 100 * e["additive"] / e["draws"], 100 * e["alpha_test"] / e["draws"]))
    return 0


# ---------------------------------------------------------------------------
# probe -- the measurement the design rests on
# ---------------------------------------------------------------------------

def cmd_probe(args):
    """Ask for a height map on a handful of textures and report the gates.

    Prints the two numbers that decide whether the generative half is viable at
    all, and writes both images so the failure mode is visible and not just scored.
    """
    manifest = _read_json(_paths("manifest.json"))
    names = args.textures or sorted(manifest)[:4]
    print("%-34s %6s %10s %7s %7s  %s" % (
        "texture", "align", "shift", "seamX", "seamY", "verdict"))

    for name in names:
        rec = manifest.get(name.lower())
        if rec is None:
            print("%-34s  not in manifest" % name[:34])
            continue
        albedo = images.load(_paths("albedo", _slug(rec["name"]) + ".png"))
        # `raw` comes back whether or not the gate passed, so a rejection can be
        # looked at rather than only scored -- a map rejected for the wrong reason
        # and a map that is genuinely wrong are indistinguishable from a number.
        got, note, raw = generate.height(
            albedo, "a game texture of unknown material",
            "whatever geometric relief the surface has",
            require_tiling=rec["tiling"], escalate=False, want_raw=True)

        if raw is not None:
            images.save(_paths("probe", _slug(rec["name"])
                               + ("_height.png" if got is not None else "_REJECTED.png")),
                        raw, gray=True)
        if got is None:
            print("%-34s %6s %10s %7s %7s  REJECTED %s" % (
                rec["name"][:34], "-", "-", "-", "-", note))
            continue
        r, shift, _ = metrics.align(metrics.luminance(albedo), got)
        sx, sy = metrics.seam_energy(got)
        print("%-34s %6.2f %10s %7.1f %7.1f  %s" % (
            rec["name"][:34], r, "(%d,%d)" % shift, sx, sy, note))
        images.save(_paths("probe", _slug(rec["name"]) + "_normal.png"),
                    derive.normal_from_height(got, green_down=args.green_down))

    print()
    print("wrote %s" % _paths("probe"))
    print("align >= %.2f and a shift within a few pixels means the generative half "
          "works.\nBelow that, the derived maps are the product." % metrics.MIN_ALIGN)


# ---------------------------------------------------------------------------
# classify
# ---------------------------------------------------------------------------

def _read_materials(path):
    """``(regions, fingerprint)``; ``fingerprint`` is ``None`` for a pre-hash file.

    The file gained a wrapper when it gained a fingerprint. Files written before that
    are the bare ``{id: spec}`` mapping and are read, not rejected -- see
    :func:`cmd_classify` for what "unknown" then does, and note that a hand-edited
    answer is still just as readable inside ``regions``.
    """
    data = _read_json(path)
    if isinstance(data, dict) and "fingerprint" in data:
        return data.get("regions") or {}, data["fingerprint"]
    return data, None


def _reconcile(rec, regions):
    """``(missing, extra)`` region ids: answered against the manifest's patch list."""
    real = {str(p["label"]) for p in rec["patches"]}
    return (sorted(real - set(regions), key=lambda k: int(k)),
            sorted(set(regions) - real - {"0"}))


def _classify_fingerprint(rec, slug, model, profile=None):
    """Today's fingerprint for one texture, plus the pieces a call would need.

    Returns ``(fingerprint, albedo_png, patches, colours, prompt, label_path)``, or
    ``None`` when the albedo is missing -- which means the manifest and the output
    directory disagree and nothing downstream can run either.

    Everything here is file reads and one prompt render, deliberately: **validating a
    cache entry must not cost what producing it did**. The region overlay is the
    expensive part (a full-resolution paint plus a PNG encode) and it is only built
    when the model is actually going to be called; the label PNG's stored bytes stand
    in for it, which :func:`classify.fingerprint` explains.
    """
    albedo_path = _paths("albedo", slug + ".png")
    if not os.path.exists(albedo_path):
        return None
    albedo_png = _read_bytes(albedo_path)
    patches = [_patch_from_dict(p) for p in rec["patches"]]
    lab_path = _paths("labels", slug + ".png")
    has_labels = bool(patches) and os.path.exists(lab_path)
    colours = classify.colours_for(patches) if has_labels else None
    # The observation goes into the prompt, so the prompt digest already covers it:
    # re-harvesting the game moves the rendered text and the entry goes stale by the
    # same route a re-segmentation does. Nothing extra is added to the fingerprint.
    prompt = classify.build_prompt(rec, patches, colours,
                                   renderstate.entry(profile, rec["name"]))
    fp = classify.fingerprint(albedo_png, _read_bytes(lab_path) if has_labels else None,
                              prompt, model)
    return fp, albedo_png, patches, colours, prompt, (lab_path if has_labels else None)


def cmd_classify(args):
    manifest = _read_json(_paths("manifest.json"))
    names = _select(args, manifest)
    profile = renderstate.load()
    if profile is None:
        print("no render_profile.json: the prompts carry no measured render states")
    done = valid = failed = adopted = 0
    stale = []
    unknown = []
    gaps = collections.Counter()

    for name in names:
        rec = manifest.get(name)
        if rec is None:
            continue
        slug = _slug(name)
        out = _paths("materials", slug + ".json")
        built = _classify_fingerprint(rec, slug, args.model, profile)
        if built is None:
            print("  %-40s no albedo on disk; re-run inventory" % name)
            continue
        fp, albedo_png, patches, colours, prompt, lab_path = built

        state, diff, regions = "fresh", [], None
        if os.path.exists(out) and not args.force:
            regions, stored = _read_materials(out)
            diff = cache.changed(stored, fp)
            state = "unknown" if diff is None else ("stale" if diff else "valid")

        # What a mismatch does, which is a choice and not an obvious one. Silently
        # re-calling is $0.001 a texture and ~$0.40 for the set -- cheap, but it
        # spends the whole stage's budget on a one-character edit to SYSTEM without
        # anyone deciding to, and it does it *quietly*, which is the property this
        # whole module exists to remove. Refusing to run at all costs someone's
        # attention on a run they may not care about. So: the entry is named, the
        # input that moved is named, the file on disk is left exactly as it was, and
        # `--refresh-stale` is the flag that spends. The run exits non-zero because
        # `maps` downstream will otherwise paint from an answer to a different
        # question and say nothing about it.
        spend = state == "fresh" or (state == "stale" and args.refresh_stale)
        if spend:
            if state == "stale":
                print("  %-40s stale (%s changed), re-asking"
                      % (name, cache.explain(diff)))
            region_png = None
            if lab_path is not None:
                overlay, _ = classify.region_map(
                    atlas.load_labels(lab_path), patches, rec["width"], rec["height"])
                region_png = images.to_png_bytes(overlay)
                images.save(_paths("regions", slug + ".png"), overlay)
            try:
                regions = classify.classify(rec, patches, albedo_png, region_png,
                                            colours, model=args.model, prompt=prompt)
            except Exception as exc:  # noqa: BLE001
                print("  %-40s FAILED %s" % (name, exc))
                failed += 1
                continue
            _write_json(out, {"fingerprint": fp, "regions": regions})
            done += 1
        elif state == "valid":
            valid += 1
        elif state == "stale":
            stale.append((name, diff))
            print("  %-40s STALE, kept: %s changed" % (name, cache.explain(diff)))

        # Runs on every path now, and that is the point of it being here rather than
        # on the fresh one: the check that catches a re-pointed region id was the one
        # check that never ran on the entries that could have one. Still reported and
        # not gated -- a label with no answer falls back to a safe neutral in
        # `derive`, and an answer for a label that does not exist is never looked up.
        missing, extra = _reconcile(rec, regions)
        if missing or extra:
            gaps["missing"] += len(missing)
            gaps["extra"] += len(extra)
            print("  %-40s %2d regions  MISSING %s EXTRA %s"
                  % (name, len(regions), missing[:6], extra[:6]))

        if state == "unknown":
            # Unknown is not the same as wrong: these were produced by this pipeline,
            # just before it recorded what from. So they neither spend nor fail the
            # run. But an unknown entry whose ids do not reconcile against today's
            # patch list is no longer unknown -- that is the exact failure the
            # fingerprint exists to catch, arriving through the only check that works
            # without one.
            if missing or extra:
                stale.append((name, ["region ids"]))
            elif args.adopt_cached:
                # An assertion by the operator, not a check: it stamps today's inputs
                # onto an answer nobody can prove came from them. Offered because the
                # alternative is reporting the same entries on every run forever, and
                # refused above for an entry that visibly does not line up.
                _write_json(out, {"fingerprint": fp, "regions": regions})
                adopted += 1
            else:
                unknown.append(name)
                print("  %-40s cached with no fingerprint (%d regions)"
                      % (name, len(regions)))
        elif spend:
            low = [k for k, v in regions.items() if float(v.get("confidence", 1)) < 0.4]
            summary = ", ".join("%s=%s" % (k, v.get("material")) for k, v in
                                list(regions.items())[:3])
            print("  %-40s %2d regions%s  %s"
                  % (name, len(regions), " (%d low-conf)" % len(low) if low else "",
                     summary[:52]))

    print("\nclassified %d, cached %d, stale %d, unknown %d, failed %d -> %s"
          % (done, valid, len(stale), len(unknown), failed, _paths("materials")))
    if adopted:
        print("adopted %d pre-fingerprint entries as answers to today's inputs"
              % adopted)
    if gaps["missing"] or gaps["extra"]:
        print("region-id mismatches: %d unanswered, %d not in the manifest"
              % (gaps["missing"], gaps["extra"]))
    if unknown:
        print("%d cached before fingerprints existed and left alone: %s%s"
              % (len(unknown), ", ".join(unknown[:4]),
                 " ..." if len(unknown) > 4 else ""))
        print("  their region ids reconcile; --adopt-cached stamps today's inputs on "
              "them without re-asking.")
    if stale:
        moved = collections.Counter(k for _, d in stale for k in d)
        print("%d STALE: cached against inputs that have since moved -- %s"
              % (len(stale), ", ".join("%s (%d)" % (cache.MEANS.get(k, k), n)
                                       for k, n in moved.most_common())))
        print("  left on disk and NOT re-asked. --refresh-stale re-classifies exactly "
              "these,\n  at about $0.001 each; --force re-classifies everything.")
    return 1 if stale else 0


class _Patch:
    """Rehydrated from the manifest, so ``classify``/``maps`` need no second walk."""

    __slots__ = ("index", "label", "box", "area", "polys", "parts", "rifs")


def _patch_from_dict(d):
    p = _Patch()
    p.index = d["index"]
    p.label = d["label"]
    p.box = tuple(d["box"])
    p.area = d["area_texels"]
    p.polys = d["polys"]
    p.parts = d["parts"]
    p.rifs = d["rif_count"]
    return p


# ---------------------------------------------------------------------------
# maps
# ---------------------------------------------------------------------------

def _fingerprint_path(png_path):
    """The sidecar beside a generated artifact. A PNG has nowhere to carry one."""
    return png_path.rsplit(".", 1)[0] + ".json"


def _reuse_generated(png_path, fp, regenerate):
    """``(array, note, state)`` for a cached model artifact.

    ``state`` is ``"absent"`` when the caller should go and ask, and anything else
    when it must not: a **stale** artifact returns ``None`` and does not licence a
    call, because the money-spending decision belongs to ``--regenerate`` and not to
    the discovery that a file is out of date. The caller then falls back to the
    derived map, which is exactly what it does for a map that failed its gate -- a
    stale height map and a rejected one are the same thing, output that must not be
    painted, and the pipeline already has one honest answer for that case.
    """
    if regenerate or not os.path.exists(png_path):
        return None, None, "absent"
    fp_path = _fingerprint_path(png_path)
    stored = _read_json(fp_path) if os.path.exists(fp_path) else None
    diff = cache.changed(stored, fp)
    if diff:
        return (None, "stale (%s changed; --regenerate to re-ask)" % cache.explain(diff),
                "stale")
    if diff is None:
        return images.load(png_path), "cached, no fingerprint", "unknown"
    return images.load(png_path), "cached", "cached"


def cmd_maps(args):
    manifest = _read_json(_paths("manifest.json"))
    names = _select(args, manifest)
    profile = renderstate.load()
    report = {}
    stale = []

    for name in names:
        rec = manifest.get(name)
        if rec is None:
            continue
        slug = _slug(name)
        albedo_path = _paths("albedo", slug + ".png")
        albedo_png = _read_bytes(albedo_path)
        albedo = images.load(albedo_path)
        h, w = albedo.shape[:2]

        mat_path = _paths("materials", slug + ".json")
        materials, mat_fp = ({}, None)
        if os.path.exists(mat_path):
            materials, mat_fp = _read_materials(mat_path)
        if mat_fp:
            # Checked here as well as in `classify`, because this is the stage where a
            # stale answer does damage: `derive` paints per region id, so an answer
            # cached against a different segmentation lands roughness and metalness on
            # the wrong texels. Reported and not fatal -- the maps are free to rewrite
            # and blocking a `derive` iteration loop would be the wrong trade -- but
            # the run exits non-zero so nobody adopts the output by accident.
            #
            # The stage-1 model is carried across from the cached entry rather than
            # compared: `maps --model` names the *image* model, so this stage is in no
            # position to have an opinion on which text model answered stage 1.
            # `classify` is where that one is checked.
            built = _classify_fingerprint(rec, slug, mat_fp.get("model", ""), profile)
            diff = cache.changed(mat_fp, built[0]) if built else None
            if diff:
                stale.append((name, diff))
                print("  %-40s STALE materials: %s changed"
                      % (name, cache.explain(diff)))

        labels = None
        lab_path = _paths("labels", slug + ".png")
        if rec["patches"] and os.path.exists(lab_path):
            labels = atlas.load_labels(lab_path)

        note = "derived"
        colour = albedo
        heightfield = derive.height_from_albedo(albedo, labels, materials)

        # Model output is cached as its own artifact, separately from the maps
        # derived from it. Without this, every edit to `derive` costs a full API run
        # to see -- which would make the README's claim that stage 1's cache lets you
        # "rewrite derive and regenerate every map with no further model calls" false
        # for the one stage that actually spends money.
        #
        # And it is cached against a fingerprint, for the same reason stage 1 is: the
        # prompt these were generated from is built out of the **stage 1 answer**
        # (`_summarise`), so re-classifying a texture, or hand-editing one material,
        # silently invalidates the height map derived from the old reading. The
        # fingerprint is a sidecar `.json`, because a PNG has nowhere to carry one.
        gen_height = _paths("generated", slug + "_height.png")
        gen_colour = _paths("generated", slug + "_delit.png")

        if args.generate:
            summary, relief = _summarise(materials)
            hfp = generate.fingerprint(albedo_png,
                                       generate.HEIGHT_PROMPT % (summary, relief),
                                       args.model, rec["tiling"])
            got, gnote, gstate = _reuse_generated(gen_height, hfp, args.regenerate)
            if got is not None and got.ndim == 3:
                got = metrics.luminance(got)
            if gstate == "absent":
                got, gnote = generate.height(albedo, summary, relief,
                                             require_tiling=rec["tiling"],
                                             model=args.model, escalate=args.escalate)
                if got is not None:
                    images.save(gen_height, got, gray=True)
                    _write_json(_fingerprint_path(gen_height), hfp)
            elif gstate == "stale":
                stale.append((name, ["generated height"]))

            if got is not None:
                heightfield = derive.fuse_height(got, albedo, labels, materials)
                note = "generated: " + gnote
            else:
                note = "fell back: " + gnote

            if args.delight and _wants_delight(materials):
                dfp = generate.fingerprint(albedo_png, generate.DELIGHT_PROMPT % summary,
                                           args.model)
                delit, dnote, dstate = _reuse_generated(gen_colour, dfp, args.regenerate)
                if dstate == "absent":
                    delit, dnote = generate.delit_albedo(albedo, summary,
                                                         model=args.model)
                    if delit is not None:
                        images.save(gen_colour, delit)
                        _write_json(_fingerprint_path(gen_colour), dfp)
                elif dstate == "stale":
                    stale.append((name, ["generated colour"]))
                if delit is not None:
                    # The request is whole-sheet because the model needs the whole
                    # picture to read it, but the *result* is kept only where a
                    # region said it was lit. See derive.delight_mask: baked
                    # lighting here is a minority of regions, and a de-lighter
                    # applied to a flat-lit photographic plate takes off material
                    # contrast rather than light.
                    colour = derive.apply_where(
                        albedo, delit, derive.delight_mask(labels, materials))
                else:
                    colour = derive.delight(albedo, labels, materials)
                note += " | delight %s" % dnote
        elif args.delight and _wants_delight(materials):
            colour = derive.delight(albedo, labels, materials)

        out = {
            "color": colour,
            "roughness": derive.roughness(albedo, labels, materials),
            "metallic": derive.metallic(albedo, labels, materials),
            "height": heightfield,
            "normal": derive.normal_from_height(heightfield, green_down=args.green_down),
            "emissive": derive.emissive(albedo, labels, materials),
        }
        wrote = []
        for kind, arr in out.items():
            if kind == "emissive" and float(arr.max()) <= 0.0:
                continue  # a black emissive map is noise in the output directory
            gray = kind in ("roughness", "metallic", "height")
            images.save(_paths("maps", slug, kind + ".png"), arr, gray=gray)
            wrote.append(kind)

        report[name] = {
            # The key is the BMPNAMES entry as the .rif spells it, so a consumer
            # resolves a polygon's texture index straight to a map set with no
            # slug-reconstruction rule to keep in step with this file.
            "texture": name,
            "width": w,
            "height": h,
            "tiling": rec["tiling"],
            "maps": {kind: "maps/%s/%s.png" % (slug, kind) for kind in wrote},
            "materials": {
                label: {"material": spec.get("material"),
                        "metallic": spec.get("metallic"),
                        "roughness": spec.get("roughness"),
                        "confidence": spec.get("confidence")}
                for label, spec in materials.items()},
            "note": note,
        }
        print("  %-40s %s  [%s]" % (name, note[:44], ",".join(wrote)))

    _write_json(_paths("maps", "index.json"), {
        # Recorded rather than implied: a consumer that samples the normal map with
        # the wrong green convention inverts lighting on one axis everywhere, and
        # nothing in the PNG says which one was written.
        "normal_convention": "directx" if args.green_down else "opengl",
        "colour_space": {"color": "srgb",
                         "roughness": "linear", "metallic": "linear",
                         "height": "linear", "normal": "linear",
                         "emissive": "srgb"},
        "textures": report,
    })
    print("\nwrote %d map sets to %s" % (len(report), _paths("maps")))
    print("index: %s (%s normals)"
          % (_paths("maps", "index.json"), "DirectX" if args.green_down else "OpenGL"))
    if stale:
        moved = collections.Counter(k for _, d in stale for k in d)
        print("%d STALE inputs, which these maps were written anyway from -- %s"
              % (len(stale), ", ".join("%s (%d)" % (cache.MEANS.get(k, k), n)
                                       for k, n in moved.most_common())))
        print("  materials: re-run `classify --refresh-stale`. Generated height or "
              "colour: it was\n  not used, the derived map was; `maps --regenerate` "
              "re-asks for it.")
    return 1 if stale else 0


# ---------------------------------------------------------------------------
# preview
# ---------------------------------------------------------------------------

def cmd_preview(args):
    """Put one generated map into the running game, or take it out again.

    Everything about *why* it is a mod rather than a `render.material_override`,
    and why a normal map does not go through DXT, is in :mod:`gkpbr.preview`.
    """
    game = assets.find_install(args.game)
    if args.remove:
        gone = preview.remove(game, args.mod)
        print("removed %s" % gone if gone else "nothing to remove (no %s mod)" % args.mod)
        return 0

    manifest = _read_json(_paths("manifest.json"))
    name = args.texture.lower()
    rec = manifest.get(name)
    if rec is None:
        print("%s is not in the manifest -- run `inventory` first" % args.texture)
        return 1

    png = _paths("maps", _slug(rec["name"]), args.map + ".png")
    if not os.path.exists(png):
        print("no %s map at %s\n  run: gkpbr maps \"%s\"" % (args.map, png, rec["name"]))
        return 1

    rimutil = preview.find_rimutil(args.rimutil)
    if rimutil is None:
        print("rimutil not found; build it (`cmake --build build`) or pass --rimutil")
        return 1

    index = assets.rim.TextureIndex(os.path.join(game, assets.TEXTURE_DIR))
    fmt = preview.format_for(args.map, args.format)
    dest = preview.target_path(game, rec["name"], args.mod, index)
    said = preview.pack(rimutil, png, dest, fmt)

    print("%s -> %s" % (args.map, rec["name"]))
    print("  %s" % said)
    print("  wrote %s" % dest)
    if rec.get("alpha") and fmt == "body":
        # Not a refusal: rimutil refuses graded alpha itself. This is the case it
        # cannot see -- a sheet whose alpha matters, packed as an encoding whose
        # ALPH the engine ignores, so the preview would be opaque where the stock
        # sheet is not (rif_chunk_format.md, "The engine does not honour an ALPH").
        print("  NOTE this sheet has alpha and `body` cannot carry it -- use dxt3")
    print()
    print("now, with GKPLUS_REPL_PORT set (utils/rendertest):")
    print(preview.REPL_HINT % rec["name"])
    print("  gkpbr preview --remove   # when you are done, ALWAYS")
    return 0


def _summarise(materials):
    """One material phrase and one relief phrase for a whole-sheet prompt."""
    if not materials:
        return "a mixed game texture", "whatever geometric relief the surface has"
    by_conf = sorted(materials.values(), key=lambda m: -float(m.get("confidence", 0)))
    mats = []
    reliefs = []
    for spec in by_conf[:4]:
        if spec.get("material"):
            mats.append(spec["material"])
        if spec.get("relief"):
            reliefs.append(spec["relief"])
    return ("; ".join(dict.fromkeys(mats)) or "a mixed game texture",
            "; ".join(dict.fromkeys(reliefs)) or "whatever relief the surface has")


def _wants_delight(materials):
    return any(m.get("delight") for m in materials.values())


# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(prog="gkpbr")
    ap.add_argument("--game", help="Gunlok directory (default: GUNLOK_DIR or Steam)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    inv = sub.add_parser("inventory", help="find used textures and segment atlases")
    inv.add_argument("textures", nargs="*")
    inv.set_defaults(func=cmd_inventory)

    ob = sub.add_parser("observed", help="what the running game draws each sheet with")
    ob.add_argument("--from", dest="rebuild_from", metavar="HARVEST", nargs="+",
                    help="rebuild render_profile.json from harvest-draws.ps1 dumps "
                         "(several sum: a session is one process and the accumulator "
                         "dies with it)")
    ob.add_argument("--note", help="what run produced it, recorded in the profile")
    ob.add_argument("--renderer", help="GKPLUS_RENDERER the harvest ran under")
    ob.add_argument("--top", type=int, default=40, help="how many sheets to list")
    ob.set_defaults(func=cmd_observed)

    pr = sub.add_parser("probe", help="measure whether the image model holds registration")
    pr.add_argument("textures", nargs="*")
    pr.add_argument("--green-down", action="store_true")
    pr.set_defaults(func=cmd_probe)

    cl = sub.add_parser("classify", help="stage 1: material JSON per texture")
    cl.add_argument("textures", nargs="*")
    cl.add_argument("--model", default=classify.DEFAULT_MODEL)
    cl.add_argument("--force", action="store_true", help="re-classify cached textures")
    # The three ways a cache entry gets past today's fingerprint, in ascending order
    # of what they assert. --refresh-stale buys a new answer for exactly the entries
    # whose inputs moved; --force buys one for everything; --adopt-cached buys nothing
    # and asserts that a pre-fingerprint answer was an answer to today's inputs.
    cl.add_argument("--refresh-stale", action="store_true",
                    help="re-classify only the entries whose inputs have moved")
    cl.add_argument("--adopt-cached", action="store_true",
                    help="stamp today's fingerprint on entries that have none")
    cl.add_argument("--level", help="only the textures this level's assets can show")
    cl.add_argument("--seen-only", action="store_true",
                    help="skip sheets the profiled run never saw drawn")
    cl.set_defaults(func=cmd_classify)

    mp = sub.add_parser("maps", help="stages 2-4: write the map set")
    mp.add_argument("textures", nargs="*")
    mp.add_argument("--generate", action="store_true", help="use the image model for height")
    # Opt-in as a statement about the consumer, not to save money: a per-pixel PBR
    # renderer wants the painted form shade gone, and Gunlok -- reachable through
    # `preview` -- does not, because its per-vertex lighting cannot put one back.
    # See the README's "Baked lighting".
    mp.add_argument("--delight", action="store_true",
                    help="de-light the regions stage 1 says carry painted lighting "
                         "(wrong for a consumer that lights per vertex, like Gunlok)")
    mp.add_argument("--escalate", action="store_true", help="retry on the pro image model")
    mp.add_argument("--regenerate", action="store_true",
                    help="ignore cached model output and call the API again")
    mp.add_argument("--model", default=generate.DEFAULT_MODEL)
    mp.add_argument("--level", help="only the textures this level's assets can show")
    mp.add_argument("--seen-only", action="store_true",
                    help="skip sheets the profiled run never saw drawn")
    mp.add_argument("--green-down", action="store_true",
                    help="DirectX normal convention (-G); default is OpenGL/Blender")
    mp.set_defaults(func=cmd_maps)

    pv = sub.add_parser("preview", help="put one generated map on screen in the game")
    pv.add_argument("texture", nargs="?", default="",
                    help="a manifest key, e.g. \"ground/city ruins ground 1_a.rim\"")
    pv.add_argument("--map", default="normal", choices=MAP_NAMES)
    pv.add_argument("--format", choices=("dxt1", "dxt3", "body"),
                    help="override the .RIM encoding; see preview.DEFAULT_FORMATS")
    pv.add_argument("--mod", default=preview.DEFAULT_MOD)
    pv.add_argument("--rimutil", help="path to rimutil.exe")
    pv.add_argument("--remove", action="store_true",
                    help="delete the preview mod and exit")
    pv.set_defaults(func=cmd_preview)

    args = ap.parse_args(argv)
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main())
