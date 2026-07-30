"""``INDSOUND`` - a ``.rif``'s indexed sound table. Imports no ``bpy``.

An animation keyframe names a sound by a **number**, bits 24-30 of
``OBASEQFR.flags``, and this is what the number selects. The whole mechanism is
inside the file: ``BuildRifFileObject`` @ 0x005a9b50 zeroes a **128-entry
pointer array at rif+0x10** -- 0x200 bytes, matching the 0x7f mask exactly --
and installs every ``INDSOUND`` at ``rif->sounds[chunk->index]``. The chunk
**declares its own slot**; document order means nothing.
``MakeHierarchyFrame`` @ 0x005ae510 then reads the entry's path, ``strchr``es
past the backslash, and hands the basename to the sound system. Full trace in
``rif_chunk_format.md``.

The chunks are **direct children of the file root** -- all 240 shipped ones sit
at depth 1 under ``REBINFF2`` -- so unlike ``BMPNAMES`` there is no container to
find and no owner to record: they absorb onto the collection like any other
root-level leaf.

So a sound needs no per-role table and no engine-side name resolution: emit an
entry, point a keyframe at its index.

Two facts that shape the code:

- **A dangling index is normal shipped data.** 12 of the 52 files with sound
  events reference an index no ``INDSOUND`` declares -- ``vlowhark.RIF`` uses
  1, 2 and 3 and declares none at all -- and the engine's null-slot check makes
  that silent. So a missing entry is never an error here either.
- **The padding after a path is uninitialised** (0xcd, the debug-heap fill), so
  it is carried verbatim exactly as ``bmpnames`` carries the texture table's.
"""

import struct

#: Slots in the table. The frame's 7-bit field cannot address more.
TABLE_SLOTS = 128

#: Trailing int32s after the path, in order. Names from measuring all 240 shipped
#: chunks across 52 files; ``min_distance``/``max_distance`` are millimetres,
#: matching AvP's ``sequence_speed`` units and the engine's 3D attenuation.
TRAILING = ("min_distance", "max_distance", "volume", "pitch", "spare0", "spare1")

#: What the shipped entries overwhelmingly carry: 5 m / 40 m, and a mid volume.
DEFAULT_MIN_DISTANCE = 5000
DEFAULT_MAX_DISTANCE = 40000
DEFAULT_VOLUME = 127

#: `volume` runs 0..127 in every shipped entry, the same range as the frame's
#: sound-index field. Nothing proves it is exactly 7-bit, but nothing exceeds it.
VOLUME_MAX = 127


class SoundError(Exception):
    pass


def _padded_len(name):
    """The path plus its terminator, rounded up to 4 -- always at least one NUL."""
    return len(name) + (4 - len(name) % 4)


def decode(body):
    """One ``INDSOUND`` body -> an entry dict.

    Plain types only, so it can go straight into a Blender ID property.
    """
    if len(body) < 8:
        raise SoundError("body is %d bytes, too small for an index and a path" % len(body))
    index, = struct.unpack_from("<i", body, 0)
    end = body.find(b"\0", 4)
    if end < 0:
        raise SoundError("unterminated path")
    path = body[4:end].decode("latin-1")
    off = 4 + _padded_len(path)
    if off > len(body):
        raise SoundError("path padding runs past the body")

    entry = {"index": index, "path": path,
             "padding": list(body[4 + len(path):off])}
    rest = body[off:]
    if len(rest) % 4:
        raise SoundError("%d trailing bytes, not a whole number of int32" % len(rest))
    vals = struct.unpack("<%di" % (len(rest) // 4), rest) if rest else ()
    for i, name in enumerate(TRAILING):
        entry[name] = vals[i] if i < len(vals) else 0
    # Anything past the six known fields would be lost otherwise. No shipped
    # chunk has any -- all 240 carry exactly six -- but the file decides, not us.
    entry["extra"] = list(vals[len(TRAILING):])
    return entry


def encode(entry):
    """An entry dict -> the ``INDSOUND`` body, byte for byte.

    Reproduces :func:`decode`'s input including the uninitialised path padding.
    An entry with no recorded padding is NUL-padded, which is what a sound added
    in Blender gets.
    """
    path = entry["path"].encode("latin-1", "replace")
    out = bytearray(struct.pack("<i", int(entry.get("index", 0))))
    out += path
    pad = bytes(bytearray(entry.get("padding") or ()))
    want = _padded_len(path.decode("latin-1")) - len(path)
    out += pad if len(pad) == want else b"\0" * want
    vals = [int(entry.get(name, 0)) for name in TRAILING]
    vals += [int(v) for v in (entry.get("extra") or ())]
    out += struct.pack("<%di" % len(vals), *vals)
    return bytes(out)


def make_entry(index, path, min_distance=DEFAULT_MIN_DISTANCE,
               max_distance=DEFAULT_MAX_DISTANCE, volume=DEFAULT_VOLUME, pitch=0):
    """An entry for a sound the scene names but the file did not."""
    return {
        "index": int(index),
        "path": path,
        # Spelled out rather than left empty: an entry decoded from a file always
        # carries at least the terminator, and keeping every entry the same shape
        # is what lets the table live in one Blender ID property array.
        "padding": [0] * (4 - len(path) % 4),
        "min_distance": int(min_distance),
        "max_distance": int(max_distance),
        "volume": int(volume),
        "pitch": int(pitch),
        "spare0": 0,
        "spare1": 0,
        "extra": [],
    }


def basename(path):
    """What the engine actually looks up: the part after the last backslash.

    ``MakeHierarchyFrame`` does ``strchr(path, '\\\\')`` and takes the character
    after it, so a path with no separator is used whole.
    """
    cut = path.rfind("\\")
    return path[cut + 1:] if cut >= 0 else path


def folder(path):
    """The directory part, or ``""`` -- ``Robots`` in ``Robots\\GL_click08.wav``."""
    cut = path.rfind("\\")
    return path[:cut] if cut >= 0 else ""
