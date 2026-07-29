"""Round-trip every shipped .rif through the format core.

    python blender/tests/test_roundtrip.py "<Gunlok dir>"

The claim under test is the one the pass-through writer rests on: parsing a file
and re-serializing it reproduces the input **byte for byte**, so an exporter that
rewrites only the chunks it understands cannot disturb the ~450k chunks it does
not.

Compressed files are compared against their *decompressed* bytes. Reproducing the
original Huffman stream would need a bit-identical encoder, which is not what the
writer does -- it emits uncompressed, which Gunlok reads.
"""

import os
import sys
import struct
import collections

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "io_scene_rif"))

import rif  # noqa: E402


def rif_files(root):
    for dirpath, _, names in os.walk(root):
        for nm in names:
            if nm.lower().endswith(".rif"):
                yield os.path.join(dirpath, nm)


def main(game_dir):
    stats = collections.Counter()
    failures = []
    chunk_ids = collections.Counter()
    total_chunks = 0

    paths = sorted(rif_files(game_dir))
    if not paths:
        print("no .rif files under %s" % game_dir)
        return 1

    for path in paths:
        rel = os.path.relpath(path, game_dir)
        try:
            with open(path, "rb") as fh:
                raw = fh.read()
            stats["compressed" if raw[:8] == rif.COMPRESSED_MAGIC else "uncompressed"] += 1

            plain = rif.decompress(raw)
            if plain[:8] != rif.ROOT_MAGIC:
                failures.append((rel, "root is %r, not REBINFF2" % plain[:8]))
                continue

            declared, = struct.unpack_from("<I", plain, 8)
            if declared != len(plain):
                failures.append((rel, "root size %d != stream length %d" % (declared, len(plain))))
                continue

            root = rif.parse(plain)
            again = rif.serialize(root)
            if again != plain:
                where = next((i for i in range(min(len(again), len(plain)))
                              if again[i] != plain[i]), min(len(again), len(plain)))
                failures.append((rel, "re-serialized %d bytes vs %d, first diff at %d"
                                 % (len(again), len(plain), where)))
                continue

            opaque = 0
            for c in root.walk():
                total_chunks += 1
                chunk_ids[c.name] += 1
                if c.id in rif.CONTAINERS and c.children is None:
                    opaque += 1
            if opaque:
                failures.append((rel, "%d container(s) kept opaque" % opaque))
                continue

            stats["round-trip exact"] += 1
        except Exception as exc:  # noqa: BLE001 - the test reports, it does not raise
            failures.append((rel, repr(exc)))

    print("files              : %d" % len(paths))
    print("  compressed       : %d" % stats["compressed"])
    print("  uncompressed     : %d" % stats["uncompressed"])
    print("round-trip exact   : %d" % stats["round-trip exact"])
    print("chunks visited     : %d across %d distinct ids" % (total_chunks, len(chunk_ids)))
    print("failures           : %d" % len(failures))
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
