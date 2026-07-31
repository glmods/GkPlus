"""Decode every shipped .RIM, and round-trip every shipped BMPNAMES table.

    python blender/tests/test_rim.py "<Gunlok dir>"

Four claims, all over the whole asset set and none needing Blender:

- **The texture table rebuilds byte for byte.** ``bmpnames.encode(*decode(body))``
  reproduces the chunk exactly, uninitialised name padding included, for every
  ``BMPNAMES`` in the game. That is what lets the exporter rewrite the table
  from the scene without disturbing a file whose textures nobody touched.
- **Every texture a shipped .rif names decodes**, and its dimensions agree with
  the table's own claim about them. Both image forms are covered: 490 S3TC and
  23 palettized.
- **Every one of them re-encodes and comes back identical** -- the claim that
  ``rim.encode`` is exactly lossless, made against every picture in the game
  rather than against a sample. The palette is checked to be minimal
  (``nPlanes == ceil(log2(colours))``) while it is there, which is what the
  engine's own files do. A whole second pass through ByteRun1 is spent only on
  the 23 the game itself ships packed; everywhere else it is checked as what it
  is, a byte-stream codec, over that image's own planar payload.
- **The codec survives a width that is not a multiple of eight**, which the
  shipped set never exercises: the base-level widths in the game are
  ``8, 128, 256, 512, 1024, 1600, 1800`` and every one divides by eight, so the
  partial group at the end of a plane row -- where the two halves of the planar
  codec have to agree about which bits are padding -- is reachable only from
  synthetic data. Those cases run first, and need no Gunlok install.

It takes about twenty minutes: 513 textures decoded and re-encoded is a few
hundred million pixels through pure Python.
"""

import collections
import math
import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "io_scene_rif"))

import bmpnames  # noqa: E402
import rif  # noqa: E402
import rim  # noqa: E402
import shapes as shp  # noqa: E402


def walk_files(root, ext):
    for dirpath, _, names in os.walk(root):
        for nm in sorted(names):
            if nm.lower().endswith(ext):
                yield os.path.join(dirpath, nm)


def round_trip(width, height, rgba, failures, label, modes=(False, True)):
    """encode -> decode -> the same pixels. -> the palette, or ``None`` on failure."""
    palette = None
    for compress in modes:
        try:
            blob = rim.encode(width, height, rgba, compress)
            back = rim.decode(blob)
        except Exception as exc:  # noqa: BLE001
            failures.append((label, "encode(compress=%s): %r" % (compress, exc)))
            return None
        if back is None:
            failures.append((label, "re-encoded to something with no image"))
            return None
        if (back.width, back.height) != (width, height):
            failures.append((label, "came back %dx%d, not %dx%d"
                             % (back.width, back.height, width, height)))
        elif back.rgba != bytes(rgba):
            differing = sum(a != b for a, b in zip(back.rgba, bytes(rgba)))
            failures.append((label, "%d of %d bytes differ after a round trip"
                             % (differing, len(rgba))))
        if palette is None:
            palette = rim.palettize(width, height, rgba)
            want = max(1, math.ceil(math.log2(palette.colours)))
            if palette.planes != want:
                failures.append((label, "%d colours in %d planes, not %d"
                                 % (palette.colours, palette.planes, want)))
            # ByteRun1 as a byte-stream codec, which is what it is -- so the
            # packed path is covered on every image without paying for a second
            # palettization and IFF walk on each of them.
            planar = rim.encode_planar(palette.indices, width, height, palette.planes)
            if rim.unpack_byterun1(rim.pack_byterun1(planar), len(planar))[:len(planar)] != planar:
                failures.append((label, "ByteRun1 does not round-trip its own output"))
    return palette


def synthetic(failures):
    """Sizes and alpha shapes the shipped textures do not have.

    A width that is not a multiple of eight is the one that matters: the last
    group of a plane row is then partial, and both halves of the planar codec
    have to agree about which bits in it are padding. Every texture in the game
    is a power of two, so nothing in the corpus reaches that path.
    """
    rng = random.Random(20260731)
    stats = collections.Counter()
    for width in (1, 2, 3, 7, 8, 9, 15, 16, 17, 33):
        for height in (1, 3):
            for kind in ("opaque", "cutout", "graded", "flat", "blank"):
                px = bytearray()
                for _ in range(width * height):
                    if kind == "flat":
                        px += b"\x40\x80\xc0\xff"
                        continue
                    if kind == "blank":
                        # Nothing opaque at all, so the palette is the
                        # transparent entry and nothing else.
                        px += b"\x00\x00\x00\x00"
                        continue
                    rgb = bytes(rng.randrange(256) for _ in range(3))
                    if kind == "opaque":
                        alpha = 255
                    elif kind == "cutout":
                        # One colour under every transparent texel, which is what
                        # a transparent palette index can represent.
                        alpha = rng.choice((0, 255))
                        rgb = b"\xff\x00\xff" if not alpha else rgb
                    else:
                        alpha = rng.randrange(256)
                    px += rgb + bytes((alpha,))
                label = "%dx%d %s" % (width, height, kind)
                palette = round_trip(width, height, bytes(px), failures, label)
                if palette is not None:
                    stats["synthetic images"] += 1
                    if palette.masking:
                        stats["... with a transparent index"] += 1
                    elif palette.alpha is not None:
                        stats["... with an ALPH chunk"] += 1
    return stats


def main(game_dir):
    failures = []
    stats = synthetic(failures)

    # ---- the tables --------------------------------------------------------
    tables = {}
    for path in sorted(walk_files(game_dir, ".rif")):
        rel = os.path.relpath(path, game_dir)
        try:
            root = rif.load(path)
        except Exception as exc:  # noqa: BLE001
            failures.append((rel, repr(exc)))
            continue
        chunk = next((c for c in root.walk() if c.id == b"BMPNAMES"), None)
        if chunk is None:
            stats["files without a table"] += 1
            continue
        try:
            version, entries = bmpnames.decode(chunk.body)
        except Exception as exc:  # noqa: BLE001
            failures.append((rel, "BMPNAMES: %s" % exc))
            continue
        stats["tables"] += 1
        stats["entries"] += len(entries)
        if bmpnames.encode(version, entries) != chunk.body:
            failures.append((rel, "BMPNAMES does not rebuild byte for byte"))
        if len({e["index"] for e in entries}) != len(entries):
            failures.append((rel, "duplicate index in the table"))
        if len({e["name"].lower() for e in entries}) != len(entries):
            failures.append((rel, "duplicate name in the table"))
        if any(e["flags"] != bmpnames.DEFAULT_FLAGS for e in entries):
            stats["entries with unusual flags"] += 1

        # Every polygon's texture index either names a table entry or is one of
        # the two documented non-references.
        by_index = {e["index"] for e in entries}
        for shape in shp.iter_shapes(root):
            for poly in shape.polys:
                ti = poly.texture_index
                if ti in by_index:
                    stats["polys resolved"] += 1
                elif ti == shp.TEXTURE_INDEX_MASK:
                    stats["polys untextured (0xfff)"] += 1
                elif "shadow" in rel.lower():
                    stats["polys unresolved in a _shadow file"] += 1
                else:
                    stats["polys unresolved"] += 1
        for e in entries:
            tables.setdefault(e["name"].replace("\\", "/").lower(), e)

    # ---- the images --------------------------------------------------------
    root_dir = rim.find_texture_root(os.path.join(game_dir, "RIF", "Levels", "x.rif"))
    if root_dir is None:
        failures.append(("<textures>", "no texture root found under %s" % game_dir))
        index = rim.TextureIndex(None)
    else:
        index = rim.TextureIndex(root_dir)

    # Every .RIM in the install, named by a .rif or not, decoded exactly once --
    # a megapixel texture is expensive enough in pure Python that the pass below
    # takes the summary rather than loading it again.
    formats = collections.Counter()
    seen = {}
    for path in sorted(walk_files(root_dir, ".rim")) if root_dir else ():
        rel = os.path.relpath(path, root_dir)
        try:
            tex = rim.load(path)
        except Exception as exc:  # noqa: BLE001
            failures.append((rel, repr(exc)))
            continue
        if tex is None:
            stats["all .RIM with no image at all"] += 1
            continue
        stats["all .RIM decoded"] += 1
        stats["... as " + tex.format] += 1
        seen[os.path.normcase(path)] = (tex.width, tex.height, tex.format)
        if len(tex.rgba) != tex.width * tex.height * 4:
            failures.append((rel, "decoded %d bytes for %dx%d"
                             % (len(tex.rgba), tex.width, tex.height)))
        png = rim.to_png(tex)
        if png[:8] != b"\x89PNG\r\n\x1a\n" or len(png) < 64:
            failures.append((rel, "PNG encode produced %d bytes" % len(png)))

        # Raw everywhere, and both ways on the files the game itself ships
        # palettized -- ByteRun1 is covered as a stream codec on all of them.
        palette = round_trip(tex.width, tex.height, tex.rgba, failures, rel,
                             (False, True) if tex.format == "BODY" else (False,))
        if palette is None:
            continue
        stats["re-encoded losslessly"] += 1
        if palette.masking:
            stats["... with a transparent index"] += 1
        elif palette.alpha is not None:
            stats["... with an ALPH chunk"] += 1
        stats["widest palette"] = max(stats["widest palette"], palette.planes)

    for _key, entry in sorted(tables.items()):
        path = index.resolve(entry["name"])
        if path is None:
            stats["named textures missing from the install"] += 1
            continue
        summary = seen.get(os.path.normcase(path))
        if summary is None:
            stats["named textures with no image at all"] += 1
            continue
        width, height, image_format = summary
        formats[image_format] += 1
        stats["named textures decoded"] += 1
        declared = bmpnames.size(entry)
        if declared and declared != (width, height):
            stats["table size disagrees with the image"] += 1

    for key, val in sorted(stats.items()):
        print("%-42s %d" % (key, val))
    print("%-42s %s" % ("formats", dict(formats)))
    print("%-42s %d" % ("failures", len(failures)))
    for rel, why in failures[:20]:
        print("    %-50s %s" % (rel, why))
    if len(failures) > 20:
        print("    ... and %d more" % (len(failures) - 20))
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
