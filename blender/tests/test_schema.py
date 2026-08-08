"""Require the typed field representation to be lossless.

    python blender/tests/test_schema.py "<Gunlok dir>"

This is what lets the scene stop referencing the source file. If
``encode(decode(body)) == body`` for every leaf chunk in every shipped asset, then
a chunk body can live in Blender as typed properties and be rebuilt exactly -- no
opaque bytes anywhere. Anything less and the original file stays load-bearing.
"""

import os
import sys
import collections

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "io_scene_rif"))

import rif  # noqa: E402
import schema  # noqa: E402


def main(game_dir):
    seen = collections.Counter()
    named = collections.Counter()
    generic = collections.Counter()
    failures = []

    paths = sorted(
        os.path.join(dp, nm)
        for dp, _, ns in os.walk(game_dir)
        for nm in ns
        if nm.lower().endswith(".rif")
    )

    for path in paths:
        rel = os.path.relpath(path, game_dir)
        try:
            root = rif.load(path)
        except Exception as exc:  # noqa: BLE001
            failures.append((rel, "-", repr(exc)))
            continue

        for chunk in root.walk():
            if chunk.children is not None:
                continue
            seen[chunk.name] += 1
            if (chunk.id in schema.SCHEMA or chunk.id in schema.STRING_CHUNKS
                    or chunk.id in schema.CODECS):
                named[chunk.name] += 1
            else:
                generic[chunk.name] += 1
            try:
                props = schema.decode(chunk.id, chunk.body)
                again = schema.encode(chunk.id, props)
            except Exception as exc:  # noqa: BLE001
                failures.append((rel, chunk.name, repr(exc)))
                continue
            if again != chunk.body:
                where = next(
                    (i for i in range(min(len(again), len(chunk.body)))
                     if again[i] != chunk.body[i]),
                    min(len(again), len(chunk.body)),
                )
                failures.append(
                    (rel, chunk.name,
                     "re-encoded %d bytes vs %d, first diff at %d"
                     % (len(again), len(chunk.body), where))
                )

    total = sum(seen.values())
    print("leaf chunks round-tripped : %d across %d ids" % (total, len(seen)))
    print("  via a named schema      : %d across %d ids" % (sum(named.values()), len(named)))
    print("  via the typed fallback  : %d across %d ids" % (sum(generic.values()), len(generic)))
    if generic:
        print("  still generic           : %s"
              % ", ".join(k for k, _ in generic.most_common(12)))
    print("failures                  : %d" % len(failures))
    for rel, cid, why in failures[:15]:
        print("    %-34s %-9s %s" % (rel, cid, why))
    if len(failures) > 15:
        print("    ... and %d more" % (len(failures) - 15))
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
