"""Round-trip rimutil's compress verb.

    python utils/rimutil/tests/test_encode.py <rimutil.exe> ["<Gunlok dir>"]

Four claims. The Gunlok directory is optional: without it the synthetic cases
still run, which is what makes this usable on a machine with no install.

* **BODY is exactly lossless.** PNG -> ``.RIM`` -> PNG is bit-identical, in both
  the ByteRun1 and the raw form, for all four channels. Alpha is the interesting
  part: ``masking 2`` can only carry one colour under its transparent texels, so
  the encoder may only choose it when they all share one -- ``tree_alpha.RIM``
  has 790 distinct RGB values under alpha 0, and the RGB *under* transparency is
  not a don't-care because bilinear filtering blends it into its neighbours.
* **BODY carries graded alpha, and refuses it only when the palette is too wide.**
  The engine honours an ``ALPH`` when it is a child of ``PROP:ILBM`` -- measured
  in the running game; a ``FORM``-placed one renders bit-for-bit identically to
  no ``ALPH`` at all. What it cannot survive is an ``ALPH`` alongside more than
  256 colours: ``RimConvertIndexed_Alpha_NoMask`` faults. So a narrow-palette
  graded image must round-trip, and a wide-palette one must be refused. Neither
  of those is a shape these tests can see for themselves, which is why both are
  asserted explicitly.
* **The reference agrees with what the encoder wrote.** rimutil reading its own
  output would prove only that it is self-consistent.
* **DXT1/DXT3 produce a well-formed file that decodes** with a plausible error.
* **The container matches the shape the shipped files use**, and the root chunk
  accounts for the file length exactly.

The synthetic cases exist for the shapes the shipped set does not contain. The
one that matters is an 8-pixel width: ``ceil(8/8)`` is 1, the only odd plane-row
length in practice, and therefore the only place where Gunlok's byte padding and
the ILBM specification's word padding disagree.
"""

import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import rimref  # noqa: E402

# id, generator, width, height
SYNTHETIC = [
    ("w8_gradient", "gradient", 8, 8),    # 1 byte per plane row: the padding trap
    ("w8_cutout", "cutout", 8, 16),
    ("w1x1", "gradient", 1, 1),
    ("w12_odd", "gradient", 12, 5),       # width not a multiple of 8
    ("w24_two", "twocolour", 24, 9),      # 2 colours -> a single bitplane
    ("w320_graded", "graded", 320, 7),    # graded alpha, >256 colours -> refused
    ("w64_gradedsmall", "gradedsmall", 64, 64),  # graded alpha, 16 colours -> round-trips
    ("w64_cutout", "cutout", 64, 64),     # one transparent colour -> masking 2
    ("w300_wide", "wide", 300, 300),      # 90,000 colours -> 17 bitplanes
]


def generate(kind, w, h):
    px = bytearray()
    for y in range(h):
        for x in range(w):
            if kind == "gradient":
                px += bytes(((x * 37 + y * 11) & 0xFF, (x * 5) & 0xFF, (y * 3) & 0xFF, 255))
            elif kind == "twocolour":
                c = 255 if ((x + y) & 1) else 0
                px += bytes((c, c, c, 255))
            elif kind == "cutout":
                if (x + y) % 3 == 0:
                    px += bytes((17, 23, 29, 0))  # every transparent texel alike
                else:
                    px += bytes(((x * 9) & 0xFF, (y * 7) & 0xFF, 64, 255))
            elif kind == "graded":
                px += bytes(((x * 3) & 0xFF, (y * 5) & 0xFF, 128, (x * 13) & 0xFF))
            elif kind == "gradedsmall":
                # 16 distinct greys, so it palettizes well inside the 256-colour
                # cap, with a genuinely graded alpha ramp on top.
                v = (x // 4) * 16
                px += bytes((v, v, v, 128 + ((y * 2) % 64)))
            elif kind == "wide":
                v = y * w + x
                px += bytes(((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, 255))
    return bytes(px)


def run(*argv):
    r = subprocess.run(list(argv), capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip()


def mean_abs_error(a, b, channel):
    total = count = 0
    for i in range(channel, len(a), 4):
        total += abs(a[i] - b[i])
        count += 1
    return total / max(count, 1)


EXPECTED_SHAPE = [
    (0, b"LIST", b"ILBM"), (1, b"PROP", b"ILBM"), (2, b"TRAN", None),
    (1, b"FORM", b"ILBM"), (2, b"BMHD", None), (2, b"S3TC", None),
    (1, b"LIST", b"MIPM"), (2, b"FORM", b"MIPM"), (3, b"CONT", None),
]


def has_graded_alpha(rgba):
    """True when any texel is partially transparent, i.e. it needs an ALPH."""
    return any(a not in (0, 255) for a in rgba[3::4])


def distinct_colours(rgba):
    """Palette size a lossless palettization would need."""
    return len({rgba[i:i + 3] for i in range(0, len(rgba), 4)})


def needs_alph(rgba):
    """True when the alpha can only be carried by an ALPH chunk.

    Mirrors ``classify_alpha`` in rimutil.cpp: opaque needs nothing, a cut-out
    whose transparent texels all share one RGB uses ``masking 2``, and anything
    else -- graded alpha, or several RGBs under transparency -- needs an ALPH.
    """
    alphas = rgba[3::4]
    if all(a == 255 for a in alphas):
        return False
    if any(a not in (0, 255) for a in alphas):
        return True
    under = {rgba[i:i + 3] for i in range(0, len(rgba), 4) if rgba[i + 3] == 0}
    return len(under) > 1


def check_one(rimutil, work, name, src, failures):
    _w, _h, original = rimref.png_read_rgba(src)

    if needs_alph(original) and distinct_colours(original) > 256:
        # An ALPH is delivered correctly, but the engine's alpha converter faults
        # above 256 colours, and this encoder has no quantizer - so refusing is
        # the only honest outcome. See rimutil.cpp's palette cap.
        rim = os.path.join(work, "%s_body_refused.RIM" % name)
        rc, msg = run(rimutil, "compress", src, rim, "--format", "body")
        if rc == 0:
            failures.append("%s body: wide palette needing an ALPH was accepted" % name)
        elif "256" not in msg:
            failures.append("%s body: refused for the wrong reason: %s" % (name, msg))
        elif os.path.exists(rim):
            failures.append("%s body: refused but still wrote %s" % (name, rim))
        else:
            print("   body     refused (alpha needs <=256 colours), as it must be")
    else:
        check_body_roundtrip(rimutil, work, name, src, original, failures)

    for fmt in ("dxt1", "dxt3"):
        rim = os.path.join(work, "%s_%s.RIM" % (name, fmt))
        back = os.path.join(work, "%s_%s.png" % (name, fmt))
        rc, msg = run(rimutil, "compress", src, rim, "--format", fmt)
        if rc:
            if "multiple of 4" in msg:
                print("   %-4s     skipped (%s)" % (fmt, msg.splitlines()[0]))
                continue
            failures.append("%s %s: compress failed: %s" % (name, fmt, msg))
            continue
        rc, msg = run(rimutil, "decompress", rim, back)
        if rc:
            failures.append("%s %s: decompress failed: %s" % (name, fmt, msg))
            continue
        _, _, got = rimref.png_read_rgba(back)
        rgb = sum(mean_abs_error(got, original, c) for c in range(3)) / 3
        alpha = mean_abs_error(got, original, 3)
        print("   %-4s     %9d bytes  rgb MAE=%.2f  alpha MAE=%.2f"
              % (fmt, os.path.getsize(rim), rgb, alpha))
        if rgb > 12:
            failures.append("%s %s: rgb error %.2f is implausibly high" % (name, fmt, rgb))

        if fmt == "dxt3":
            shape, total = rimref.tree(rim)
            got_shape = [(d, i, k) for d, i, k, _ in shape]
            if got_shape != EXPECTED_SHAPE:
                failures.append("%s: container shape is %s" % (name, got_shape))
            elif shape[0][3] + 8 != total:
                # A chunk occupies 8 bytes of header plus its declared size, so
                # the root has to account for the whole file with nothing over.
                failures.append("%s: root declares %d bytes, the file is %d"
                                % (name, shape[0][3] + 8, total))


def check_body_roundtrip(rimutil, work, name, src, original, failures):
    for mode, extra in (("rle", []), ("raw", ["--raw"])):
        rim = os.path.join(work, "%s_body_%s.RIM" % (name, mode))
        back = os.path.join(work, "%s_body_%s.png" % (name, mode))
        rc, msg = run(rimutil, "compress", src, rim, "--format", "body", *extra)
        if rc:
            failures.append("%s body/%s: compress failed: %s" % (name, mode, msg))
            continue
        note = msg
        rc, msg = run(rimutil, "decompress", rim, back)
        if rc:
            failures.append("%s body/%s: decompress failed: %s" % (name, mode, msg))
            continue
        _, _, got = rimref.png_read_rgba(back)
        ref = rimref.decode_body_file(rim)
        lossless = got == original
        agrees = ref is not None and ref[2] == original
        if not lossless:
            differing = sum(1 for a, b in zip(got, original) if a != b)
            failures.append("%s body/%s: not lossless, %d bytes differ"
                            % (name, mode, differing))
        if not agrees:
            failures.append("%s body/%s: the reference disagrees with the encoder"
                            % (name, mode))
        print("   body/%-3s %9d bytes  lossless=%-5s reference-agrees=%-5s  %s"
              % (mode, os.path.getsize(rim), lossless, agrees, note))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    rimutil = sys.argv[1]
    gunlok = sys.argv[2] if len(sys.argv) > 2 else None

    work = tempfile.mkdtemp(prefix="rimutil-encode-")
    failures = []

    print("=== synthetic ===")
    for name, kind, w, h in SYNTHETIC:
        src = os.path.join(work, name + ".png")
        rimref.png_write_rgba(src, w, h, generate(kind, w, h))
        print("%s  %dx%d" % (name, w, h))
        check_one(rimutil, work, name, src, failures)

    if gunlok:
        graphics = gunlok
        if not os.path.isdir(os.path.join(graphics, "Ground")):
            graphics = os.path.join(gunlok, "Graphics")
        # A handful of real textures: one DXT1, and palettized ones covering
        # cut-out alpha, a big palette and a plain opaque image.
        wanted = ["lava.RIM", "tree_alpha.RIM", "save screen.RIM",
                  "Command Wheel 01.RIM"]
        found = {}
        for dirpath, _, names in os.walk(graphics):
            for n in names:
                if n in wanted and n not in found:
                    found[n] = os.path.join(dirpath, n)
        if found:
            print()
            print("=== shipped textures ===")
        for n in wanted:
            if n not in found:
                continue
            name = os.path.splitext(n)[0].replace(" ", "_")
            src = os.path.join(work, name + "_src.png")
            rc, msg = run(rimutil, "decompress", found[n], src)
            if rc:
                failures.append("%s: could not decode the source: %s" % (n, msg))
                continue
            w, h, _ = rimref.png_read_rgba(src)
            print("%s  %dx%d" % (n, w, h))
            check_one(rimutil, work, name, src, failures)

    print()
    src = os.path.join(work, SYNTHETIC[0][0] + ".png")
    rc, _ = run(rimutil, "compress", src, os.path.join(work, "nope.RIM"),
                "--format", "dxt5")
    print("dxt5 refused: %s" % (rc != 0))
    if rc == 0:
        failures.append("dxt5 was accepted; Gunlok cannot load it")

    print()
    for f in failures:
        print("FAIL %s" % f)
    print("failures: %d" % len(failures))

    for f in os.listdir(work):
        try:
            os.remove(os.path.join(work, f))
        except OSError:
            pass
    try:
        os.rmdir(work)
    except OSError:
        pass

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
