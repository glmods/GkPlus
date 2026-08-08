"""Rebellion ``.RIM`` textures: the IFF container and its S3TC payloads.

Imports no ``bpy``, so ``tests/test_rim.py`` decodes all 513 shipped textures
with Blender absent -- the same split :mod:`rif` and :mod:`shapes` have.

A ``.RIM`` is an ordinary **IFF** file, not a chunk file: 4-character ids,
**big-endian** sizes, odd sizes padded to even, and ``LIST``/``FORM``/``PROP``
group chunks whose body opens with a 4-character type. That is the opposite of
the ``.rif`` container in :mod:`rif` in every one of those respects, which is why
this is a separate module rather than a mode of that one.

The shape is the same in all 513::

    LIST:ILBM
      PROP:ILBM         TRAN          (shared properties; also ALPH, in one file)
      FORM:ILBM         BMHD + S3TC          <- a DXT image
                        BMHD + CMAP + BODY   <- ... or a palettized one
      LIST:MIPM
        FORM:MIPM       CONT (level count), FLAG
        LIST:ILBM       FORM:ILBM per mip level

``PROP:ILBM`` holds only ``TRAN``, never a ``BMHD`` -- every ``BMHD`` sits in a
``FORM:ILBM`` beside the image it describes.

The mip chain is discarded on purpose: Blender generates its own, and a mip
level here is just the same image at half size.

**Both image forms are read, and the selection rule is the engine's.**
``RimOpenAndScan`` enumerates ``ILBM``->``BODY`` *first* and only falls back to
``ILBM``->``S3TC``, so a file carrying both would have its DXT ignored; and
among several palette-depth variants of the same picture ``RimBindImageChunks``
takes the largest ``CMAP`` (its colour cap is "no limit" on a true-colour
destination). :func:`decode` matches both, which is what makes it agree with
what the player would see rather than with whichever chunk comes first.

The palettized form is planar ILBM: a ``CMAP`` of 3-byte RGB entries and a
``BODY`` of ``nPlanes`` MSB-first bitplane rows per scanline, each
``ceil(width/8)`` bytes -- padded to a **byte**, not to the word boundary the
ILBM spec requires -- optionally ByteRun1-compressed, with an ``ALPH`` chunk or
a transparent palette index carrying alpha. 23 of the 513 shipped files use it.
Full layout, offsets and the engine addresses are in ``rif_chunk_format.md``
under "The texture images: .RIM".

**Writing is the palettized path only** (:func:`encode`), because it is exactly
lossless and needs no DXT compressor -- which the addon could not carry, being
dependency-free. ``utils/rimutil`` is where DXT lives, and this module writes
the same bytes it does for the same image so the two can be diffed. What that
costs is size, 2-6x a DXT payload; what it buys is that a texture edited in
Blender reaches the game as the pixels that were painted.

**The S3TC body is a 22-byte header then the raw DXT payload**, which is
measured rather than inferred -- across all 3,423 S3TC chunks in the shipped
textures, the declared payload size, the size implied by the header's own
dimensions and the actual body length agree exactly, in every one:

===== ===== ================================================================
Off   Size  Field
===== ===== ================================================================
0x00  4     zero in all 3,423
0x04  4     the DXT four-character code, **byte-reversed** (``1TXD`` = DXT1)
0x08  6     ``01 35 00 52 02 60``, identical in all 3,423
0x0e  2     width, big-endian
0x10  2     height, big-endian
0x12  4     payload size, big-endian
0x16  ...   DXT1 or DXT3 blocks
===== ===== ================================================================

3,385 of those payloads are DXT1 and 38 are DXT3. Of the 365 distinct textures
the shipped ``.rif`` files actually name, 361 are DXT1, 3 are DXT3, 4 are
missing from the install, and **one is not S3TC at all**: the ``*_fmv_*``
ground textures store three palettized ``CMAP``/``BODY`` variants and no S3TC.
That one decodes to ``None`` rather than to a guess -- the material still gets
its name and its place in the table, only its image is missing.

``BMHD``'s ``nPlanes`` is not a bit depth here (a 256x256 DXT3 font declares 3,
a 512x512 DXT1 declares 17), so it is ignored: the four-character code in the
S3TC header is what says how to decode the payload.
"""

import array
import os
import struct
import sys
import zlib

#: IFF group chunks: the body opens with a 4-character type, then child chunks.
GROUPS = (b"LIST", b"FORM", b"PROP", b"CAT ")

S3TC_HEADER = 22

#: ``BMHD.masking``. The engine's ``RimConvertRows`` dispatches on
#: ``(has ALPH, masking)`` and sets its error 8 for anything else, so ILBM's
#: mask plane (1) and lasso (3) are rejected rather than ignored.
MASK_NONE = 0
MASK_TRANSPARENT_COLOUR = 2

#: ``BMHD.compression``, which discriminates the whole image rather than being a
#: boolean: raw planar, ByteRun1 planar, or "the pixels are in an S3TC chunk".
COMPRESS_NONE = 0
COMPRESS_BYTERUN1 = 1
COMPRESS_S3TC = 2

#: The perceptual channel weights every shipped S3TC chunk carries, per-mille
#: and summing to 999. Nothing in the loader reads them.
S3TC_WEIGHTS = (309, 82, 608)

#: Directories a Gunlok install keeps its textures in. A ``BMPNAMES`` entry is a
#: path relative to the root that holds these (``Units\\baddies3.RIM``).
TEXTURE_DIRS = ("units", "ground", "structures", "bitmaps")

TEXTURE_ROOT_NAME = "graphics"


class RimError(Exception):
    pass


class Texture:
    """One decoded image: 8-bit RGBA, row 0 at the **top**.

    ``format`` is the S3TC four-character code (``DXT1``/``DXT3``) or the string
    ``BODY`` for a palettized image, which has no fourcc at all -- it was named
    ``fourcc`` while this module read the DXT path only.
    """

    __slots__ = ("width", "height", "format", "rgba")

    def __init__(self, width, height, format, rgba):  # noqa: A002 - it is the field's name
        self.width = width
        self.height = height
        self.format = format
        self.rgba = rgba

    @property
    def has_alpha(self):
        """True when any pixel is not fully opaque.

        Worth knowing per image rather than per format: DXT1 carries 1-bit
        alpha only in the blocks whose endpoints are ordered ``c0 <= c1``, so
        "is this DXT1" says nothing about whether the texture is cut out.
        """
        alpha = self.rgba[3::4]
        return alpha.count(255) != len(alpha)

    def __repr__(self):
        return "<Texture %dx%d %s>" % (self.width, self.height, self.format)


# --------------------------------------------------------------------------
# IFF
# --------------------------------------------------------------------------

def iter_chunks(buf, off=0, end=None):
    """Yield ``(id, type, body_start, body_end)`` for one level of an IFF file.

    ``type`` is the group type for ``LIST``/``FORM``/``PROP``/``CAT``, and
    ``None`` for a leaf; ``body_start`` already skips a group's type word.
    """
    end = len(buf) if end is None else end
    while off + 8 <= end:
        cid = bytes(buf[off:off + 4])
        size, = struct.unpack_from(">I", buf, off + 4)
        body, stop = off + 8, off + 8 + size
        if stop > end:
            raise RimError("chunk %r at %d declares %d bytes, %d available"
                           % (cid, off, size, end - body))
        if cid in GROUPS:
            yield cid, bytes(buf[body:body + 4]), body + 4, stop
        else:
            yield cid, None, body, stop
        off = stop + (size & 1)  # IFF pads odd bodies to an even boundary


#: The chunks that make up one candidate image. A ``PROP:ILBM``'s properties are
#: inherited by every ``FORM:ILBM`` beside it in the same list, which is the only
#: reason ``Ground\\tree_alpha.RIM``'s ``ALPH`` is reachable at all.
_INHERITED = (b"BMHD", b"CMAP", b"ALPH")
_OWN = (b"BMHD", b"CMAP", b"BODY", b"S3TC", b"ALPH")


def _collect_variants(buf, cid, kind, body, stop, inherited, out):
    """Append every ``FORM:ILBM`` with an image to ``out``, mip levels excluded.

    A variant is ``{chunk id: (start, end)}``. Recursion stops at a ``MIPM``
    group: a mip level is the same picture at half size, and Blender makes its
    own.
    """
    if kind is None or kind == b"MIPM":
        return

    if cid in (b"LIST", b"CAT "):
        for kid, kkind, kbody, kstop in iter_chunks(buf, body, stop):
            if kid == b"PROP" and kkind == b"ILBM":
                inherited = dict(inherited)
                for pid, _, pbody, pstop in iter_chunks(buf, kbody, kstop):
                    if pid in _INHERITED:
                        inherited[pid] = (pbody, pstop)

    if cid == b"FORM" and kind == b"ILBM":
        found = dict(inherited)
        for kid, _, kbody, kstop in iter_chunks(buf, body, stop):
            if kid in _OWN:
                found[kid] = (kbody, kstop)
        if b"BMHD" in found and (b"BODY" in found or b"S3TC" in found):
            out.append(found)
        return

    for kid, kkind, kbody, kstop in iter_chunks(buf, body, stop):
        _collect_variants(buf, kid, kkind, kbody, kstop, inherited, out)


def _variants(data):
    out = []
    for cid, kind, body, stop in iter_chunks(data, 0, len(data)):
        _collect_variants(data, cid, kind, body, stop, {}, out)
    return out


def _choose(variants):
    """The image the engine would show, or ``None``.

    ``BODY`` before ``S3TC``, and the deepest palette among the ``BODY``
    variants -- both of them ``RimOpenAndScan``/``RimBindImageChunks``' own
    rules rather than document order.
    """
    best, depth = None, -1
    for v in variants:
        if b"BODY" not in v or b"CMAP" not in v:
            continue
        start, end = v[b"CMAP"]
        if end - start > depth:
            best, depth = v, end - start
    if best is not None:
        return best
    return next((v for v in variants if b"S3TC" in v), None)


def _parse_bmhd(data, span):
    start, end = span
    if end - start < 20:
        raise RimError("BMHD is %d bytes" % (end - start))
    (width, height, _x, _y, planes, masking, compression,
     _flags, transparent, _xa, _ya, _pw, _ph) = struct.unpack_from(
        ">HHhhBBBBHBBHH", data, start)
    return {"width": width, "height": height, "planes": planes,
            "masking": masking, "compression": compression,
            "transparent": transparent}


def decode(data):
    """A ``.RIM``'s bytes -> a :class:`Texture`, or ``None`` if it holds no image."""
    chosen = _choose(_variants(data))
    if chosen is None:
        return None
    header = _parse_bmhd(data, chosen[b"BMHD"])
    if b"BODY" in chosen and b"CMAP" in chosen:
        return _decode_body(data, chosen, header)
    return _decode_s3tc(data, chosen[b"S3TC"])


def _decode_s3tc(data, span):
    body, stop = span
    if stop - body < S3TC_HEADER:
        raise RimError("S3TC body is %d bytes, too small for its header" % (stop - body))
    fourcc = bytes(data[body + 4:body + 8])[::-1].decode("latin-1")
    width, height = struct.unpack_from(">HH", data, body + 14)
    declared, = struct.unpack_from(">I", data, body + 18)
    payload = data[body + S3TC_HEADER:stop]
    if declared != len(payload):
        raise RimError("S3TC declares %d payload bytes, body holds %d"
                       % (declared, len(payload)))
    if not width or not height:
        raise RimError("S3TC declares a %dx%d image" % (width, height))

    # DXT1 and DXT3 are the only codes in the shipped set (3,385 and 38 of the
    # 3,423 S3TC chunks), so anything else says so rather than being decoded as
    # whichever of the two it resembles.
    if fourcc == "DXT1":
        return Texture(width, height, fourcc, _decode_dxt1(payload, width, height))
    if fourcc == "DXT3":
        return Texture(width, height, fourcc, _decode_dxt3(payload, width, height))
    raise RimError("unsupported texture format %r" % fourcc)


def load(path):
    with open(path, "rb") as fh:
        return decode(fh.read())


# --------------------------------------------------------------------------
# S3TC
# --------------------------------------------------------------------------

def _rgb565(c, memo={}):  # noqa: B006 - deliberate cache across calls
    """5:6:5 -> an opaque RGBA byte string, with the low bits replicated."""
    out = memo.get(c)
    if out is None:
        r = (c >> 11) & 0x1F
        g = (c >> 5) & 0x3F
        b = c & 0x1F
        out = memo[c] = bytes((
            (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255))
    return out


def _lerp(a, b, num, den):
    return bytes((
        (a[0] * (den - num) + b[0] * num) // den,
        (a[1] * (den - num) + b[1] * num) // den,
        (a[2] * (den - num) + b[2] * num) // den,
        255))


def _colours(c0, c1, opaque):
    """The four-entry block palette.

    In DXT1 the endpoint order picks the mode: ``c0 > c1`` is four opaque
    colours, ``c0 <= c1`` is three plus a transparent one. DXT3 carries its
    alpha separately and always uses the four-colour form, whatever the order.
    """
    a, b = _rgb565(c0), _rgb565(c1)
    if opaque or c0 > c1:
        return a, b, _lerp(a, b, 1, 3), _lerp(a, b, 2, 3)
    return a, b, _lerp(a, b, 1, 2), b"\0\0\0\0"


def _decode_dxt1(data, width, height):
    out = bytearray(width * height * 4)
    stride = width * 4
    bw, bh = (width + 3) // 4, (height + 3) // 4
    if len(data) < bw * bh * 8:
        raise RimError("DXT1 payload is %d bytes, %dx%d needs %d"
                       % (len(data), width, height, bw * bh * 8))
    off = 0
    unpack = struct.unpack_from
    for by in range(bh):
        top = by * 4
        for bx in range(bw):
            c0, c1, bits = unpack("<HHI", data, off)
            off += 8
            pal = _colours(c0, c1, False)
            left = bx * 16
            for py in range(4):
                y = top + py
                if y >= height:
                    break
                row = bits >> (py * 8)
                base = y * stride + left
                if left + 16 <= stride:
                    out[base:base + 16] = (pal[row & 3] + pal[(row >> 2) & 3]
                                           + pal[(row >> 4) & 3] + pal[(row >> 6) & 3])
                else:  # a width that is not a multiple of four; none ship
                    for px in range(min(4, width - bx * 4)):
                        out[base + px * 4:base + px * 4 + 4] = pal[(row >> (px * 2)) & 3]
    return bytes(out)


def _decode_dxt3(data, width, height):
    out = bytearray(width * height * 4)
    stride = width * 4
    bw, bh = (width + 3) // 4, (height + 3) // 4
    if len(data) < bw * bh * 16:
        raise RimError("DXT3 payload is %d bytes, %dx%d needs %d"
                       % (len(data), width, height, bw * bh * 16))
    off = 0
    unpack = struct.unpack_from
    for by in range(bh):
        top = by * 4
        for bx in range(bw):
            alpha, = unpack("<Q", data, off)
            c0, c1, bits = unpack("<HHI", data, off + 8)
            off += 16
            pal = _colours(c0, c1, True)
            left = bx * 16
            for py in range(4):
                y = top + py
                if y >= height:
                    break
                row = bits >> (py * 8)
                arow = alpha >> (py * 16)
                base = y * stride + left
                for px in range(min(4, width - bx * 4)):
                    # 4 bits of alpha per pixel, scaled to 8 by replication.
                    a = (arow >> (px * 4)) & 0xF
                    c = pal[(row >> (px * 2)) & 3]
                    i = base + px * 4
                    out[i:i + 4] = c[:3] + bytes(((a << 4) | a,))
    return bytes(out)


# --------------------------------------------------------------------------
# ByteRun1 (PackBits)
# --------------------------------------------------------------------------

# `n < 0x80` is a literal run of `n + 1` bytes, `n > 0x80` repeats the next byte
# `0x101 - n` times, `n == 0x80` is a no-op. The engine decodes one continuous
# stream rather than restarting it per scanline, so this does too.

def unpack_byterun1(src, want):
    """-> at least ``want`` bytes. The last run may overshoot, as it may in the file."""
    out = bytearray()
    i, end = 0, len(src)
    while len(out) < want:
        if i >= end:
            raise RimError("ByteRun1 stream ran out of input")
        n = src[i]
        i += 1
        if n < 0x80:
            run = n + 1
            if i + run > end:
                raise RimError("ByteRun1 literal run overruns the input")
            out += src[i:i + run]
            i += run
        elif n > 0x80:
            if i >= end:
                raise RimError("ByteRun1 repeat run overruns the input")
            out += src[i:i + 1] * (0x101 - n)
            i += 1
    return out


def pack_byterun1(src):
    out = bytearray()
    i, end = 0, len(src)
    while i < end:
        run = 1
        while i + run < end and run < 128 and src[i + run] == src[i]:
            run += 1
        if run >= 2:
            out.append(0x101 - run)
            out.append(src[i])
            i += run
            continue
        # A literal run, stopped one short of any three-in-a-row so the repeat
        # case can take those instead.
        lit = 1
        while i + lit < end and lit < 128:
            if i + lit + 2 < end and src[i + lit] == src[i + lit + 1] == src[i + lit + 2]:
                break
            lit += 1
        out.append(lit - 1)
        out += src[i:i + lit]
        i += lit
    return bytes(out)


# --------------------------------------------------------------------------
# planar bitplanes
# --------------------------------------------------------------------------

#: **Padded to a byte.** The ILBM specification says a word; Gunlok does not,
#: and the 8-pixel-wide mip levels in the shipped set are what prove it -- they
#: consume exactly ``ceil(width / 8) * planes * height`` bytes.
def plane_row_bytes(width):
    return (width + 7) // 8


#: One byte of a plane row spread so that its eight bits land in eight fields of
#: ``field`` bits each, most significant bit first -- i.e. pixel *x* of the group
#: occupies field *x*. OR-ing the table entry for plane *p*'s byte shifted left by
#: *p* therefore assembles eight whole palette indices at once, which is what
#: keeps this loop off the per-pixel path: the work per group of eight pixels is
#: ``planes`` big-integer ORs and one ``struct.unpack``, not 8 x ``planes`` bit
#: tests.
_SPREAD_ROW = {}

#: The transpose of it, for writing: bit *k* of a byte lands at bit ``k * 8``, so
#: the spread of a palette index has bit *p* sitting in byte *p* -- one byte per
#: plane, in plane order, which ``int.to_bytes`` hands back directly.
_SPREAD_INDEX = None


def _spread_row(field):
    table = _SPREAD_ROW.get(field)
    if table is None:
        table = _SPREAD_ROW[field] = [
            sum(((b >> (7 - k)) & 1) << (k * field) for k in range(8))
            for b in range(256)]
    return table


def _spread_index():
    global _SPREAD_INDEX
    if _SPREAD_INDEX is None:
        _SPREAD_INDEX = [sum(((b >> k) & 1) << (k * 8) for k in range(8))
                         for b in range(256)]
    return _SPREAD_INDEX


def _field_width(planes):
    """Bits per palette index in the group accumulator, and its ``struct`` code."""
    if planes <= 8:
        return 8, "<8B"
    if planes <= 16:
        return 16, "<8H"
    return 32, "<8I"


def decode_planar(blob, width, height, planes, compression, what="BODY"):
    """Planar bitplane rows -> one palette index per pixel, row 0 first."""
    if not 1 <= planes <= 31:
        # `IffBodyDecodeScanline` accumulates into a uint32, so 31 is the real
        # limit rather than the 8 an ordinary ILBM would stop at.
        raise RimError("%s: %d planes is out of range (1..31)" % (what, planes))
    row = plane_row_bytes(width)
    need = row * planes * height
    if compression == COMPRESS_NONE:
        if len(blob) < need:
            raise RimError("%s: holds %d bytes, needs %d" % (what, len(blob), need))
        planar = blob
    else:
        planar = unpack_byterun1(blob, need)

    field, code = _field_width(planes)
    spread = _spread_row(field)
    unpack = struct.Struct(code).unpack
    nbytes = field  # eight fields of `field` bits
    out = []
    for y in range(height):
        base = y * planes * row
        line = []
        for c in range(row):
            acc = 0
            for p, b in enumerate(planar[base + c:base + planes * row:row]):
                if b:
                    acc |= spread[b] << p
            line += unpack(acc.to_bytes(nbytes, "little"))
        del line[width:]  # the last group's padding bits are not pixels
        out += line
    return out


def encode_planar(indices, width, height, planes):
    """The inverse: one palette index per pixel -> planar bitplane rows."""
    row = plane_row_bytes(width)
    out = bytearray(row * planes * height)
    spread = _spread_index()
    for y in range(height):
        base = y * planes * row
        line = indices[y * width:(y + 1) * width]
        for c in range(0, width, 8):
            acc = 0
            for j, i in enumerate(line[c:c + 8]):
                if i:
                    s = spread[i & 0xFF]
                    i >>= 8
                    shift = 64
                    while i:
                        s |= spread[i & 0xFF] << shift
                        i >>= 8
                        shift += 64
                    acc |= s << (7 - j)
            # Byte `p` of the accumulator is plane `p`'s byte for this group, so
            # the whole group scatters into the plane rows in one strided write.
            at = base + (c >> 3)
            out[at:at + planes * row:row] = acc.to_bytes(planes, "little")
    return bytes(out)


# --------------------------------------------------------------------------
# CMAP + BODY
# --------------------------------------------------------------------------

def _decode_body(data, variant, header):
    cstart, cend = variant[b"CMAP"]
    colours = (cend - cstart) // 3
    if not colours:
        # The engine's own error 9: a BODY without a palette means nothing.
        raise RimError("BODY with an empty CMAP")
    width, height = header["width"], header["height"]
    if not width or not height:
        raise RimError("BMHD declares a %dx%d image" % (width, height))
    bstart, bend = variant[b"BODY"]
    indices = decode_planar(data[bstart:bend], width, height, header["planes"],
                            header["compression"])

    alpha = None
    if b"ALPH" in variant:
        astart, aend = variant[b"ALPH"]
        if aend - astart < 6:
            raise RimError("ALPH is %d bytes" % (aend - astart))
        aw, ah, bits, acompression = struct.unpack_from(">HHBB", data, astart)
        if (aw, ah) == (width, height):
            # It reuses the BODY decoder wholesale, with `bits` in the role of
            # the plane count.
            alpha = decode_planar(data[astart + 6:aend], aw, ah, bits,
                                  acompression, "ALPH")
        # An ALPH of another size is ignored rather than fatal: it describes a
        # different image, and the picture is still readable without it.

    lut = [bytes(data[cstart + e * 3:cstart + e * 3 + 3]) + b"\xff"
           for e in range(colours)]
    if header["masking"] == MASK_TRANSPARENT_COLOUR and header["transparent"] < colours:
        lut[header["transparent"]] = lut[header["transparent"]][:3] + b"\0"
    highest = max(indices)
    if highest >= colours:
        # The engine clamps rather than failing, and so does rimutil.
        lut += [lut[0]] * (highest + 1 - colours)
    rgba = bytearray(b"".join(map(lut.__getitem__, indices)))
    if alpha is not None:
        rgba[3::4] = bytes(min(a, 255) for a in alpha)
    return Texture(width, height, "BODY", bytes(rgba))


# --------------------------------------------------------------------------
# writing a palettized .RIM
# --------------------------------------------------------------------------

#: A whole RGBA pixel as one integer, which is what makes the palette pass run at
#: C speed: ``dict.fromkeys`` over the array gives the distinct colours in
#: first-appearance order in one call. Picked by width rather than assumed --
#: ``L`` is 8 bytes on 64-bit Unix and ``I`` is 4 everywhere CPython runs, but a
#: silently wrong item size would shift every colour by a byte.
_U32 = next(c for c in "IL" if array.array(c).itemsize == 4)


class Palette:
    """An image reduced to ``(index per pixel, palette, alpha)``, losslessly.

    ``masking`` is :data:`MASK_TRANSPARENT_COLOUR` when one palette entry stands
    for "transparent" and :data:`MASK_NONE` otherwise; ``alpha`` is a byte per
    pixel when the image needs an ``ALPH`` chunk, and ``None`` when it does not.
    """

    __slots__ = ("indices", "cmap", "planes", "masking", "transparent", "alpha")

    def __init__(self, indices, cmap, planes, masking, transparent, alpha):
        self.indices = indices
        self.cmap = cmap
        self.planes = planes
        self.masking = masking
        self.transparent = transparent
        self.alpha = alpha

    @property
    def colours(self):
        return len(self.cmap) // 3

    def __repr__(self):
        return "<Palette %d colours, %d planes%s%s>" % (
            self.colours, self.planes,
            ", transparent index" if self.masking else "",
            ", ALPH" if self.alpha else "")


def palettize(width, height, rgba):
    """RGBA -> a :class:`Palette` holding exactly the colours the image uses.

    The palette is every distinct colour in first-appearance order, so nothing
    is quantized and ``nPlanes`` follows from the count. That is only viable
    because the engine's colour cap is *unbounded* on a true-colour destination
    and its scanline decoder accumulates into a uint32, which puts the ceiling at
    31 planes rather than the 8 an ordinary ILBM would stop at.

    Alpha goes one of two ways, and the choice is not free: **masking 2 spends a
    palette entry on "transparent" and can therefore carry only one colour
    underneath the transparent texels.** That is lossless exactly when they all
    share one -- and the RGB under a transparent texel is not a don't-care, since
    bilinear filtering blends it into the opaque neighbours, which is where dark
    halos come from. Anything else takes an ``ALPH`` chunk, which keeps all four
    channels.
    """
    n = width * height
    if len(rgba) != n * 4:
        raise RimError("%dx%d needs %d bytes of RGBA, got %d"
                       % (width, height, n * 4, len(rgba)))

    pixels = array.array(_U32)
    pixels.frombytes(bytes(rgba))
    if sys.byteorder != "little":
        pixels.byteswap()
    alphas = bytes(rgba[3::4])
    distinct_alpha = set(alphas)
    any_transparent = 0 in distinct_alpha
    any_translucent = bool(distinct_alpha - {0, 255})

    one_transparent_colour = True
    if any_transparent:
        one_transparent_colour = len(
            {v & 0xFFFFFF for v, a in zip(pixels, alphas) if not a}) == 1
    use_transparent_index = (any_transparent and not any_translucent
                             and one_transparent_colour)

    if use_transparent_index:
        # The transparent entry is its own even when an opaque pixel shares the
        # colour, so the two can never collapse onto one index.
        order = dict.fromkeys(v & 0xFFFFFF for v, a in zip(pixels, alphas) if a)
        table = {v: i for i, v in enumerate(order)}
        transparent = len(order)
        transparent_rgb = next(v & 0xFFFFFF for v, a in zip(pixels, alphas) if not a)
        colours = list(order) + [transparent_rgb]
        indices = [transparent if not a else table[v & 0xFFFFFF]
                   for v, a in zip(pixels, alphas)]
        masking, alpha = MASK_TRANSPARENT_COLOUR, None
    else:
        # Fully opaque means the alpha byte is 0xff in every key, so the raw
        # pixel value is already one key per colour and the masking pass can go.
        keys = pixels if not (any_transparent or any_translucent) else \
            [v & 0xFFFFFF for v in pixels]
        order = dict.fromkeys(keys)
        table = {v: i for i, v in enumerate(order)}
        indices = list(map(table.__getitem__, keys))
        colours = list(order)
        transparent, masking = 0, MASK_NONE
        alpha = alphas if (any_transparent or any_translucent) else None

    cmap = b"".join(struct.pack("<I", v & 0xFFFFFF)[:3] for v in colours)
    planes = 1
    while (1 << planes) < len(colours):
        planes += 1
    if planes > 31:
        raise RimError("%d distinct colours needs more than 31 bitplanes"
                       % len(colours))
    return Palette(indices, cmap, planes, masking, transparent, alpha)


def check_lossless(rgba, palette):
    """Rebuild the image from the palette and fail if it is not the original.

    A silent quantisation bug is invisible in the written file and only shows up
    in the game, so the encoder checks its own work rather than trusting it.
    """
    lut = [palette.cmap[e * 3:e * 3 + 3] + b"\xff"
           for e in range(palette.colours)]
    if palette.masking == MASK_TRANSPARENT_COLOUR:
        lut[palette.transparent] = lut[palette.transparent][:3] + b"\0"
    try:
        rebuilt = bytearray(b"".join(map(lut.__getitem__, palette.indices)))
    except IndexError:
        raise RimError("internal: a palette index is outside the CMAP") from None
    if palette.alpha is not None:
        rebuilt[3::4] = palette.alpha
    if rebuilt != bytes(rgba):
        raise RimError("internal: the palettization is not lossless")


def _chunk(cid, body):
    # IFF pads an odd body to an even boundary; the pad is not in the size.
    return cid + struct.pack(">I", len(body)) + body + (b"\0" if len(body) & 1 else b"")


def _group(cid, kind, children):
    return _chunk(cid, kind + b"".join(children))


def _bmhd(width, height, planes, masking, compression, transparent):
    return _chunk(b"BMHD", struct.pack(">HHhhBBBBHBBHH", width, height, 0, 0,
                                       planes, masking, compression, 0,
                                       transparent, 1, 1, width, height))


def _tran():
    # Every shipped TRAN is eight zero bytes, and `RimBindImageChunks` gates its
    # colour key on the first byte being non-zero, so this is the inert form.
    return _chunk(b"TRAN", b"\0" * 8)


def _empty_mipm():
    # A level count of zero, which several shipped files carry and the engine's
    # mip walk skips over. Blender generates its own mips; the game builds any it
    # wants from the base image.
    return _group(b"LIST", b"MIPM",
                  [_group(b"FORM", b"MIPM", [_chunk(b"CONT", b"\0\0")])])


def encode(width, height, rgba, compress=False):
    """RGBA -> a complete ``.RIM`` as ``BMHD`` + ``CMAP`` + ``BODY``.

    Byte-identical to ``rimutil compress --format body`` for the same image
    (add ``--raw`` unless ``compress``), which is what lets the two writers be
    diffed against each other rather than each being trusted on its own.

    ``compress`` selects ByteRun1 over a raw ``BODY``. It is off by default:
    raw is the form verified end to end in the running game, and packing is a
    per-byte pass in Python over a payload that can reach megabytes.
    """
    if not 1 <= width <= 0xFFFF or not 1 <= height <= 0xFFFF:
        raise RimError("a BMHD dimension is 16 bits; this is %dx%d" % (width, height))
    palette = palettize(width, height, rgba)
    check_lossless(rgba, palette)

    compression = COMPRESS_BYTERUN1 if compress else COMPRESS_NONE
    body = encode_planar(palette.indices, width, height, palette.planes)
    ilbm = [_bmhd(width, height, palette.planes, palette.masking, compression,
                  palette.transparent),
            _chunk(b"CMAP", palette.cmap),
            _chunk(b"BODY", pack_byterun1(body) if compress else body)]

    # PROP:ILBM holds the properties shared by every FORM:ILBM beside it. TRAN is
    # always there; an ALPH joins it, because that is the only scope the loader's
    # lookup can see.
    prop = [_tran()]

    if palette.alpha is not None:
        # The engine's alpha converter cannot survive a wide palette: above 256
        # colours ``RimConvertIndexed_Alpha_NoMask`` @ 0x005defe0 faults --
        # measured, 0xc0000005 at 0x005df14a, on a 40,742-entry re-encode of
        # ``Units\alpha junk.RIM``. The same image at 8 planes renders correctly.
        # This palettizes losslessly and has no quantizer, so refusing is the
        # only honest answer. The cap applies ONLY when an ALPH is emitted: a
        # wide palette with no alpha takes the other converter and renders.
        colours = len(palette.cmap) // 3
        if colours > 256:
            raise RimError(
                "this image needs an ALPH chunk but palettizes to %d colours; "
                "the engine's alpha converter crashes above 256. Reduce the "
                "palette, or encode as DXT3." % colours)

        plane = encode_planar(palette.alpha, width, height, 8)
        # **In the PROP, not the FORM.** An ALPH inside the FORM:ILBM is never
        # found by the loader: measured in the running game, such a file renders
        # bit-for-bit identically to one carrying no ALPH at all (mean
        # 0.0000/255, max 0), while the same chunk in the PROP renders correct
        # graded alpha, matching a 50% blend to within 1/255. The one shipped
        # ALPH, ``Ground\tree_alpha.RIM``, is in the PROP for the same reason.
        prop.append(_chunk(b"ALPH",
                           struct.pack(">HHBB", width, height, 8, compression)
                           + (pack_byterun1(plane) if compress else plane)))

    return _group(b"LIST", b"ILBM",
                  [_group(b"PROP", b"ILBM", prop),
                   _group(b"FORM", b"ILBM", ilbm),
                   _empty_mipm()])


def save(path, width, height, rgba, compress=False):
    with open(path, "wb") as fh:
        fh.write(encode(width, height, rgba, compress))


# --------------------------------------------------------------------------
# PNG
# --------------------------------------------------------------------------

def to_png(texture):
    """RGBA -> PNG bytes.

    Blender has no way to be handed a decoded buffer that survives a save: an
    image created with ``images.new`` and filled through ``pixels`` is a
    *generated* image, and generated images keep their settings but not their
    pixels across a ``.blend`` round trip. Going through a PNG means the image
    can be loaded and then packed, which does survive -- and it hands the sRGB
    tagging to Blender rather than reimplementing a transfer function here.
    ``zlib`` is in the standard library, so this stays dependency-free.
    """
    raw = bytearray()
    stride = texture.width * 4
    for y in range(texture.height):
        raw.append(0)  # filter type 0 (none)
        raw += texture.rgba[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", texture.width, texture.height,
                                         8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
            + chunk(b"IEND", b""))


# --------------------------------------------------------------------------
# finding the textures
# --------------------------------------------------------------------------

def find_texture_root(start):
    """The directory a ``BMPNAMES`` path is relative to, searched for upwards.

    A shipped file sits at ``<game>\\RIF\\Levels\\level01.RIF`` and names
    ``Ground\\swampgreen_V2.RIM``, which lives at
    ``<game>\\Graphics\\Ground\\swampgreen_V2.RIM`` -- so the root is the
    install's ``Graphics``. Returns ``None`` rather than guessing when nothing
    upwards looks like one.
    """
    here = os.path.dirname(os.path.abspath(start))
    for _ in range(6):
        for name in os.listdir(here) if os.path.isdir(here) else ():
            if name.lower() == TEXTURE_ROOT_NAME and os.path.isdir(os.path.join(here, name)):
                return os.path.join(here, name)
        if _looks_like_root(here):
            return here
        parent = os.path.dirname(here)
        if parent == here:
            break
        here = parent
    return None


def _looks_like_root(path):
    if not os.path.isdir(path):
        return False
    names = {n.lower() for n in os.listdir(path)}
    return len(names & set(TEXTURE_DIRS)) >= 2


class TextureIndex:
    """Case-insensitive lookup of a ``BMPNAMES`` path under a root.

    The table stores Windows paths in whatever case the artist typed
    (``Units\\baddies3.RIM`` and ``units\\baddies3.RIM`` both ship, naming the
    same file), so the directory is indexed once and matched folded. 1,597 of
    the 1,601 shipped entries resolve this way; the other four name files that
    are not in the install at all.
    """

    def __init__(self, root):
        self.root = root
        self._by_key = {}
        if not root or not os.path.isdir(root):
            return
        for dirpath, _, names in os.walk(root):
            rel = os.path.relpath(dirpath, root)
            prefix = "" if rel == "." else rel.replace("\\", "/").lower() + "/"
            for name in names:
                self._by_key.setdefault(prefix + name.lower(), os.path.join(dirpath, name))

    def resolve(self, name):
        """A table entry's name -> a path on disk, or ``None``."""
        if not name:
            return None
        return self._by_key.get(name.replace("\\", "/").lstrip("/").lower())
