"""An independent reference implementation of the ``.RIM`` palettized path.

This exists to disagree with ``rimutil``. It is a second, deliberately separate
decoder written from `rif_chunk_format.md` rather than from `rimutil.cpp`, so
"the two agree on every pixel of all 23 shipped palettized files" is evidence
about the format rather than a tautology about one codec. It was itself checked
against the shipped set before ``rimutil`` could read a ``BODY`` at all: every
one of the 77 palettized images (palette variants and mip levels included)
consumes its ``BODY`` stream *exactly*, and every index lands inside its own
``CMAP``.

Standard library only -- no ``spng``, no ``squish``, no Blender -- which is what
lets it run anywhere ``rimutil`` can be built.

The three things it is easy to get wrong, and which this file therefore states
rather than assumes:

* a ``BODY`` plane row is ``ceil(width/8)`` bytes padded to a **byte**, not to
  the even boundary the ILBM specification requires;
* ByteRun1 is decoded as one continuous stream across the whole image, not
  restarted per scanline;
* ``PROP:ILBM`` supplies shared properties to every ``FORM:ILBM`` beside it,
  which is the only reason ``tree_alpha.RIM``'s ``ALPH`` is reachable.
"""

import struct
import zlib

GROUPS = (b"LIST", b"FORM", b"PROP", b"CAT ")


# --------------------------------------------------------------------------
# PNG, just enough of it
# --------------------------------------------------------------------------

def png_read_rgba(path):
    """-> (width, height, RGBA bytes). 8-bit, non-interlaced, which is what spng writes."""
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s is not a PNG" % path)
    off, idat, ihdr = 8, bytearray(), None
    while off < len(d):
        ln, = struct.unpack_from(">I", d, off)
        tag = d[off + 4:off + 8]
        body = d[off + 8:off + 8 + ln]
        if tag == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
        off += 12 + ln
    w, h, depth, ctype, _comp, _filt, interlace = ihdr
    if depth != 8 or ctype != 6 or interlace != 0:
        raise ValueError("unsupported PNG: %r" % (ihdr,))
    raw = zlib.decompress(bytes(idat))
    bpp, stride = 4, w * 4
    out = bytearray(h * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        f = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                c = prev[i - bpp] if i >= bpp else 0
                b = prev[i]
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif f != 0:
            raise ValueError("bad PNG filter %d" % f)
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, bytes(out)


def png_write_rgba(path, w, h, pixels):
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter 0 (none)
        raw += pixels[y * w * 4:(y + 1) * w * 4]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    open(path, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
        + chunk(b"IEND", b""))


# --------------------------------------------------------------------------
# IFF
# --------------------------------------------------------------------------

def children(buf, off, end):
    """One level: (id, group type or None, body start, body end). Odd bodies are padded."""
    out = []
    while off + 8 <= end:
        cid = buf[off:off + 4]
        size, = struct.unpack_from(">I", buf, off + 4)
        body, stop = off + 8, off + 8 + size
        if stop > end:
            break
        if cid in GROUPS:
            out.append((cid, buf[body:body + 4], body + 4, stop))
        else:
            out.append((cid, None, body, stop))
        off = stop + (size & 1)
    return out


def tree(path):
    """Flat [(depth, id, group type or None, declared body size)] for structural checks.

    The size is the one written in the chunk header, so a chunk occupies
    ``8 + size`` bytes on disk (plus a pad byte when it is odd). Note that
    :func:`children` already advances past a group's 4-byte type, so that has to
    be added back rather than subtracted again.
    """
    d = open(path, "rb").read()
    out = []

    def walk(off, end, depth):
        for cid, kind, body, stop in children(d, off, end):
            out.append((depth, cid, kind, stop - body + (4 if kind else 0)))
            if kind is not None:
                walk(body, stop, depth + 1)

    walk(0, len(d), 0)
    return out, len(d)


def variants(buf, off=None, end=None, inherited=None, out=None):
    """Every base-image ILBM holding a BODY, with PROP properties folded in.

    Mip levels are skipped: a ``LIST:MIPM`` holds the same picture at half size.
    """
    off = 0 if off is None else off
    end = len(buf) if end is None else end
    inherited = {} if inherited is None else inherited
    out = [] if out is None else out

    kids = children(buf, off, end)
    props = dict(inherited)
    for cid, kind, body, stop in kids:
        if cid == b"PROP" and kind == b"ILBM":
            for pid, _, pbody, pstop in children(buf, body, stop):
                props[pid] = (pbody, pstop)
    for cid, kind, body, stop in kids:
        if kind is None or kind == b"MIPM":
            continue
        if cid == b"FORM" and kind == b"ILBM":
            v = dict(props)
            for sid, _, sbody, sstop in children(buf, body, stop):
                v[sid] = (sbody, sstop)
            if b"BMHD" in v and b"BODY" in v:
                out.append(v)
        else:
            variants(buf, body, stop, props, out)
    return out


# --------------------------------------------------------------------------
# the palettized image
# --------------------------------------------------------------------------

def unpack_byterun1(src, want):
    """n < 0x80: literal run of n+1. n > 0x80: repeat next byte 0x101-n. 0x80: no-op."""
    out = bytearray()
    i = 0
    while len(out) < want:
        if i >= len(src):
            raise ValueError("ByteRun1 ran out of input")
        n = src[i]
        i += 1
        if n < 0x80:
            out += src[i:i + n + 1]
            i += n + 1
        elif n > 0x80:
            out += bytes([src[i]]) * (0x101 - n)
            i += 1
    return bytes(out), i


def decode_planar(blob, width, height, planes, compression):
    """-> list of palette indices, one per pixel.

    Plane rows are padded to a byte. That only shows at widths where
    ``ceil(width/8)`` is odd -- i.e. 8 -- and it is the single place Gunlok
    departs from the ILBM specification.
    """
    row = (width + 7) // 8
    need = row * planes * height
    if compression == 0:
        if len(blob) < need:
            raise ValueError("raw plane data is %d bytes, need %d" % (len(blob), need))
        planar = blob[:need]
    else:
        planar, _ = unpack_byterun1(blob, need)

    idx = [0] * (width * height)
    o = 0
    for y in range(height):
        base = y * width
        for p in range(planes):
            cur = 0
            for x in range(width):
                if (x & 7) == 0:
                    cur = planar[o]
                    o += 1
                idx[base + x] |= ((cur >> 7) & 1) << p
                cur = (cur << 1) & 0xFF
    return idx


def decode_body_file(path):
    """A palettized ``.RIM`` -> (width, height, RGBA bytes), or None if it has no BODY.

    Picks the variant with the largest ``CMAP``, which is what the engine does:
    its colour cap is "no limit" on any true-colour destination.
    """
    d = open(path, "rb").read()
    vs = variants(d)
    if not vs:
        return None
    v = max(vs, key=lambda v: (v[b"CMAP"][1] - v[b"CMAP"][0]) if b"CMAP" in v else -1)
    if b"CMAP" not in v:
        raise ValueError("BODY with no CMAP")  # the engine's error 9

    hb, _ = v[b"BMHD"]
    w, h, _x, _y, planes, masking, comp, _flags, transparent = \
        struct.unpack_from(">HHhhBBBBH", d, hb)

    bb, be = v[b"BODY"]
    idx = decode_planar(d[bb:be], w, h, planes, comp)

    cb, ce = v[b"CMAP"]
    pal = d[cb:ce]
    colours = len(pal) // 3

    alpha = None
    if b"ALPH" in v:
        ab, ae = v[b"ALPH"]
        aw, ah, abits, acomp = struct.unpack_from(">HHBB", d, ab)
        if (aw, ah) == (w, h):
            alpha = decode_planar(d[ab + 6:ae], aw, ah, abits, acomp)

    out = bytearray(w * h * 4)
    for i, e in enumerate(idx):
        if e >= colours:
            raise ValueError("index %d outside a %d-entry CMAP" % (e, colours))
        out[i * 4 + 0] = pal[e * 3 + 0]
        out[i * 4 + 1] = pal[e * 3 + 1]
        out[i * 4 + 2] = pal[e * 3 + 2]
        if alpha is not None:
            out[i * 4 + 3] = min(alpha[i], 255)
        elif masking == 2 and e == transparent:
            out[i * 4 + 3] = 0
        else:
            out[i * 4 + 3] = 255
    return w, h, bytes(out)


def has_chunk(path, cid):
    return cid in open(path, "rb").read()
