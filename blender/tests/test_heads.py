"""Measure the OBJHEAD1/SHPHEAD1 record layer against every shipped .rif.

    python blender/tests/test_heads.py "<Gunlok dir>"

Two separate claims, and only the first is a hard pass/fail:

- **Reading is exact.** Decomposing a header and rebuilding it from the pieces
  reproduces the original bytes, ignoring only the uninitialised padding after a
  trailing name (``SQUARE.RIF`` ends its object name ``'SQUARE\\0C'``). If this
  fails, some field is being read at the wrong offset.
- **Regeneration is faithful.** The fields :func:`heads.sync_shphead` recomputes
  from geometry -- ``num_verts``, ``num_polys``, ``radius`` and both bound
  corners -- are compared against what the file stores. This is the evidence for
  regenerating them on export rather than carrying a header that goes stale the
  moment a mesh is edited. Only ``radius`` is expected to drift, and the run
  prints by how much rather than asserting a threshold nobody measured.
"""

import collections
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "io_scene_rif"))

import heads  # noqa: E402
import rif  # noqa: E402
import shapes as shp  # noqa: E402


def rif_files(root):
    for dirpath, _, names in os.walk(root):
        for nm in names:
            if nm.lower().endswith(".rif"):
                yield os.path.join(dirpath, nm)


def zero_from(body, offset):
    """``body`` with everything from ``offset`` zeroed.

    Used to mask the alignment padding after a trailing name, which is genuinely
    uninitialised in the shipped files -- ``SQUARE.RIF`` pads its object name
    with ``'C'`` -- and is the one thing a rebuilt header is not asked to
    reproduce. The offset is computed from the name lengths rather than found by
    scanning for a NUL, because junk padding need not start with one.
    """
    raw = bytearray(body)
    for i in range(min(offset, len(raw)), len(raw)):
        raw[i] = 0
    return bytes(raw)


def check_objhead(body, counters, failures, where):
    counters["objhead"] += 1
    name = heads.objhead_name(body)
    if len(body) < heads.OBJHEAD1_HEADER:
        failures.append((where, "OBJHEAD1 is %d bytes, header is %d"
                         % (len(body), heads.OBJHEAD1_HEADER)))
        return

    counters["objhead named"] += bool(name)

    # Rebuilt from the fields this module claims are there.
    rebuilt = heads.make_objhead(
        name,
        location=struct.unpack_from("<3i", body, heads.OBJHEAD1_LOCATION),
        orientation=struct.unpack_from("<4f", body, heads.OBJHEAD1_ORIENT),
        shape_id=heads.objhead_shape_id(body),
        flags=struct.unpack_from("<i", body, heads.OBJHEAD1_FLAGS)[0],
        index_num=struct.unpack_from("<i", body, heads.OBJHEAD1_INDEX_NUM)[0],
        version=struct.unpack_from("<i", body, heads.OBJHEAD1_VERSION)[0],
        lock_user=body[heads.OBJHEAD1_LOCK_USER:
                       heads.OBJHEAD1_LOCK_USER + heads.LOCK_USER_SIZE],
    )
    # lock_user is authored, not derived -- how often it holds anything is the
    # evidence that it has to be carried rather than zeroed.
    counters["objhead lock_user set"] += any(
        body[heads.OBJHEAD1_LOCK_USER:heads.OBJHEAD1_LOCK_USER + heads.LOCK_USER_SIZE])

    pad_from = heads.OBJHEAD1_NAME + len(name) + 1
    if zero_from(rebuilt, pad_from) != zero_from(body, pad_from):
        failures.append((where, "OBJHEAD1 %r does not rebuild" % name))
        return
    counters["objhead rebuilt"] += 1

    # And the two edits the UI performs must not disturb anything else.
    if heads.objhead_name(heads.set_objhead_name(body, "Renamed")) != "Renamed":
        failures.append((where, "OBJHEAD1 %r rename did not take" % name))
    elif heads.set_objhead_name(heads.set_objhead_name(body, "Renamed"), name) \
            != zero_from(body, pad_from):
        failures.append((where, "OBJHEAD1 %r rename is not reversible" % name))
    elif heads.objhead_shape_id(heads.set_objhead_shape_id(body, 4242)) != 4242:
        failures.append((where, "OBJHEAD1 %r shape id did not take" % name))
    else:
        counters["objhead edits"] += 1


def check_shphead(chunk, counters, failures, where, drift):
    head = chunk.find(b"SHPHEAD1")
    shape = shp.read_shape(chunk)
    if head is None or shape is None:
        return
    body = head.body
    counters["shphead"] += 1

    if len(body) < heads.SHPHEAD1_HEADER:
        failures.append((where, "SHPHEAD1 is %d bytes" % len(body)))
        return

    names = heads.shphead_names(body)
    counters["shphead names"] += len(names)

    stored = struct.unpack_from("<iif6i", body, heads.SHPHEAD1_NUM_VERTS)
    lo, hi = heads.shape_bounds(shape.verts)
    counters["num_verts"] += stored[0] == len(shape.verts)
    counters["num_polys"] += stored[1] == len(shape.polys)
    counters["bounds"] += tuple(stored[3:]) == (hi[0], lo[0], hi[1], lo[1], hi[2], lo[2])

    got = heads.shape_radius(shape.verts)
    exact = struct.pack("<f", got) == struct.pack("<f", stored[2])
    counters["radius exact"] += exact
    if not exact and stored[2] > 0:
        drift.append(abs(got - stored[2]) / stored[2])

    # The full rebuild: what export will now write for an unedited shape. The
    # padding after the last name is uninitialised in the shipped files, exactly
    # as it is in OBJHEAD1, so it is masked off on both sides.
    rebuilt = heads.sync_shphead(body, heads.shphead_file_id(body), shape.verts,
                                 len(shape.polys), names)
    pad_from = heads.SHPHEAD1_HEADER + sum(len(n) + 1 for n in names)
    want = zero_from(body, pad_from)
    got = zero_from(rebuilt, pad_from)
    if got == want:
        counters["shphead rebuilt"] += 1
    elif len(rebuilt) != len(body):
        failures.append((where, "SHPHEAD1 rebuilt to %d bytes, was %d"
                         % (len(rebuilt), len(body))))
    else:
        # Only the radius word may differ; anything else is a layout error.
        differing = {off for off in range(0, len(want), 4)
                     if got[off:off + 4] != want[off:off + 4]}
        if differing - {heads.SHPHEAD1_RADIUS}:
            failures.append((where, "SHPHEAD1 differs at %s"
                             % sorted(hex(o) for o in differing)))
        else:
            counters["shphead rebuilt but radius"] += 1


def check_sequence(chunk, counters, failures, where):
    """The timing rule, against a real sequence's frames.

    Two properties, and they pull against each other -- which is the whole
    reason `sequence_times` is anchored rather than formulaic:

    - **an untouched sequence re-times to exactly what it had**, because every
      key is an anchor; and
    - **a key inserted between two of them lands between their times**, in order,
      without disturbing either.
    """
    frames = [k for k in (chunk.children or ()) if k.id == b"OBASEQFR"]
    if len(frames) < 2:
        return
    counters["sequence"] += 1

    times = [struct.unpack_from("<i", k.body, 0x1C)[0] for k in frames]
    if times != sorted(times):
        return  # 973 shipped sequences are not monotonic; not this test's business
    counters["sequence monotonic"] += 1

    # Import places a key at `time/65536 * duration * fps`; the exact scale does
    # not matter here, only that the mapping is order-preserving.
    positions = [t / 65536.0 * 24.0 for t in times]
    anchors = list(zip(positions, times))

    if heads.sequence_times(positions, anchors) == times:
        counters["retimes exactly"] += 1
    else:
        failures.append((where, "sequence does not re-time to itself"))
        return

    # Now insert a position halfway between the first two keys.
    mid = (positions[0] + positions[1]) / 2.0
    if not (positions[0] < mid < positions[1]):
        return
    counters["insertion tested"] += 1
    grown = sorted(positions + [mid])
    got = heads.sequence_times(grown, anchors)
    if len(got) != len(times) + 1:
        failures.append((where, "inserted key did not produce a frame"))
    elif got != sorted(got) or len(set(got)) != len(got):
        failures.append((where, "inserted key broke the ordering: %s" % got[:5]))
    elif [g for g, f in zip(got, grown) if any(abs(f - a) < 1e-9 for a, _t in anchors)] \
            != times:
        failures.append((where, "inserted key disturbed an existing time"))
    else:
        counters["insertion clean"] += 1


def main(game_dir):
    counters = collections.Counter()
    failures = []
    drift = []

    paths = sorted(rif_files(game_dir))
    if not paths:
        print("no .rif files under %s" % game_dir)
        return 1

    for path in paths:
        rel = os.path.relpath(path, game_dir)
        try:
            root = rif.load(path)
        except Exception as exc:  # noqa: BLE001 - the test reports, it does not raise
            failures.append((rel, repr(exc)))
            continue
        for chunk in root.walk():
            if chunk.id == b"OBJHEAD1":
                check_objhead(chunk.body, counters, failures, rel)
            elif chunk.id in (b"REBSHAPE", b"SUBSHAPE"):
                check_shphead(chunk, counters, failures, rel, drift)
            elif chunk.id == b"OBANSEQC":
                check_sequence(chunk, counters, failures, rel)

    n_obj = counters["objhead"]
    n_shp = counters["shphead"]

    def pct(k, total):
        return "%d/%d (%.3f%%)" % (counters[k], total, 100.0 * counters[k] / total) \
            if total else "0/0"

    print("files                    : %d" % len(paths))
    print("OBJHEAD1 seen            : %d, %d named" % (n_obj, counters["objhead named"]))
    print("  rebuilds exactly       : %s" % pct("objhead rebuilt", n_obj))
    print("  lock_user non-empty    : %s" % pct("objhead lock_user set", n_obj))
    print("  rename / id edits ok   : %s" % pct("objhead edits", n_obj))
    print("SHPHEAD1 seen            : %d, %d object name(s)"
          % (n_shp, counters["shphead names"]))
    print("  num_verts regenerated  : %s" % pct("num_verts", n_shp))
    print("  num_polys regenerated  : %s" % pct("num_polys", n_shp))
    print("  bounds regenerated     : %s" % pct("bounds", n_shp))
    print("  radius bit-exact       : %s" % pct("radius exact", n_shp))
    if drift:
        print("    when not            : %d shapes, max relative drift %.2e, mean %.2e"
              % (len(drift), max(drift), sum(drift) / len(drift)))
    print("  whole header exact     : %s" % pct("shphead rebuilt", n_shp))
    print("  exact but for radius   : %s" % pct("shphead rebuilt but radius", n_shp))
    n_seq = counters["sequence monotonic"]
    print("sequences (2+ frames)    : %d, %d monotonic" % (counters["sequence"], n_seq))
    print("  re-time to themselves  : %s" % pct("retimes exactly", n_seq))
    print("  clean key insertion    : %s" % pct("insertion clean",
                                                counters["insertion tested"]))
    print("failures                 : %d" % len(failures))
    for rel, why in failures[:20]:
        print("    %-40s %s" % (rel, why))
    if len(failures) > 20:
        print("    ... and %d more" % (len(failures) - 20))
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
