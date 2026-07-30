"""Decode every shipped ``.RIM`` with rimutil, and check the palettized ones.

    python utils/rimutil/tests/test_decode.py <rimutil.exe> "<Gunlok dir>"

Two claims over the whole shipped asset set:

* **Every palettized file decodes to exactly the same pixels as the reference.**
  ``rimref.py`` is a separate implementation written from the format notes, so
  this is a cross-check rather than a codec agreeing with itself. All four
  channels are compared, which is what catches an ``ALPH`` or a masking-2
  transparent index going astray.
* **Every S3TC file still decodes**, at the dimensions its own ``BMHD`` declares.
  This is a regression guard: the palettized path was added to a tool that only
  ever handled S3TC.

The counts are printed rather than hardcoded, so a different install (a demo, a
localised release) reports what it found instead of failing.

**This takes a few minutes.** The reference decoder is pure Python walking one
bit at a time, and the largest shipped image is 1024x1024 at 17 bitplanes -- 17.8
million inner iterations for that file alone. That is the price of the reference
being independent and dependency-free, and it is paid once per run rather than
per build. ``test_encode.py`` is the quick one.
"""

import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import rimref  # noqa: E402


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    rimutil, gunlok = sys.argv[1], sys.argv[2]
    graphics = gunlok
    if not os.path.isdir(os.path.join(graphics, "Ground")):
        graphics = os.path.join(gunlok, "Graphics")
    if not os.path.isdir(graphics):
        print("No Graphics directory under %s" % gunlok)
        return 2

    import tempfile
    work = tempfile.mkdtemp(prefix="rimutil-decode-")
    png = os.path.join(work, "out.png")

    files = []
    for dirpath, _, names in os.walk(graphics):
        for n in names:
            if n.lower().endswith(".rim"):
                files.append(os.path.join(dirpath, n))
    files.sort()

    body_ok = body_bad = s3tc_ok = s3tc_bad = 0
    failures = []

    for path in files:
        data = open(path, "rb").read()
        palettized = b"BODY" in data
        r = subprocess.run([rimutil, "decompress", path, png],
                           capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(png):
            failures.append("%s: rimutil failed: %s"
                            % (os.path.basename(path), r.stderr.strip()))
            if palettized:
                body_bad += 1
            else:
                s3tc_bad += 1
            continue

        got_w, got_h, got = rimref.png_read_rgba(png)

        if palettized:
            ref = rimref.decode_body_file(path)
            if ref is None:
                failures.append("%s: reference found no BODY" % os.path.basename(path))
                body_bad += 1
                continue
            ref_w, ref_h, expect = ref
            if (got_w, got_h) != (ref_w, ref_h):
                failures.append("%s: rimutil %dx%d, reference %dx%d"
                                % (os.path.basename(path), got_w, got_h, ref_w, ref_h))
                body_bad += 1
            elif got != expect:
                differing = sum(1 for a, b in zip(got, expect) if a != b)
                failures.append("%s: %d of %d bytes differ"
                                % (os.path.basename(path), differing, len(expect)))
                body_bad += 1
            else:
                body_ok += 1
        else:
            # No independent DXT decoder here -- squish checked against squish
            # would prove nothing. What is worth asserting is that the image
            # comes back at the size the container declares.
            i = data.find(b"S3TC")
            want_w, want_h = struct.unpack_from(">HH", data, i + 8 + 14)
            if (got_w, got_h) != (want_w, want_h):
                failures.append("%s: decoded %dx%d, S3TC declares %dx%d"
                                % (os.path.basename(path), got_w, got_h, want_w, want_h))
                s3tc_bad += 1
            else:
                s3tc_ok += 1

    for f in failures[:40]:
        print("FAIL %s" % f)
    if len(failures) > 40:
        print("... and %d more" % (len(failures) - 40))

    print()
    print("%-42s %d" % (".RIM files found", len(files)))
    print("%-42s %d" % ("palettized, identical to the reference", body_ok))
    print("%-42s %d" % ("palettized, wrong", body_bad))
    print("%-42s %d" % ("S3TC, decoded at the declared size", s3tc_ok))
    print("%-42s %d" % ("S3TC, wrong", s3tc_bad))

    try:
        os.remove(png)
        os.rmdir(work)
    except OSError:
        pass

    if not files:
        print("\nNo .RIM files found -- is that a Gunlok install?")
        return 2
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
