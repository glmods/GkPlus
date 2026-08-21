"""Three prompts, one texture, one ``.dds``.

    uv run python -m gklightmap.cli gen "ground/gunlok rust.rim"
    uv run python -m gklightmap.cli gen "ground/gunlok rust.rim" --install preview
    uv run python -m gklightmap.cli pack "ground/gunlok rust.rim"       # no API calls
    uv run python -m gklightmap.cli albedo "ground/gunlok rust.rim"     # no API calls
    uv run python -m gklightmap.cli install "ground/gunlok rust.rim" --mod preview
    uv run python -m gklightmap.cli install --remove --mod preview

Every stage writes to disk, so the expensive one runs once: ``gen`` leaves the
three greyscale PNGs beside the ``.dds``, ``pack`` rebuilds the ``.dds`` from them,
and ``gen --map bump`` re-asks for one channel and reuses the other two. Editing a
channel by hand and re-running ``pack`` is the supported repair, which is the same
bargain ``pbr``'s stage-1 JSON strikes.
"""

import argparse
import json
import os
import shutil
import sys
import time

from PIL import Image

from . import dds, openrouter, pack, prompts, source

OUT = os.environ.get("GKLIGHTMAP_OUT") or os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "out")

#: The suffix ``src/VkLighting.cpp``'s ``Candidates`` looks for first. It accepts
#: ``_lighting.dds`` too; the spaced spelling is the one written, because a space in
#: an asset name is normal in this game and the file sits beside its ``.RIM``.
SUFFIX = " lighting.dds"


def _out_dir(rel):
    path = os.path.join(OUT, source.slug(rel))
    os.makedirs(path, exist_ok=True)
    return path


def _albedo_png(directory):
    return os.path.join(directory, "albedo.png")


def _map_png(directory, name):
    return os.path.join(directory, name + ".png")


def _dds_path(directory, rel):
    return os.path.join(directory, os.path.basename(source.stem(rel)) + SUFFIX)


def _install_path(game_dir, mod, rel):
    """Where a mod has to put the file for the engine to find it.

    ``src/VkLighting`` probes ``graphics/<stem> lighting.dds`` in the mod VFS before
    the real file, and ``src/Vfs`` mounts every directory under
    ``<Gunlok>\\gkplus\\mods``. So the mod mirrors ``Graphics`` exactly, which is what
    ``pbr``'s ``preview`` does for a ``.RIM`` and for the same reason.
    """
    return os.path.join(game_dir, "gkplus", "mods", mod, "Graphics",
                        *(source.stem(rel) + SUFFIX).split("/"))


# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------

def cmd_albedo(args):
    """Decode the ``.RIM`` and write what the model will be shown. No API calls."""
    game_dir = source.find_install()
    path, rel = source.resolve(args.texture, game_dir)
    albedo = source.load_albedo(path)
    directory = _out_dir(rel)
    out = _albedo_png(directory)
    Image.fromarray(albedo, mode="RGB").save(out, optimize=True)
    print("%s  %dx%d -> %s" % (rel, albedo.shape[1], albedo.shape[0], out))
    return 0


def cmd_gen(args):
    """Ask for each map, then pack. The one command that spends money."""
    game_dir = source.find_install()
    path, rel = source.resolve(args.texture, game_dir)
    albedo = source.load_albedo(path)
    height, width = albedo.shape[:2]
    directory = _out_dir(rel)
    Image.fromarray(albedo, mode="RGB").save(_albedo_png(directory), optimize=True)

    wanted = args.map or list(prompts.ORDER)
    reference = pack.to_png_bytes(albedo)
    size = args.size or ("%dx%d" % (width, height))
    key = openrouter.api_key()

    meta = {"texture": rel, "source": path, "width": width, "height": height,
            "model": args.model, "size": size, "maps": {}}
    meta_path = os.path.join(directory, "meta.json")
    if os.path.isfile(meta_path):
        try:
            with open(meta_path) as fh:
                meta["maps"] = json.load(fh).get("maps", {})
        except ValueError:
            pass

    total = 0.0
    for name in prompts.ORDER:
        target = _map_png(directory, name)
        if name not in wanted:
            print("%-9s %s" % (name, ("reused " + target) if os.path.isfile(target)
                                     else "MISSING - not packing yet"))
            continue
        prompt = prompts.prompt_for(name, source.stem(rel))
        started = time.time()
        costs = []
        draws = []
        for _ in range(max(1, args.samples)):
            data, usage = openrouter.generate(
                prompt, reference, model=args.model, size=size, key=key,
                timeout=args.timeout)
            draws.append(pack.to_gray(data, width, height))
            per_call = openrouter.cost_of(usage)
            if per_call is not None:
                costs.append(per_call)
        # None, not 0.0, when the provider reported nothing -- the readout says
        # "cost n/a" rather than claiming the call was free.
        cost = sum(costs) if costs else None
        gray = pack.median(draws)
        pack.save_gray(target, gray)
        total += cost or 0.0
        meta["maps"][name] = {
            "prompt": prompt, "model": args.model, "cost": cost,
            "samples": len(draws),
            "sample_means": [round(float(d.mean()) / 255.0, 4) for d in draws],
            "seconds": round(time.time() - started, 1),
            "mean": round(float(gray.mean()) / 255.0, 4),
            "std": round(float(gray.std()) / 255.0, 4),
        }
        print("%-9s %5.1fs  mean %.3f  std %.3f  %s  -> %s" % (
            name, time.time() - started, gray.mean() / 255.0, gray.std() / 255.0,
            ("$%.4f" % cost) if cost is not None else "cost n/a", target))

    with open(meta_path, "w") as fh:
        json.dump(meta, fh, indent=1)
    if total:
        print("spent $%.4f" % total)

    # **A partial run is legal and does not pack.** Each channel is a separate
    # request and they are bought from different models -- the Units bump comes from
    # gemini and the other two from gpt-image-2 -- so a first pass that buys one
    # channel of a texture with none is an ordinary step, not an error. Requiring all
    # three up front forced a throwaway call per texture purely to satisfy the check.
    absent = [n for n in prompts.ORDER if not os.path.isfile(_map_png(directory, n))]
    if absent:
        print("not packed: no %s yet for %s" % (" or ".join(absent), rel))
        return 0

    out = _pack(directory, rel, width, height, mips=not args.no_mips)
    if args.install:
        _install(game_dir, args.install, rel, out)
    return 0


def _pack(directory, rel, width=None, height=None, mips=True):
    channels = []
    for name in prompts.ORDER:
        path = _map_png(directory, name)
        if not os.path.isfile(path):
            raise SystemExit("missing %s; run `gen` first" % path)
        channels.append(pack.load_gray(path, width, height))
    rgb = pack.combine(*channels)
    out = _dds_path(directory, rel)
    dds.write(out, rgb, mips=mips)
    print("packed %dx%d %s -> %s (%.1f MB%s)" % (
        rgb.shape[1], rgb.shape[0], "/".join(prompts.ORDER), out,
        os.path.getsize(out) / 1e6, ", full mip chain" if mips else ", no mips"))
    return out


def cmd_pack(args):
    """Rebuild the ``.dds`` from the PNGs on disk. No API calls."""
    _, rel = source.resolve(args.texture)
    _pack(_out_dir(rel), rel, mips=not args.no_mips)
    return 0


METADATA_README = """\
# gklightmap preview

A **throwaway** mod, written by `gklightmap install` to put a generated
`<stem> lighting.dds` in front of the running game. It is not a distributable mod
and holds whichever texture was last installed.

Remove it with:

    gklightmap install --remove --mod %s

The three channels are `src/VkLighting.h`'s and two do not mean what their names
mean elsewhere: R is a height field, G is highlight intensity, B is highlight
sharpness. See `lightmap/README.md`.
"""


def _write_metadata(game_dir, mod):
    """Writes the mod's ``metadata`` directory: every mod is expected to have one.

    A mod without it still loads and still enables -- ``mods.load`` reports what is
    missing in ``mod.problems`` and falls back to the name on disk -- but this one
    is ours, and a mod that names itself in the load order is one less thing to
    wonder about when a leftover preview turns up. No ``website``: every field is
    optional and inventing a URL would put a wrong one in front of whoever reads
    it.
    """
    root = os.path.join(game_dir, "gkplus", "mods", mod, "metadata")
    os.makedirs(root, exist_ok=True)
    with open(os.path.join(root, "info.json"), "w") as handle:
        json.dump({"name": "gklightmap preview", "author": "gklightmap",
                   "license": "same as GkPlus", "version": "1"}, handle, indent=2)
        handle.write("\n")
    with open(os.path.join(root, "README.md"), "w") as handle:
        handle.write(METADATA_README % mod)
    return root


def _install(game_dir, mod, rel, built):
    target = _install_path(game_dir, mod, rel)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    shutil.copyfile(built, target)
    _write_metadata(game_dir, mod)
    print("installed -> %s" % target)
    # Nothing scans for mods and there is no mods directory - a mod is only ever
    # enabled by being named (mod_loading_notes.md) - so writing the file does not
    # put it in front of the engine. enable(...mods, x) keeps what was enabled and
    # puts this on top. The absolute path is used because a relative one resolves
    # against GKPLUS_PROFILE while this always writes under <Gunlok>\gkplus.
    root = os.path.join(game_dir, "gkplus", "mods", mod).replace(os.sep, "/")
    print("  ENABLE IT FIRST, in the REPL -- nothing scans for mods:")
    print("    mods.enable(...mods, mods.load(String.raw`%s`))" % root)
    print("  then the engine finds the file by name alone; nothing registers it.")
    print("  Check with")
    print("  `render.lighting_maps` / `render.describe_lighting()` in the REPL, and")
    print("  REMOVE IT AFTERWARDS: `install --remove --mod %s`." % mod)
    return target


def cmd_install(args):
    game_dir = source.find_install()
    root = os.path.join(game_dir, "gkplus", "mods", args.mod)
    if args.remove:
        if os.path.isdir(root):
            shutil.rmtree(root)
            print("removed %s" % root)
        else:
            print("nothing at %s" % root)
        return 0
    _, rel = source.resolve(args.texture, game_dir)
    built = _dds_path(_out_dir(rel), rel)
    if not os.path.isfile(built):
        raise SystemExit("no %s; run `gen` or `pack` first" % built)
    _install(game_dir, args.mod, rel, built)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="gklightmap", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    def texture_arg(p, required=True):
        p.add_argument("texture", nargs=None if required else "?",
                       help="a .RIM path, or a BMPNAMES-style name like "
                            "'ground/cracks.rim'")

    p = sub.add_parser("albedo", help="decode the .RIM to PNG and stop")
    texture_arg(p)
    p.set_defaults(func=cmd_albedo)

    p = sub.add_parser("gen", help="generate the three maps and pack the .dds")
    texture_arg(p)
    p.add_argument("--model", default=openrouter.DEFAULT_MODEL,
                   help="OpenRouter model slug (default: %(default)s)")
    p.add_argument("--map", action="append", choices=prompts.ORDER,
                   help="only (re)generate this map; repeatable. The others are "
                        "reused from disk")
    p.add_argument("--size", help="what to ask the endpoint for: a tier ('1K') or "
                                  "pixels ('1024x1024'). Default is the source's "
                                  "own size; the reply is resized back either way")
    p.add_argument("--samples", type=int, default=1, metavar="N",
                   help="ask N times per map and keep the per-pixel median. 1 (the "
                        "default) is one call and is bit-identical to not passing "
                        "this. Use 3 where the answer is unstable between runs -- "
                        "the unit atlases are, by a wider margin than any prompt "
                        "edit moved them")
    p.add_argument("--timeout", type=int, default=300, help="seconds per request")
    p.add_argument("--install", metavar="MOD",
                   help="also copy the result into <Gunlok>\\gkplus\\mods\\MOD")
    p.add_argument("--no-mips", action="store_true",
                   help="write the base level only. Not recommended: a mip-less map "
                        "with texel-scale content sparkles at minification")
    p.set_defaults(func=cmd_gen)

    p = sub.add_parser("pack", help="rebuild the .dds from the PNGs on disk")
    texture_arg(p)
    p.add_argument("--no-mips", action="store_true")
    p.set_defaults(func=cmd_pack)

    p = sub.add_parser("install", help="copy a built .dds into a mod, or remove it")
    texture_arg(p, required=False)
    p.add_argument("--mod", default="gklightmap-preview",
                   help="mod directory name (default: %(default)s)")
    p.add_argument("--remove", action="store_true",
                   help="delete the whole mod directory. Do this when you are done: "
                        "a leftover mod goes on serving in every later session")
    p.set_defaults(func=cmd_install)

    args = parser.parse_args(argv)
    if args.command == "install" and not args.remove and not args.texture:
        parser.error("install needs a texture, or --remove")
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
