"""The file's texture table: the ``BMPNAMES`` chunk. Imports no ``bpy``.

A polygon names its texture with an *index*, not a path -- ``colour & 0xfff``
(see :mod:`shapes`) -- and the index is resolved against one table per file, the
``BMPNAMES`` chunk hanging off the file-level ``REBENVDT``. 527 of the 563
shipped files carry one; the other 36 have no textured geometry to name.

The layout is AvP's ``Chunk_With_BMPs`` (``3dc/win95/BMPNAMES.CPP``) verbatim: a
count word, then a 20-byte record and a padded name per entry.

===== ===== ================================================================
Off   Size  Field
===== ===== ================================================================
0x00  4     ``count`` in the low 16 bits, table ``version`` in the high 16
0x04  20    per entry: ``flags``, ``index``, ``data1``, ``priority``,
            ``transparency``
\\.     ...   the entry's name, NUL-terminated and padded so that the bytes
            spent on it are ``strlen + (4 - strlen % 4)``
===== ===== ================================================================

Four things are measured across all 1,601 entries in the 527 shipped tables
rather than taken from the AvP source:

- **The index is a stable id, not a position.** Entries are stored in
  *descending* index order and the values are sparse -- ``Maze.RIF`` holds
  ``[10, 9, 8, 5, 4, 1]`` for six entries. Matching a polygon's texture index
  against ``index`` resolves 1,518,963 of the 1,766,071 shipped polygons; of the
  rest, 22,331 use the ``0xfff`` untextured sentinel and 215,517 of the
  remaining 224,747 are in ``_shadow`` files, whose polygons carry junk texture
  and UV indices because the meshes are never textured. **No table has a
  duplicate index or a duplicate name.**
- **The padding after a name is not always zero.** The writer pads out of
  uninitialised memory (``Units\\baddies3.RIM`` is followed by ``00 f5``), so the
  bytes are carried verbatim or the chunk cannot be rebuilt.
- **``flags`` is ``0x0100010c`` in every shipped entry.** That is
  ``ChunkBMPFlag_IFF`` (the name is a path relative to the textures root, and
  everything about the image lives in the file itself), plus
  ``PriorityAndTransparencyAreValid`` and both mip-map requests.
- **With the IFF flag set, ``transparency`` holds the image size** -- AvP's
  ``transparency_colour_union``, width in the low 16 bits and height in the high
  16. It agrees with the ``.RIM``'s own header in 1,580 of the 1,597 entries
  whose file is on disk, so it is authoring output that can go stale, not a
  second source of truth. It is carried, never trusted.
"""

import struct

#: The name is a path relative to the textures root, and the image's own header
#: is authoritative. Set on every entry in the shipped assets.
FLAG_IFF = 0x01000000
FLAG_PRIORITY_VALID = 0x00000100
FLAG_GAME_MIPMAPS = 0x00000004
FLAG_TOOLS_MIPMAPS = 0x00000008
FLAG_TRANSPARENCY = 0x00000002

#: What every one of the 1,601 shipped entries carries.
DEFAULT_FLAGS = FLAG_IFF | FLAG_PRIORITY_VALID | FLAG_GAME_MIPMAPS | FLAG_TOOLS_MIPMAPS

#: AvP's ``DEFAULT_BMPN_PRIORITY``, and the value in every shipped entry.
DEFAULT_PRIORITY = 6

ENTRY_STRIDE = 20


class BmpNamesError(Exception):
    pass


def _padded_len(name):
    """AvP's rule: always at least one NUL, always up to a 4-byte boundary."""
    return len(name) + (4 - len(name) % 4)


def decode(body):
    """``BMPNAMES`` body -> ``(version, [entry])``.

    An entry is a plain dict so it can go straight into a Blender ID property.
    ``padding`` is the bytes after the name's terminator, as a list of ints.
    """
    if len(body) < 4:
        raise BmpNamesError("body is %d bytes, too small for the count" % len(body))
    head, = struct.unpack_from("<I", body, 0)
    count, version = head & 0xFFFF, head >> 16

    entries = []
    off = 4
    for i in range(count):
        if off + ENTRY_STRIDE > len(body):
            raise BmpNamesError("entry %d runs past the body" % i)
        flags, index, data1, priority, transparency = struct.unpack_from("<5i", body, off)
        off += ENTRY_STRIDE
        end = body.find(b"\0", off)
        if end < 0:
            raise BmpNamesError("entry %d has an unterminated name" % i)
        name = body[off:end].decode("latin-1")
        padded = _padded_len(name)
        entries.append({
            "name": name,
            "flags": flags & 0xFFFFFFFF,
            "index": index,
            "data1": data1,
            "priority": priority,
            "transparency": transparency & 0xFFFFFFFF,
            "padding": list(body[off + len(name):off + padded]),
        })
        off += padded
    if off != len(body):
        raise BmpNamesError("%d bytes left over after %d entries" % (len(body) - off, count))
    return version, entries


def encode(version, entries):
    """``(version, [entry])`` -> the ``BMPNAMES`` body.

    Reproduces :func:`decode`'s input byte for byte, padding junk included. An
    entry with no recorded ``padding`` is padded with NULs, which is what a
    newly added texture gets.
    """
    out = bytearray(struct.pack("<I", (len(entries) & 0xFFFF) | ((version & 0xFFFF) << 16)))
    for e in entries:
        name = e["name"].encode("latin-1")
        out += struct.pack(
            "<5i",
            _signed(e.get("flags", DEFAULT_FLAGS)),
            int(e.get("index", 0)),
            int(e.get("data1", 0)),
            int(e.get("priority", DEFAULT_PRIORITY)),
            _signed(e.get("transparency", 0)),
        )
        out += name
        pad = bytes(e.get("padding") or ())
        want = _padded_len(name.decode("latin-1")) - len(name)
        out += pad if len(pad) == want else b"\0" * want
    return bytes(out)


def _signed(v):
    v = int(v)
    return v - 0x100000000 if v >= 0x80000000 else v


def size(entry):
    """``(width, height)`` the table claims for this entry, or ``None``.

    Only meaningful with :data:`FLAG_IFF` set; the field is the transparency
    colour otherwise. Stale in 17 of the 1,597 shipped entries that resolve to a
    file, so it is a hint for a table that has to be written without opening the
    image, never a substitute for the image's own header.
    """
    if not entry.get("flags", 0) & FLAG_IFF:
        return None
    t = int(entry.get("transparency", 0)) & 0xFFFFFFFF
    return t & 0xFFFF, (t >> 16) & 0xFFFF


def make_entry(name, index, width=0, height=0):
    """A table entry for a texture the scene names but the file did not."""
    return {
        "name": name,
        "flags": DEFAULT_FLAGS,
        "index": index,
        # AvP packs a bitmap version and the table version (shifted by 8) here;
        # both are editor bookkeeping nothing in the game reads.
        "data1": 0,
        "priority": DEFAULT_PRIORITY,
        "transparency": (width & 0xFFFF) | ((height & 0xFFFF) << 16),
        # NUL padding, spelled out rather than left empty: an entry decoded from
        # a file always carries at least the terminator here, and keeping every
        # entry the same shape is what lets the table live in one Blender ID
        # property array.
        "padding": [0] * (4 - len(name) % 4),
    }
