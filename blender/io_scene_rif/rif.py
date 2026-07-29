"""RIF container format: REBCRIF1 decompression, chunk tree, serialization.

This module deliberately imports nothing from ``bpy``. Nothing in GkPlus's
``src/`` can be exercised outside Gunlok, but this layer can -- and the whole
point of splitting it out is that ``tests/test_roundtrip.py`` runs it over every
shipped ``.rif`` without Blender in the picture.

The format is Rebellion's ``3dc`` chunk library, shared verbatim with the
published Aliens vs Predator (1999) source; see ``rif_chunk_format.md``. Every
chunk, container or leaf, is a 12-byte header (8-char id + total size including
the header) followed by its body. A container's body is nothing but its
concatenated children.

Two facts about the shipped assets shape this code:

- **413 of the 563 files are Huffman-compressed, 150 are not**, and the game
  loads both. So writing is a plain chunk serialize -- there is no need to
  reimplement the compressor to produce a file Gunlok will read.
- **Container bodies are exactly packed**: every one of the 563 files parses
  with no slack bytes anywhere, and re-serializing reproduces the input byte for
  byte. That is what makes the pass-through writer below trustworthy.
"""

import struct

MAX_DEPTH = 11

# `bits & ~EDXMASK` in the original; the decode table is indexed in 16-bit units
# and the index is always even, so it lands on 32-bit entry boundaries.
_TABLE_INDEX_MASK = ((1 << (MAX_DEPTH + 1)) - 1) ^ 1  # 0xffe

COMPRESSED_MAGIC = b"REBCRIF1"
ROOT_MAGIC = b"REBINFF2"

#: Chunk ids whose body is nothing but child chunks. Everything else is treated
#: as an opaque leaf, which is what makes unknown chunks survive a round-trip.
CONTAINERS = frozenset(
    (
        b"REBINFF2", b"RBOBJECT", b"REBSHAPE", b"SUBSHAPE", b"MODULEDT",
        b"OBJPRJDT", b"ANIMSEQU", b"ANIMFRAM", b"ASALTTEX", b"SHPEXTFL",
        b"SHPMORPH", b"SHPFRAGS", b"REBENVDT", b"SPECLOBJ", b"OBJCHIER",
        b"OBANSEQS", b"OBANSEQC", b"SOUNDEXD", b"LIGHTSET", b"DUMMYOBJ",
        b"CUTSHEAD", b"CUTSCUSR", b"CUTTRACK", b"SUBRIFFL",
        # AvP's Object_Interface_Data_Chunk, and a container there too. Its id is
        # seven characters NUL-padded, and its body is a single OBJNOTES holding
        # the editor's note text -- all 9,313 of them in the shipped assets.
        b"OBINTDT\0",
    )
)


class RifError(Exception):
    """Raised when a buffer cannot be read as a RIF chunk tree."""


# --------------------------------------------------------------------------
# Huffman
# --------------------------------------------------------------------------

def _make_decode_table(codelength_count, byte_assignment):
    """Port of ``MakeHuffmanDecodeTable`` (AvP ``3dc/win95/huffman.cpp``).

    Each table entry packs the code width in its low byte and the decoded symbol
    in the next, which is how the decode loop reads it back out.
    """
    table = [0] * (1 << MAX_DEPTH)
    lenbits = 0
    repcount = 1 << MAX_DEPTH
    repspace = 1
    depthbit = 4
    offset = 0
    sym = 255  # walks the assignment table downwards
    depth_i = 0
    while True:
        while True:
            lenbits += 1
            depthbit <<= 1
            repspace <<= 1
            repcount >>= 1
            if depth_i >= len(codelength_count):
                raise RifError("huffman code lengths exhausted")
            this_depth = codelength_count[depth_i]
            depth_i += 1
            if this_depth:
                break
        while True:
            if sym < 0:
                entry = 0xFF
            else:
                entry = lenbits | (byte_assignment[sym] << 8)
                sym -= 1
            out = offset >> 2
            for _ in range(repcount):
                table[out] = entry
                out += repspace
            step = depthbit
            while True:
                step >>= 1
                if step & 3:
                    return table
                offset ^= step
                if offset & step:
                    break
            this_depth -= 1
            if not this_depth:
                break


def _decode(table, src, length):
    """Port of ``HuffmanDecode``.

    The original is a hand-tuned bit pump written against x86 semantics, and two
    details do not survive a naive transcription:

    - **Shift counts are masked to 5 bits on x86**, so ``x << 32`` is ``x << 0``,
      not zero. Python shifts by the full count, which silently corrupts the bit
      window whenever ``fill`` or ``reserve`` reaches 32.
    - **It reads up to one word past the end of the compressed block** while
      draining the last few symbols. Benign in C; here the input is padded
      instead. Without this, the 64 largest files stop exactly 4 bytes short.
    """
    out = bytearray()
    words = struct.unpack_from("<%dI" % (len(src) // 4), src, 0) + (0, 0, 0, 0)
    available = 0
    reserve = 0
    width = 0
    bits = 0
    resbits = 0
    word_i = 0
    while True:
        available += width
        fill = 31 - available
        bits = (bits << fill) & 0xFFFFFFFF
        if fill > reserve:
            fill -= reserve
            available += reserve
            if reserve:
                bits = (bits >> (reserve & 31)) | (
                    (resbits << ((32 - reserve) & 31)) & 0xFFFFFFFF
                )
            resbits = words[word_i]
            word_i += 1
            reserve = 32
        bits = (bits >> (fill & 31)) | ((resbits << ((32 - fill) & 31)) & 0xFFFFFFFF)
        resbits >>= fill & 31
        reserve -= fill
        available = 31
        while True:
            entry = table[(bits & _TABLE_INDEX_MASK) >> 1]
            width = entry & 0xFF
            available -= width
            if available < 0 or len(out) == length:
                break
            bits >>= width
            out.append((entry >> 8) & 0xFF)
        if not (available > -32 and len(out) != length):
            break
    return bytes(out)


def decompress(data):
    """Return the plain chunk stream for ``data``, which may or may not be packed."""
    if data[:8] != COMPRESSED_MAGIC:
        return data
    if len(data) < 0x13C:
        raise RifError("truncated REBCRIF1 header")
    _compressed_size, uncompressed_size = struct.unpack_from("<ii", data, 8)
    counts = list(struct.unpack_from("<%di" % MAX_DEPTH, data, 0x10))
    assignment = data[0x3C:0x13C]
    table = _make_decode_table(counts, assignment)
    out = _decode(table, data[0x13C:], uncompressed_size)
    if len(out) != uncompressed_size:
        raise RifError(
            "decompressed %d bytes, header declares %d" % (len(out), uncompressed_size)
        )
    return out


# --------------------------------------------------------------------------
# Chunk tree
# --------------------------------------------------------------------------

class Chunk:
    """One RIF chunk.

    ``children`` is ``None`` for a leaf (and for a container whose body did not
    parse, which is kept opaque rather than rejected). When it is a list, ``body``
    is unused and the wire form is rebuilt from the children.
    """

    __slots__ = ("id", "body", "children")

    def __init__(self, cid, body=b"", children=None):
        self.id = cid
        self.body = body
        self.children = children

    @property
    def name(self):
        return self.id.decode("ascii", "replace")

    def size(self):
        if self.children is None:
            return 12 + len(self.body)
        return 12 + sum(c.size() for c in self.children)

    def find(self, cid):
        """First direct child with this id, or None."""
        for c in self.children or ():
            if c.id == cid:
                return c
        return None

    def find_all(self, cid):
        return [c for c in self.children or () if c.id == cid]

    def walk(self):
        """Yield this chunk and every descendant, depth first."""
        yield self
        for c in self.children or ():
            yield from c.walk()

    def __repr__(self):
        if self.children is None:
            return "<Chunk %s %d bytes>" % (self.name, len(self.body))
        return "<Chunk %s %d children>" % (self.name, len(self.children))


def _parse_span(buf, off, end):
    out = []
    while off < end:
        if end - off < 12:
            raise RifError("%d trailing bytes at %d, too small for a header" % (end - off, off))
        cid = bytes(buf[off:off + 8])
        size, = struct.unpack_from("<I", buf, off + 8)
        if size < 12 or off + size > end:
            raise RifError("chunk %r at %d declares size %d, %d available"
                           % (cid, off, size, end - off))
        children = None
        if cid in CONTAINERS:
            try:
                children = _parse_span(buf, off + 12, off + size)
            except RifError:
                # Keep it opaque instead of failing the whole file: an
                # unparseable container still round-trips byte for byte.
                children = None
        out.append(Chunk(cid, b"" if children is not None else bytes(buf[off + 12:off + size]),
                         children))
        off += size
    return out


def parse(data):
    """Parse a plain (already decompressed) chunk stream into its root chunk."""
    roots = _parse_span(data, 0, len(data))
    if len(roots) != 1:
        raise RifError("expected exactly one root chunk, found %d" % len(roots))
    return roots[0]


def serialize(chunk):
    """Render a chunk tree back to the wire form."""
    out = bytearray()
    _emit(chunk, out)
    return bytes(out)


def _emit(chunk, out):
    if len(chunk.id) != 8:
        raise RifError("chunk id %r is not 8 bytes" % (chunk.id,))
    header = len(out)
    out += chunk.id
    out += b"\0\0\0\0"  # size patched below, once the children are laid down
    if chunk.children is None:
        out += chunk.body
    else:
        for c in chunk.children:
            _emit(c, out)
    struct.pack_into("<I", out, header + 8, len(out) - header)


def load(path):
    """Read ``path`` and return its root chunk."""
    with open(path, "rb") as fh:
        return parse(decompress(fh.read()))


def save(path, root):
    """Write ``root`` to ``path`` as an uncompressed RIF.

    Gunlok reads uncompressed files -- 150 of the 563 it ships are stored that
    way -- so there is no compression step here.
    """
    with open(path, "wb") as fh:
        fh.write(serialize(root))
