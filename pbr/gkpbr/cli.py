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


from . import assets, atlas, classify, derive, generate, images, levels, metrics

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
        return names
    return [n.lower() for n in args.textures] or sorted(manifest)


# ---------------------------------------------------------------------------
# inventory
# ---------------------------------------------------------------------------

def cmd_inventory(args):
    game = assets.find_install(args.game)
    print("install: %s" % game)
    wanted = {n.lower() for n in args.textures} or None
    refs = assets.collect(game, wanted)
    print("textures referenced by geometry: %d" % len(refs))

    index = assets.rim.TextureIndex(os.path.join(game, "Graphics"))
    manifest = {}
    skipped = collections.Counter()

    for name in sorted(refs):
        tex = assets.load_texture(game, name, index)
        if tex is None:
            skipped["no image on disk"] += 1
            continue
        albedo = images.from_texture(tex)
        mask, labels, patches, stats = atlas.segment(refs[name], tex.width, tex.height)

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
            "fourcc": tex.fourcc,
            "alpha": tex.has_alpha,
            "rif_count": len({r.rif for r in refs[name]}),
            "polys": len(refs[name]),
            "tiling": bool(tiling),
            "tiling_fraction": round(frac, 3),
            "uv_samples": stats,
            "seam": [round(sx, 2), round(sy, 2)],
            "coverage": round(atlas.coverage(mask), 3),
            "lum_mean": round(float(metrics.luminance(albedo).mean()) * 255, 1),
            "sat_mean": round(float(derive.saturation(albedo).mean()), 3),
            "patches": [p.as_dict() for p in patches],
        }
        manifest[name] = rec
        images.save(_paths("albedo", _slug(name) + ".png"), albedo)
        if labels is not None:
            atlas.save_labels(_paths("labels", _slug(name) + ".png"), labels)
        print("  %-40s %4dx%-4d %s tf %.2f %3d patches cov %.2f" % (
            name, tex.width, tex.height, "tiling" if tiling else "atlas ",
            frac, len(patches), rec["coverage"]))

    _write_json(_paths("manifest.json"), manifest)
    n_atlas = sum(1 for r in manifest.values() if r["patches"])
    print()
    print("wrote %s" % _paths("manifest.json"))
    print("%d textures: %d tiling, %d atlases (%d regions total)" % (
        len(manifest), len(manifest) - n_atlas, n_atlas,
        sum(len(r["patches"]) for r in manifest.values())))
    for why, n in skipped.items():
        print("skipped %d: %s" % (n, why))


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

def cmd_classify(args):
    manifest = _read_json(_paths("manifest.json"))
    names = _select(args, manifest)
    done = skipped = failed = 0
    gaps = collections.Counter()

    for name in names:
        rec = manifest.get(name)
        if rec is None:
            continue
        out = _paths("materials", _slug(name) + ".json")
        if os.path.exists(out) and not args.force:
            skipped += 1
            continue
        albedo_png = _read_bytes(_paths("albedo", _slug(name) + ".png"))
        patches = [_patch_from_dict(p) for p in rec["patches"]]

        region_png = colours = None
        lab_path = _paths("labels", _slug(name) + ".png")
        if patches and os.path.exists(lab_path):
            overlay, colours = classify.region_map(
                atlas.load_labels(lab_path), patches, rec["width"], rec["height"])
            region_png = images.to_png_bytes(overlay)
            images.save(_paths("regions", _slug(name) + ".png"), overlay)
        try:
            regions = classify.classify(rec, patches, albedo_png, region_png, colours,
                                        model=args.model)
        except Exception as exc:  # noqa: BLE001
            print("  %-40s FAILED %s" % (name, exc))
            failed += 1
            continue
        _write_json(out, regions)
        done += 1

        # Reported, not gated: a label with no answer falls back to a safe neutral in
        # `derive`, and an answer for a label that does not exist is simply never
        # looked up -- so neither is fatal, but both are silent, and over 364 textures
        # nobody is going to eyeball them. Spot-checked as 0/0 on the first three.
        real = {str(p["label"]) for p in rec["patches"]}
        missing = sorted(real - set(regions), key=lambda k: int(k))
        extra = sorted(set(regions) - real - {"0"})
        if missing or extra:
            gaps["missing"] += len(missing)
            gaps["extra"] += len(extra)
            print("  %-40s %2d regions  MISSING %s EXTRA %s"
                  % (name, len(regions), missing[:6], extra[:6]))
            continue

        low = [k for k, v in regions.items() if float(v.get("confidence", 1)) < 0.4]
        summary = ", ".join("%s=%s" % (k, v.get("material")) for k, v in
                            list(regions.items())[:3])
        print("  %-40s %2d regions%s  %s"
              % (name, len(regions), " (%d low-conf)" % len(low) if low else "",
                 summary[:52]))

    print("\nclassified %d, cached %d, failed %d -> %s"
          % (done, skipped, failed, _paths("materials")))
    if gaps["missing"] or gaps["extra"]:
        print("region-id mismatches: %d unanswered, %d not in the manifest"
              % (gaps["missing"], gaps["extra"]))


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

def cmd_maps(args):
    manifest = _read_json(_paths("manifest.json"))
    names = _select(args, manifest)
    report = {}

    for name in names:
        rec = manifest.get(name)
        if rec is None:
            continue
        slug = _slug(name)
        albedo = images.load(_paths("albedo", slug + ".png"))
        h, w = albedo.shape[:2]

        mat_path = _paths("materials", slug + ".json")
        materials = _read_json(mat_path) if os.path.exists(mat_path) else {}

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
        gen_height = _paths("generated", slug + "_height.png")
        gen_colour = _paths("generated", slug + "_delit.png")

        if args.generate:
            summary, relief = _summarise(materials)
            got = gnote = None
            if os.path.exists(gen_height) and not args.regenerate:
                got, gnote = images.load(gen_height), "cached"
                if got.ndim == 3:
                    got = metrics.luminance(got)
            else:
                got, gnote = generate.height(albedo, summary, relief,
                                             require_tiling=rec["tiling"],
                                             model=args.model, escalate=args.escalate)
                if got is not None:
                    images.save(gen_height, got, gray=True)

            if got is not None:
                heightfield = derive.fuse_height(got, albedo, labels, materials)
                note = "generated: " + gnote
            else:
                note = "fell back: " + gnote

            if args.delight and _wants_delight(materials):
                if os.path.exists(gen_colour) and not args.regenerate:
                    colour, dnote = images.load(gen_colour), "cached"
                else:
                    delit, dnote = generate.delit_albedo(albedo, summary,
                                                         model=args.model)
                    if delit is not None:
                        images.save(gen_colour, delit)
                    colour = delit if delit is not None else derive.delight(
                        albedo, labels, materials)
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

    pr = sub.add_parser("probe", help="measure whether the image model holds registration")
    pr.add_argument("textures", nargs="*")
    pr.add_argument("--green-down", action="store_true")
    pr.set_defaults(func=cmd_probe)

    cl = sub.add_parser("classify", help="stage 1: material JSON per texture")
    cl.add_argument("textures", nargs="*")
    cl.add_argument("--model", default=classify.DEFAULT_MODEL)
    cl.add_argument("--force", action="store_true", help="re-classify cached textures")
    cl.add_argument("--level", help="only the textures this level's assets can show")
    cl.set_defaults(func=cmd_classify)

    mp = sub.add_parser("maps", help="stages 2-4: write the map set")
    mp.add_argument("textures", nargs="*")
    mp.add_argument("--generate", action="store_true", help="use the image model for height")
    mp.add_argument("--delight", action="store_true", help="also de-light the albedo")
    mp.add_argument("--escalate", action="store_true", help="retry on the pro image model")
    mp.add_argument("--regenerate", action="store_true",
                    help="ignore cached model output and call the API again")
    mp.add_argument("--model", default=generate.DEFAULT_MODEL)
    mp.add_argument("--level", help="only the textures this level's assets can show")
    mp.add_argument("--green-down", action="store_true",
                    help="DirectX normal convention (-G); default is OpenGL/Blender")
    mp.set_defaults(func=cmd_maps)

    args = ap.parse_args(argv)
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main())
