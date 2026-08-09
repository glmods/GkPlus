"""A contact sheet of one texture's maps across several runs.

There is no gate in this tool -- ``lightmap/README.md`` says so and says why -- so
looking at the pictures side by side is the whole quality instrument. This lays out
one row per run: albedo, bump, metallic, roughness, labelled.

    uv run python tools/contact_sheet.py out/models sheet.png
    uv run python tools/contact_sheet.py out/models sheet.png --texture ground__gunlok\\ rust

``root`` is a directory of ``GKLIGHTMAP_OUT`` trees (one per model, say), or a
single one.
"""

import argparse
import os
import sys

from PIL import Image, ImageDraw

MAPS = ("albedo", "bump", "metallic", "roughness")


def _runs(root, texture=None):
    """``[(label, directory)]`` -- every output tree under ``root`` holding maps."""
    out = []
    for dirpath, _, names in os.walk(root):
        if "albedo.png" not in names:
            continue
        rel = os.path.relpath(dirpath, root).replace(os.sep, "/")
        label, _, tex = rel.rpartition("/")
        if texture and tex != texture:
            continue
        out.append((label or tex, dirpath))
    return sorted(out)


def build(root, out_path, tile=320, texture=None):
    runs = _runs(root, texture)
    if not runs:
        raise SystemExit("no runs with an albedo.png under %s" % root)
    pad, head = 4, 22
    width = len(MAPS) * (tile + pad) + pad
    height = len(runs) * (tile + head + pad) + pad + head
    sheet = Image.new("RGB", (width, height), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for column, name in enumerate(MAPS):
        draw.text((pad + column * (tile + pad) + 4, 5), name, fill=(220, 220, 220))
    for row, (label, directory) in enumerate(runs):
        top = head + pad + row * (tile + head + pad)
        draw.text((pad + 4, top), label, fill=(255, 200, 120))
        for column, name in enumerate(MAPS):
            path = os.path.join(directory, name + ".png")
            box = (pad + column * (tile + pad), top + head)
            if not os.path.isfile(path):
                draw.rectangle([box, (box[0] + tile, box[1] + tile)], outline=(120, 60, 60))
                draw.text((box[0] + 8, box[1] + 8), "missing", fill=(200, 120, 120))
                continue
            image = Image.open(path).convert("RGB").resize((tile, tile), Image.LANCZOS)
            sheet.paste(image, box)
    sheet.save(out_path)
    print("%d runs -> %s" % (len(runs), out_path))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("root", help="a GKLIGHTMAP_OUT tree, or a directory of them")
    parser.add_argument("out", help="where to write the sheet")
    parser.add_argument("--tile", type=int, default=320)
    parser.add_argument("--texture", help="only this texture's slug, when a tree holds several")
    args = parser.parse_args(argv)
    return build(args.root, args.out, args.tile, args.texture)


if __name__ == "__main__":
    sys.exit(main())
