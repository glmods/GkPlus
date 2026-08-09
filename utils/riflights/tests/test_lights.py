#!/usr/bin/env python3
"""Cross-check `src/Rif.cpp` against the Python decoder over every shipped `.rif`.

    python utils/riflights/tests/test_lights.py <riflights.exe> "<Gunlok dir>"

`src/Rif.cpp` and `blender/io_scene_rif` are two independent implementations of the same
measurements in `rif_chunk_format.md` -- different languages, different authors, written months
apart -- so agreeing on all 3,794 shipped lights is real evidence about the format rather than one
decoder agreeing with itself. That is the whole reason `src/Rif` is pure: nothing else in `src/`
can be run without Gunlok, and a decoder verified only by "the game did not crash" is not
verified.

These `test_*` functions **assert**, so the file is safe under any runner -- the `lightmap/`
convention, not `pbr/`'s. Run it as a script and read the exit code.
"""

import importlib.util
import os
import struct
import subprocess
import sys

FAILED = []


def load_module(name, path):
    """Import one file from `blender/io_scene_rif` without importing the package.

    The package's `__init__` imports `bpy`, which does not exist outside Blender; `rif.py` and
    `schema.py` deliberately do not, which is what makes this possible at all.
    """
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def rif_files(gunlok_dir):
    root = os.path.join(gunlok_dir, "RIF")
    found = []
    for directory, _, names in os.walk(root):
        for name in names:
            if name.lower().endswith(".rif"):
                found.append(os.path.join(directory, name))
    return sorted(found)


def python_light_set(rif, schema, path):
    """The reference reading: every STDLIGHT in the file, in document order."""
    tree = rif.load(path)
    lights = []
    name = None
    ambience = None

    def walk(chunk):
        nonlocal name, ambience
        for child in (getattr(chunk, "children", None) or ()):
            if child.id == b"STDLIGHT":
                lights.append(schema.decode(b"STDLIGHT", child.body))
            elif child.id == b"LTSETHDR":
                name = child.body[:8].split(b"\0")[0].decode("ascii", "replace")
            elif child.id == b"AMBIENCE":
                ambience = struct.unpack("<i", child.body[:4])[0]
            walk(child)

    walk(tree)
    return lights, name, ambience


def cpp_light_sets(exe, paths, batch=40):
    """`riflights` over `paths`, parsed back into {path: (lights, name, ambience)}.

    Batched because a single command line carrying 563 absolute paths runs past Windows' limit,
    and the failure is a truncated argument list rather than an error.
    """
    out = {}
    for start in range(0, len(paths), batch):
        chunk = paths[start:start + batch]
        result = subprocess.run([exe] + chunk, capture_output=True, text=True)
        if result.returncode != 0:
            FAILED.append("riflights exited %d: %s" % (result.returncode, result.stderr.strip()))
        current = None
        for line in result.stdout.splitlines():
            parts = line.split("\t")
            if parts[0] == "file":
                path = parts[1]
                name = parts[3].split("=", 1)[1]
                ambience = int(parts[4].split("=", 1)[1])
                current = ([], name, ambience)
                out[path] = current
            elif parts[0] == "light" and current is not None:
                current[0].append(parts[1:])
    return out


def check(condition, message):
    if not condition:
        FAILED.append(message)
    return condition


def test_every_shipped_rif(exe, gunlok_dir):
    rif = load_module("rif", os.path.join(REPO, "blender", "io_scene_rif", "rif.py"))
    schema = load_module("schema", os.path.join(REPO, "blender", "io_scene_rif", "schema.py"))

    paths = rif_files(gunlok_dir)
    assert paths, "no .rif files under %s" % gunlok_dir
    cpp = cpp_light_sets(exe, paths)

    total_lights = 0
    files_with_lights = 0
    for path in paths:
        want_lights, want_name, want_ambience = python_light_set(rif, schema, path)
        got = cpp.get(path)
        if not check(got is not None, "%s: riflights printed nothing" % path):
            continue
        got_lights, got_name, got_ambience = got

        if not check(len(got_lights) == len(want_lights),
                     "%s: %d lights, expected %d" % (path, len(got_lights), len(want_lights))):
            continue
        if want_lights:
            files_with_lights += 1
        total_lights += len(want_lights)

        # Presence is a separate question from the value: a file with no AMBIENCE prints 0 and
        # marks it absent, so only compare where the reference found one.
        if want_ambience is not None:
            check(got_ambience == want_ambience,
                  "%s: ambience %d, expected %d" % (path, got_ambience, want_ambience))
        if want_name is not None:
            check(got_name == want_name,
                  "%s: light set name %r, expected %r" % (path, got_name, want_name))

        for index, (got_row, want) in enumerate(zip(got_lights, want_lights)):
            where = "%s light %d" % (path, index)
            # Integers must be EXACT. These are the fields a renderer positions and ranges a
            # light by, and a tolerance here would hide a decode that is off by a field.
            check(int(got_row[0]) == want["light_id"], "%s: id" % where)
            check([int(v) for v in got_row[1:4]] == list(want["position"]), "%s: position" % where)
            check(int(got_row[5]) == want["spread"], "%s: spread" % where)
            check(int(got_row[6]) == want["range"], "%s: range" % where)
            check(int(got_row[7], 16) == want["colour"], "%s: colour" % where)
            check(int(got_row[8]) == want["flags"], "%s: engine flags" % where)
            check(int(got_row[9]) == want["local_flags"], "%s: local flags" % where)
            # The two 16.16 fields are the only ones that become floats, and both sides divide
            # the same int by 65536 - one in float, one in double - then print to six decimals.
            check(abs(float(got_row[4]) - want["brightness"] / 65536.0) < 1e-5,
                  "%s: brightness" % where)
            for column in range(9):
                check(abs(float(got_row[10 + column]) - want["orientation"][column] / 65536.0)
                      < 1e-5, "%s: orientation[%d]" % (where, column))

    print("%d files, %d with a light set, %d lights compared"
          % (len(paths), files_with_lights, total_lights))
    # The shipped totals, asserted so a decoder that silently finds nothing cannot pass. Both
    # numbers are from rif_chunk_format.md and are measured over the whole set.
    assert total_lights == 3794, "expected 3,794 shipped lights, compared %d" % total_lights
    assert files_with_lights == 38, \
        "expected 38 files with at least one light, saw %d" % files_with_lights


REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    exe, gunlok_dir = sys.argv[1], sys.argv[2]
    test_every_shipped_rif(exe, gunlok_dir)
    if FAILED:
        for line in FAILED[:40]:
            print("FAIL", line)
        print("%d failures" % len(FAILED))
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
