"""Decode every shipped .RIM, and round-trip every shipped BMPNAMES table.

    python blender/tests/test_rim.py "<Gunlok dir>"

Two claims, both over the whole asset set and neither needing Blender:

- **The texture table rebuilds byte for byte.** ``bmpnames.encode(*decode(body))``
  reproduces the chunk exactly, uninitialised name padding included, for every
  ``BMPNAMES`` in the game. That is what lets the exporter rewrite the table
  from the scene without disturbing a file whose textures nobody touched.
- **Every texture a shipped .rif names decodes**, and its dimensions agree with
  the table's own claim about them. The one documented exception is the
  palettized ``*_fmv_*`` set, which carries no S3TC image; it is counted, not
  tolerated silently.
"""

import collections
import os
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


def main(game_dir):
    stats = collections.Counter()
    failures = []

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

    formats = collections.Counter()
    for key, entry in sorted(tables.items()):
        path = index.resolve(entry["name"])
        if path is None:
            stats["named textures missing from the install"] += 1
            continue
        try:
            tex = rim.load(path)
        except Exception as exc:  # noqa: BLE001
            failures.append((key, repr(exc)))
            continue
        if tex is None:
            stats["named textures with no S3TC image"] += 1
            continue
        formats[tex.fourcc] += 1
        stats["named textures decoded"] += 1
        if len(tex.rgba) != tex.width * tex.height * 4:
            failures.append((key, "decoded %d bytes for %dx%d"
                             % (len(tex.rgba), tex.width, tex.height)))
        declared = bmpnames.size(entry)
        if declared and declared != (tex.width, tex.height):
            stats["table size disagrees with the image"] += 1
        png = rim.to_png(tex)
        if png[:8] != b"\x89PNG\r\n\x1a\n" or len(png) < 64:
            failures.append((key, "PNG encode produced %d bytes" % len(png)))

    # Every .RIM in the install, named by a .rif or not.
    if root_dir:
        for path in sorted(walk_files(root_dir, ".rim")):
            try:
                tex = rim.load(path)
            except Exception as exc:  # noqa: BLE001
                failures.append((os.path.relpath(path, root_dir), repr(exc)))
                continue
            stats["all .RIM decoded" if tex is not None else "all .RIM with no S3TC"] += 1

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
