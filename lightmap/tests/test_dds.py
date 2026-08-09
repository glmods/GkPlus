"""The DDS writer against ``src/Dds.cpp``'s own rules, plus the channel packing.

Run it as a script; it takes no arguments and needs no Gunlok install:

    uv run python tests/test_dds.py

**This deliberately does not follow ``pbr/tests``' convention.** Those append to a
module-level ``FAILURES`` list instead of asserting, which makes them report green
under pytest collection whatever fails (the root ``CLAUDE.md`` says so). Here a
failure is an ``assert``, so the exit code is right under any runner.

The parser below is a **re-derivation of ``ParseHeader``/``Parse``**, not a port of
the writer -- if it were written from ``dds.py`` it would agree with a wrong file
as readily as a right one. Every constant in it comes from ``src/Dds.cpp`` and the
line it comes from is named.
"""

import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from gklightmap import dds, pack  # noqa: E402


# ---------------------------------------------------------------------------
# an independent reader, from src/Dds.cpp
# ---------------------------------------------------------------------------

def parse(data):
    """What ``gk::dds::Parse`` would make of these bytes, or raise.

    Returns ``(format, width, height, [(w, h, offset, size)])``.
    """
    assert len(data) >= 128, "too small to hold a DDS header"
    assert data[:4] == b"DDS ", "bad magic"
    header = data[4:128]
    (size, flags, height, width, _pitch, _depth, mips) = struct.unpack_from("<7I", header, 0)
    assert size == 124, "DDS_HEADER.dwSize is not 124"

    pf = header[72:104]
    (pf_size, pf_flags, fourcc, bits, rm, gm, bm, am) = struct.unpack("<8I", pf)
    assert pf_size == 32, "DDS_PIXELFORMAT.dwSize is not 32"

    if pf_flags & 0x4:  # DDPF_FOURCC
        fmt = {0x31545844: "DXT1", 0x33545844: "DXT3"}.get(fourcc)
        assert fmt, "unsupported block format %08x" % fourcc
    else:
        assert pf_flags & 0x40, "neither DDPF_FOURCC nor DDPF_RGB"
        bgr = rm == 0x00ff0000 and gm == 0x0000ff00 and bm == 0x000000ff
        has_alpha = bool(pf_flags & 0x1)
        if bits == 32 and bgr and (am == 0xff000000 or (not has_alpha and am == 0)):
            fmt = "ARGB32"
        elif bits == 24 and bgr:
            fmt = "RGB24"
        else:
            raise AssertionError("unsupported uncompressed layout")

    caps2 = struct.unpack_from("<I", header, 112)[0]
    assert not caps2 & 0x200, "cubemap"
    assert not caps2 & 0x200000, "volume"
    assert 0 < width <= 8192 and 0 < height <= 8192, "bad dimensions"

    declared = mips if (flags & 0x20000) else 1
    declared = declared or 1

    possible, w, h = 1, width, height
    while w > 1 or h > 1:
        w = max(1, w // 2)
        h = max(1, h // 2)
        possible += 1
    assert declared <= possible, "dwMipMapCount %d exceeds the %d the dimensions allow" % (
        declared, possible)

    compressed = fmt in ("DXT1", "DXT3")
    if compressed:
        assert width % 4 == 0 and height % 4 == 0, "compressed base level is not a multiple of 4"

    def level_bytes(w, h):
        if not compressed:
            return w * h * (3 if fmt == "RGB24" else 4)
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * (8 if fmt == "DXT1" else 16)

    levels, offset, w, h = [], 128, width, height
    for _ in range(declared):
        n = level_bytes(w, h)
        assert offset + n <= len(data), "truncated: the mip chain runs past the end"
        levels.append((w, h, offset, n))
        offset += n
        w = max(1, w // 2)
        h = max(1, h // 2)
    return fmt, width, height, levels


def read_level(data, level, fmt):
    """One uncompressed level back as ``HxWx3`` in R,G,B order (undoing the B,G,R store)."""
    assert fmt == "RGB24"
    w, h, offset, n = level
    arr = np.frombuffer(data, dtype=np.uint8, count=n, offset=offset).reshape(h, w, 3)
    return arr[..., ::-1]


# ---------------------------------------------------------------------------
# the tests
# ---------------------------------------------------------------------------

def _ramp(width, height):
    """A picture with a different, recognisable gradient in each channel."""
    ys, xs = np.mgrid[0:height, 0:width]
    r = (xs * 255 // max(1, width - 1)).astype(np.uint8)
    g = (ys * 255 // max(1, height - 1)).astype(np.uint8)
    b = ((xs + ys) * 255 // max(1, width + height - 2)).astype(np.uint8)
    return np.dstack([r, g, b])


def test_header_is_what_the_engine_accepts():
    blob = dds.encode(_ramp(64, 64))
    fmt, width, height, levels = parse(blob)
    assert fmt == "RGB24", fmt
    assert (width, height) == (64, 64)
    assert len(levels) == 7, levels          # 64,32,16,8,4,2,1
    assert levels[-1][:2] == (1, 1), levels[-1]
    # Nothing trailing, nothing short: the last level ends exactly at EOF.
    assert levels[-1][2] + levels[-1][3] == len(blob), (levels[-1], len(blob))


def test_channels_survive_the_bgr_store():
    """The whole interface is which channel is which; a swap is silent in the engine."""
    src = _ramp(32, 16)
    blob = dds.encode(src, mips=False)
    fmt, _, _, levels = parse(blob)
    back = read_level(blob, levels[0], fmt)
    assert np.array_equal(back, src), "the base level did not survive the round trip"
    # And a specific, hand-checkable pixel: R=10 G=20 B=30 must be stored 30,20,10.
    one = np.zeros((4, 4, 3), dtype=np.uint8)
    one[1, 2] = (10, 20, 30)
    blob = dds.encode(one, mips=False)
    row = 128 + (1 * 4 + 2) * 3
    assert blob[row:row + 3] == bytes((30, 20, 10)), blob[row:row + 3]


def test_non_square_and_non_power_of_two():
    """Uncompressed levels are exempt from the 4x4 floor, so odd sizes must work."""
    for width, height in ((100, 60), (17, 5), (1, 1), (3, 256)):
        blob = dds.encode(_ramp(width, height))
        fmt, w, h, levels = parse(blob)
        assert (w, h) == (width, height)
        assert levels[-1][:2] == (1, 1) or (width, height) == (1, 1)
        assert levels[-1][2] + levels[-1][3] == len(blob)
        assert np.array_equal(read_level(blob, levels[0], fmt), _ramp(width, height))


def test_mip_chain_halves_and_stops_at_one():
    levels = dds.mip_chain(_ramp(128, 32))
    sizes = [(a.shape[1], a.shape[0]) for a in levels]
    assert sizes == [(128, 32), (64, 16), (32, 8), (16, 4), (8, 2), (4, 1), (2, 1), (1, 1)], sizes
    # A box filter on a flat channel must leave it flat -- an overshooting filter
    # is the reason this is BOX and not LANCZOS.
    flat = np.full((64, 64, 3), 200, dtype=np.uint8)
    for level in dds.mip_chain(flat):
        assert level.min() == level.max() == 200, (level.min(), level.max())


def test_no_mips_writes_one_level():
    blob = dds.encode(_ramp(8, 8), mips=False)
    _, _, _, levels = parse(blob)
    assert len(levels) == 1
    assert len(blob) == 128 + 8 * 8 * 3


def test_combine_rejects_mismatched_channels():
    a = np.zeros((4, 4), dtype=np.uint8)
    b = np.zeros((4, 5), dtype=np.uint8)
    try:
        pack.combine(a, a, b)
    except ValueError:
        pass
    else:
        raise AssertionError("combine accepted channels of different sizes")


def test_combine_order_is_bump_metallic_roughness():
    bump = np.full((2, 2), 10, dtype=np.uint8)
    metallic = np.full((2, 2), 20, dtype=np.uint8)
    roughness = np.full((2, 2), 30, dtype=np.uint8)
    rgb = pack.combine(bump, metallic, roughness)
    assert tuple(rgb[0, 0]) == (10, 20, 30), rgb[0, 0]


def test_to_gray_forces_size_and_greyscale():
    from PIL import Image
    buf = pack.to_png_bytes(_ramp(64, 64))
    gray = pack.to_gray(buf, 32, 32)
    assert gray.shape == (32, 32), gray.shape
    assert gray.dtype == np.uint8
    # A JPEG reply (which is what some providers return whatever was asked for)
    # must decode the same way.
    import io
    jpeg = io.BytesIO()
    Image.fromarray(_ramp(64, 64), mode="RGB").save(jpeg, "JPEG")
    gray = pack.to_gray(jpeg.getvalue(), 64, 64)
    assert gray.shape == (64, 64) and gray.dtype == np.uint8


def test_the_reader_can_actually_fail():
    """A harness that cannot fail proves nothing -- so break one on purpose."""
    blob = bytearray(dds.encode(_ramp(8, 8)))
    blob[4:8] = struct.pack("<I", 123)         # dwSize
    try:
        parse(bytes(blob))
    except AssertionError:
        pass
    else:
        raise AssertionError("the reader accepted a header with dwSize 123")

    blob = bytearray(dds.encode(_ramp(8, 8)))
    del blob[-1]                                # truncate the 1x1 level
    try:
        parse(bytes(blob))
    except AssertionError:
        pass
    else:
        raise AssertionError("the reader accepted a truncated file")


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for test in tests:
        try:
            test()
        except Exception as exc:  # noqa: BLE001 - a harness reports rather than dies
            failed += 1
            print("FAIL %s: %s" % (test.__name__, exc))
        else:
            print("ok   %s" % test.__name__)
    print("%d/%d passed" % (len(tests) - failed, len(tests)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
