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
      PROP:ILBM         BMHD (shared properties) + TRAN
      FORM:ILBM         BMHD + S3TC          <- the image
      LIST:MIPM
        FORM:MIPM       CONT, FLAG
        LIST:ILBM       FORM:ILBM per mip level, each BMHD + S3TC

Only the first ``FORM:ILBM`` carrying an ``S3TC`` is read. The mip chain is
discarded on purpose: Blender generates its own, and a mip level here is just
the same image at half size.

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

import os
import struct
import zlib

#: IFF group chunks: the body opens with a 4-character type, then child chunks.
GROUPS = (b"LIST", b"FORM", b"PROP", b"CAT ")

S3TC_HEADER = 22

#: Directories a Gunlok install keeps its textures in. A ``BMPNAMES`` entry is a
#: path relative to the root that holds these (``Units\\baddies3.RIM``).
TEXTURE_DIRS = ("units", "ground", "structures", "bitmaps")

TEXTURE_ROOT_NAME = "graphics"


class RimError(Exception):
    pass


class Texture:
    """One decoded image: 8-bit RGBA, row 0 at the **top**."""

    __slots__ = ("width", "height", "fourcc", "rgba")

    def __init__(self, width, height, fourcc, rgba):
        self.width = width
        self.height = height
        self.fourcc = fourcc
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
        return "<Texture %dx%d %s>" % (self.width, self.height, self.fourcc)


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


def _find_image(buf, off, end, depth=0):
    """The first ``FORM:ILBM`` holding an ``S3TC``, as ``(body_start, body_end)``."""
    for cid, kind, body, stop in iter_chunks(buf, off, end):
        if cid == b"FORM" and kind == b"ILBM":
            for kid, _, kbody, kstop in iter_chunks(buf, body, stop):
                if kid == b"S3TC":
                    return kbody, kstop
        # PROP holds shared properties, not an image, and the mip chain is a
        # LIST:MIPM: neither is descended into.
        elif kind is not None and depth < 2 and kind != b"MIPM" and cid != b"PROP":
            got = _find_image(buf, body, stop, depth + 1)
            if got:
                return got
    return None


def decode(data):
    """A ``.RIM``'s bytes -> a :class:`Texture`, or ``None`` if it has no S3TC image."""
    found = _find_image(data, 0, len(data))
    if found is None:
        return None
    body, stop = found
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
