"""Chunk bodies as typed fields, so nothing has to be stored as opaque bytes.

Every leaf body in the shipped assets is a multiple of four bytes, which is what
makes a universal fallback possible: a chunk with no named schema still decodes to
a typed ``int32`` array rather than a blob. That array is a real Blender ID
property -- visible and editable in the N-panel -- so "everything is modelled"
holds even for the chunk types nobody has reverse engineered yet.

Named schemas are only added where the layout has been *measured* across all 563
files. Everything else stays generic on purpose: a wrong field name is worse than
an honest ``data`` array, and promoting a field later cannot break anything,
because ``tests/test_schema.py`` requires ``encode(decode(body)) == body`` for
every leaf chunk in the game either way.

Field kinds are ``i`` int32, ``I`` uint32 and ``f`` float32; a count of ``-1`` means
"however many fit in the rest of the body". There is deliberately no embedded-string
kind: the standalone name chunks turned out to be fixed-size buffers rather than
padded strings (see ``_decode_string_body``), so any mid-body string needs its own
measurement before it gets a field kind rather than a guessed padding rule.
"""

import struct

I32, U32, F32 = "i", "I", "f"

#: Chunk id -> [(name, kind, count)]. See the module docstring for what earns a
#: place here. Sizes are in elements, not bytes.
SCHEMA = {
    # --- measured in this repo ------------------------------------------------
    b"OBASEQFR": [
        ("rotation", F32, 4),      # unit quaternion (x, y, z, w) in 100% of 323,334
        ("position", I32, 3),      # rif units
        # AvP's at_frame_no: a position in the 0..OBASEQHD.num_frames span, 0 at
        # the first frame in all 28,577 monotonic sequences. **Authored, not
        # derived** - no formula reproduces the shipped values (see
        # rif_chunk_format.md), which is why the exporter anchors rather than
        # recomputes.
        ("time", I32, 1),
        # AvP's frame_ref_no, and this one *is* derived: it equals the frame's
        # position in the list in 323,334 of 323,334.
        ("frame_index", I32, 1),
        # Sound index in bits 24-30 (AvP's HierarchyFrame_SoundIndexMask), flag
        # mask in bits 0-23 (zero in 323,323 of 323,334), and bit 31 unrecovered.
        ("flags", I32, 1),
        ("num_extra_data", I32, 1),  # AvP's; zero in every shipped frame
    ],
    # Field for field AvP's `Light_Data` (win95/LTCHUNK.HPP): 21 int32 in exactly
    # this order, and Gunlok's chunk is exactly 84 bytes in all 3,794. That is
    # what names `spread`, `local_flags` and `pad` -- they were `field_0x38`,
    # `field_0x48` and `field_0x4c` while only their offsets were known.
    b"STDLIGHT": [
        ("light_id", I32, 1),      # unique within a file in all 38 that have lights
        ("position", I32, 3),      # rif units
        ("orientation", I32, 9),   # orthonormal 3x3, 16.16, row major, in 100% of 3,794
        ("brightness", I32, 1),    # 16.16, 0.2 .. 2.0
        ("spread", I32, 1),        # 67 distinct values, 1000 in 2,955 of 3,794
        ("range", I32, 1),         # rif units, 3,000 .. 357,300
        ("colour", U32, 1),        # 0x00RRGGBB
        ("flags", I32, 1),         # AvP's engine_light_flags; 3 or 7, nothing else
        ("local_flags", I32, 1),   # AvP's local_light_flags; always 1
        ("pad", I32, 2),           # AvP's pad1/pad2; always zero
    ],
    b"SHPCENTR": [
        ("centre", I32, 3),        # (min+max)/2, truncating toward zero
        ("radius", F32, 1),        # furthest vertex from the ORIGIN, not the centre
    ],
    # --- single scalars, unambiguous -----------------------------------------
    b"RIFVERIN": [("version", I32, 1)],
    b"AMBIENCE": [("ambience", I32, 1)],
    b"BMNAMVER": [("version", I32, 1)],
    # AvP's sequence flags (animobs.hpp): Loops 0x04, NoLoop 0x08,
    # NoInterpolation 0x10, HalfFrameRate 0x20, plus two Mummy-specific bits.
    # Gunlok ships exactly four values -- 0x4, 0x8, 0x84, 0x88 -- so it is
    # loop-or-not plus a 0x80 whose meaning is not recovered, and the two loop
    # bits are never set together.
    b"OBASEQFL": [("flags", I32, 1)],
    b"OBASEQTM": [("duration_ms", I32, 1)],  # AvP's sequence_time, milliseconds
    # AvP's Object_Animation_Sequence_Speed_Chunk: a movement speed in mm/second
    # and a heading in degrees, which `get_sequence_vector` turns into a unit
    # direction. Gunlok leaves both `angle` and `spare` zero in all 582 shipped
    # chunks and uses speeds of 1400..3000 mm/s -- walking and running pace.
    # (This was one field `("speed", I32, 3)` until the layout was read off AvP;
    # it round-tripped either way, but named all three after the first.)
    b"OBASEQSP": [("sequence_speed", I32, 1), ("angle", I32, 1), ("spare", I32, 1)],
    # 12 bytes, but **1 float + 2 int32**, not 3 floats: the loader reads
    # `*(float *)(chunk + 0x28)` and computes `2 * tan(radians(fov) / 2)`,
    # defaulting to 90 degrees when the chunk is absent. The other two have no
    # consumer. (This was `("fov", F32, 3)`, which round-tripped either way.)
    b"CUTTRFOV": [("fov_degrees", F32, 1), ("fields", I32, 2)],
    b"ENVSDSCL": [("sound_scale", F32, 2)],
    # Six int32, and **inert**: nothing in gl.exe ever looks this chunk up, so
    # there is no consumer to read a meaning off. Widths only.
    b"CTUSNDPR": [("sound_properties", I32, 6)],
    b"HIERBBOX": [("bounds_min", I32, 3), ("bounds_max", I32, 3)],
}

#: Chunk ids whose body is one NUL-terminated string padded to a 4-byte boundary.
#:
#: ``CUTTRNAM`` and ``CTUSRHIE`` used to be here and are **not** bare strings --
#: each carries trailing int32 that this treated as `padding`. They have codecs
#: below.
STRING_CHUNKS = frozenset(
    (b"RIFFNAME", b"OBHIERNM", b"SOUNDDIR",
     b"SHPEXTFN", b"SHPFNAME", b"SHPFRGTP", b"TRSNDCAT", b"EXTOBJNM")
)

#: The name of the fallback property for a chunk with no named schema.
GENERIC_FIELD = "data"

_SIZE = {I32: 4, U32: 4, F32: 4}


class SchemaError(Exception):
    pass


# --------------------------------------------------------------------------
# Bodies that are not a fixed field list
# --------------------------------------------------------------------------
#
# The cutscene chunks are variable-length -- a padded string, a counted array, a
# tagged record stream -- so :data:`SCHEMA` cannot describe them and they fell to
# the untyped ``int32[]`` fallback. Each one below is a hand-written
# decode/encode pair instead, registered in :data:`CODECS`.
#
# **Everything they produce has to survive Blender's ID properties**, which is
# what rules the representation. ``scene._unpack_absorbed`` converts each stored
# value with a single ``list(val)``, so a value may be a scalar, a string, or a
# **flat homogeneous list** -- never a nested list and never a list mixing ints
# with floats. That is why a record stream comes back as parallel arrays
# (``kinds`` / ``headers`` / ``payload`` / ``payload_counts``) rather than as the
# list of dicts the format itself suggests.
#
# Layouts and the evidence for them: rif_chunk_format.md, "The cutscene chunks".


def pad4(name):
    """Bytes a name field occupies: ``(strlen + 4) & ~3``.

    Note this is **not** "round the string up to 4" -- it always leaves at least
    one NUL, so a 3-character name takes 4 bytes and a 4-character name takes 8.
    Measured across all 1,856 shipped cutscene chunks.
    """
    return (len(name) + 4) & ~3


def _read_name(body, off):
    """``(text, offset past the padded field)``."""
    end = body.find(b"\0", off)
    if end < 0:
        raise SchemaError("unterminated name at %d" % off)
    raw = body[off:end]
    return raw.decode("latin-1"), off + pad4(raw)


def _write_name(text):
    raw = text.encode("latin-1")
    return raw + b"\0" * (pad4(raw) - len(raw))


def _ints(body, off, n):
    return list(struct.unpack_from("<%di" % n, body, off))


def _name_and_ints(n_ints):
    """A padded string followed by exactly ``n_ints`` int32."""

    def dec(body):
        name, off = _read_name(body, 0)
        return {"name": name, "fields": _ints(body, off, n_ints)}

    def enc(props):
        return _write_name(props["name"]) + struct.pack(
            "<%di" % n_ints, *props["fields"])

    return dec, enc


# --- CUTSCDAT: which cutscene this is -------------------------------------

def _dec_cutscdat(body):
    name, off = _read_name(body, 12)
    return {
        # Never read by gl.exe; shaped like a rif-unit position.
        "position": _ints(body, 0, 3),
        "name": name,
        "reserved": _ints(body, off, 2),         # zero in all 34 shipped
        # MD5("Cutscene:" + name)[0:8], little-endian, top byte of the second
        # dword cleared -- reproduced in 34 of 34. See `cutscene_name_hash`.
        "name_hash": _ints(body, off + 8, 2),
    }


def _enc_cutscdat(props):
    return (struct.pack("<3i", *props["position"])
            + _write_name(props["name"])
            + struct.pack("<2i", *props["reserved"])
            + struct.pack("<2i", *props["name_hash"]))


def cutscene_name_hash(name):
    """The `name_hash` pair a `CUTSCDAT` must carry for ``name``.

    `Cutscene_Data_Chunk` @ 0x005d7e60 formats ``"Cutscene:%s"`` and MD5s it.
    An authored cutscene needs this; it is not a value that can be invented.
    """
    import hashlib

    digest = hashlib.md5(b"Cutscene:" + name.encode("latin-1")).digest()
    lo, hi = struct.unpack("<2i", digest[:8])
    return [lo, hi & 0xffffff]


# --- CTUSRDAT: a participant ----------------------------------------------
#
# The string is the participant's own `.rif` file name (`Elint MkII.rif`). Of the
# twelve trailing int32 the loader reads four; the rest keep the repo's
# `field_N` convention rather than being guessed at.

def _dec_ctusrdat(body):
    name, off = _read_name(body, 0)
    d = _ints(body, off, 12)
    return {
        "name": name,
        "field_0": d[0],
        "anim_id": d[1],          # -1 = none
        "user_id": d[2],          # what a CUTEVENT record targets
        "field_3": d[3],
        "is_camera": d[4],        # 0 marks the camera-position track
        "flags": d[5],            # bit 0 marks the camera look-at track
        "fields_6_11": d[6:],
    }


def _enc_ctusrdat(props):
    d = [props["field_0"], props["anim_id"], props["user_id"], props["field_3"],
         props["is_camera"], props["flags"]] + list(props["fields_6_11"])
    return _write_name(props["name"]) + struct.pack("<12i", *d)


# --- CUTPOINT: the path ---------------------------------------------------
#
# body = uint32 count + count * 16 + a 48-byte trailer, i.e. `16*count + 52`
# (`GetDataBlockSize` @ 0x005d92a0 returns `(count + 4) * 16`). The record is
# three int32 rif units and a packed dword whose **low 24 bits are the interval's
# duration in milliseconds**; the top 8 bits are masked off by the consumer but
# are non-zero in 125 of 763 shipped points, so they are carried, not cleared.
#
# The path is a Catmull-Rom spline and the loader synthesises the two phantom end
# control points itself -- these are control points, not baked keyframes.

POINT_STRIDE = 16


def _dec_cutpoint(body):
    count = struct.unpack_from("<I", body, 0)[0]
    end = 4 + count * POINT_STRIDE
    if end + 48 != len(body):
        raise SchemaError("CUTPOINT: %d points needs %d bytes, body is %d"
                          % (count, end + 48, len(body)))
    return {
        # Flat x, y, z, packed_time per point -- flat because a list of 4-tuples
        # is not a storable ID property.
        "points": _ints(body, 4, count * 4),
        "start_quat": list(struct.unpack_from("<4f", body, end)),
        "end_quat": list(struct.unpack_from("<4f", body, end + 16)),
        "has_start_quat": struct.unpack_from("<i", body, end + 32)[0],
        "has_end_quat": struct.unpack_from("<i", body, end + 36)[0],
        "unread": _ints(body, end + 40, 2),   # never read by gl.exe
    }


def _enc_cutpoint(props):
    pts = list(props["points"])
    if len(pts) % 4:
        raise SchemaError("CUTPOINT: `points` must be 4 values per point, got %d"
                          % len(pts))
    return (struct.pack("<I", len(pts) // 4)
            + struct.pack("<%di" % len(pts), *pts)
            + struct.pack("<4f", *props["start_quat"])
            + struct.pack("<4f", *props["end_quat"])
            + struct.pack("<2i", props["has_start_quat"], props["has_end_quat"])
            + struct.pack("<2i", *props["unread"]))


def point_time_ms(packed):
    """The duration of the interval starting at this point, in milliseconds."""
    return packed & 0xffffff


def pack_point_time(ms, spare=0):
    """Inverse of :func:`point_time_ms`; `spare` is the ignored top byte."""
    packed = (ms & 0xffffff) | ((spare & 0xff) << 24)
    return packed - (1 << 32) if packed >= (1 << 31) else packed


# --- CUTEVENT: the event list ---------------------------------------------
#
# body = 12 + pad4(name) + uint32 record_count + that many variable-length
# records. Every record opens with 24 bytes -- `kind`, `record_size` (including
# the header, which is how the engine skips a kind it does not know), and four
# more int32 -- and kinds 5, 11 and 13 then carry a padded string before their
# payload dwords.

EVENT_HEADER = 24

#: Record kinds whose payload begins with a padded string. Any other kind's
#: payload is read as dwords, so an unknown kind still round-trips.
EVENT_STRING_KINDS = frozenset((5, 11, 13))

#: Separator for the per-record strings, which are stored as one joined string
#: because a list of strings is not a storable ID property.
_EVENT_SEP = "\n"


def _dec_cutevent(body):
    name, off = _read_name(body, 12)
    count = struct.unpack_from("<I", body, off)[0]
    off += 4

    kinds, headers, payload, counts, strings = [], [], [], [], []
    for _ in range(count):
        if off + EVENT_HEADER > len(body):
            raise SchemaError("CUTEVENT: record header runs past the body")
        kind, size = struct.unpack_from("<2i", body, off)
        if size < EVENT_HEADER or off + size > len(body):
            raise SchemaError("CUTEVENT: record size %d at %d is out of range"
                              % (size, off))
        kinds.append(kind)
        headers.extend(_ints(body, off + 8, 4))

        inner = off + EVENT_HEADER
        if kind in EVENT_STRING_KINDS:
            text, inner = _read_name(body, inner)
            if _EVENT_SEP in text:
                raise SchemaError("CUTEVENT: a record string contains %r"
                                  % _EVENT_SEP)
            strings.append(text)
        rest = off + size - inner
        if rest % 4:
            raise SchemaError("CUTEVENT: kind %d leaves %d trailing bytes"
                              % (kind, rest))
        counts.append(rest // 4)
        payload.extend(_ints(body, inner, rest // 4))
        off += size

    if off != len(body):
        raise SchemaError("CUTEVENT: %d records consumed %d of %d bytes"
                          % (count, off, len(body)))
    return {
        # The group's position, in authored CUTPOINT *index* space: the integer
        # part is the point index and the fraction is the position within that
        # interval.
        "position": struct.unpack_from("<f", body, 0)[0],
        "fields": _ints(body, 4, 2),            # zero in all 300 shipped
        "name": name,
        "kinds": kinds,
        "headers": headers,                     # 4 per record
        "payload": payload,
        "payload_counts": counts,               # dwords per record
        "strings": _EVENT_SEP.join(strings),
    }


def _enc_cutevent(props):
    kinds = list(props["kinds"])
    headers = list(props["headers"])
    payload = list(props["payload"])
    counts = list(props["payload_counts"])
    n_strings = sum(1 for k in kinds if k in EVENT_STRING_KINDS)
    strings = props["strings"].split(_EVENT_SEP) if n_strings else []
    if len(strings) != n_strings:
        raise SchemaError("CUTEVENT: %d string-carrying records but %d strings"
                          % (n_strings, len(strings)))
    if len(headers) != 4 * len(kinds) or len(counts) != len(kinds):
        raise SchemaError("CUTEVENT: parallel arrays disagree on record count")

    out = bytearray()
    at_word = 0
    at_str = 0
    for i, kind in enumerate(kinds):
        inner = b""
        if kind in EVENT_STRING_KINDS:
            inner = _write_name(strings[at_str])
            at_str += 1
        words = payload[at_word:at_word + counts[i]]
        if len(words) != counts[i]:
            raise SchemaError("CUTEVENT: `payload` is short for record %d" % i)
        at_word += counts[i]
        size = EVENT_HEADER + len(inner) + 4 * counts[i]
        out += struct.pack("<2i", kind, size)
        out += struct.pack("<4i", *headers[4 * i:4 * i + 4])
        out += inner
        out += struct.pack("<%di" % counts[i], *words)
    if at_word != len(payload):
        raise SchemaError("CUTEVENT: `payload` has %d unused values"
                          % (len(payload) - at_word))

    return (struct.pack("<f", props["position"])
            + struct.pack("<2i", *props["fields"])
            + _write_name(props["name"])
            + struct.pack("<I", len(kinds))
            + bytes(out))


# --- DUMOBJTX: an ambient sound emitter -----------------------------------
#
# A NUL-terminated, CRLF-separated directive padded to `(strlen + 4) & ~3` --
# `pad4` exactly, verified as the padding length of all 1,097 shipped chunks,
# every one of them NUL-filled (unlike `INDSOUND`'s path padding, which is the
# debug heap's 0xcd). So the body is the text and nothing else, and the padding
# is derived rather than carried.
#
# The text itself is *not* decomposed here. It is free-form -- 362 chunks have
# no third line, 221 end in a trailing CRLF, 29 in two, and one splits its
# directives across two lines -- so the grammar lives in :mod:`emitters` and
# operates on the string, while this stays the byte layer. `size` is kept for the
# same reason `_decode_string_body` keeps it: a body has to rebuild exactly even
# if a future file pads differently.


def _dec_dumobjtx(body):
    end = body.find(b"\0")
    if end < 0:
        raise SchemaError("DUMOBJTX: unterminated text")
    out = {"text": body[:end].decode("latin-1"), "size": len(body)}
    tail = body[end:]
    if any(tail):  # no shipped chunk has one; a hand-made file could
        out["padding"] = list(tail)
    return out


def _enc_dumobjtx(props):
    raw = props.get("text", "").encode("latin-1")
    padding = props.get("padding")
    if padding:
        return raw + bytes(padding)
    return raw.ljust(max(int(props.get("size", pad4(raw))), len(raw) + 1), b"\0")


def dumobjtx_body(text):
    """The ``DUMOBJTX`` body for a text authored from nothing."""
    return _enc_dumobjtx({"text": text})


#: Chunk id -> (decode, encode). Consulted before :data:`STRING_CHUNKS` and
#: :data:`SCHEMA`.
CODECS = {
    b"DUMOBJTX": (_dec_dumobjtx, _enc_dumobjtx),
    b"CUTSCDAT": (_dec_cutscdat, _enc_cutscdat),
    b"CTUSRDAT": (_dec_ctusrdat, _enc_ctusrdat),
    b"CTUSRHIE": _name_and_ints(3),
    b"CUTTRNAM": _name_and_ints(2),
    b"CUTPOINT": (_dec_cutpoint, _enc_cutpoint),
    b"CUTEVENT": (_dec_cutevent, _enc_cutevent),
}


def _decode_string_body(body):
    """A name chunk is a fixed-size buffer, not a tight string.

    ``CUTTRNAM`` is 16 bytes, ``CTUSRHIE`` 24, ``RIFFNAME`` varies -- so the buffer
    size has to be carried or the body cannot be rebuilt. It is almost always
    NUL-filled after the terminator, but not always (``SOUNDDIR`` ships
    ``b'Robots\\0\\xff'``), so a non-zero tail is kept verbatim.
    """
    end = body.find(b"\0")
    if end < 0:  # unterminated; keep every byte
        return {"name": body.decode("latin-1"), "size": len(body)}
    out = {"name": body[:end].decode("latin-1"), "size": len(body)}
    tail = body[end + 1:]
    if any(tail):
        out["padding"] = list(tail)
    return out


def _encode_string_body(props):
    raw = bytearray(props.get("name", "").encode("latin-1"))
    size = props.get("size", len(raw) + 1)
    if len(raw) < size:
        raw += b"\0"
    padding = props.get("padding")
    if padding:
        raw += bytes(padding)
    else:
        raw += b"\0" * (size - len(raw))
    return bytes(raw)


def decode(chunk_id, body):
    """Body bytes -> {name: value}. Values are ints, floats, strings or lists."""
    codec = CODECS.get(chunk_id)
    if codec is not None:
        return codec[0](body)

    if chunk_id in STRING_CHUNKS:
        return _decode_string_body(body)

    fields = SCHEMA.get(chunk_id)
    if fields is None:
        if len(body) % 4:
            # No shipped chunk hits this, but a hand-made file could.
            return {GENERIC_FIELD: list(body), "_raw_bytes": True}
        n = len(body) // 4
        return {GENERIC_FIELD: list(struct.unpack_from("<%di" % n, body, 0))}

    out = {}
    off = 0
    for name, kind, count in fields:
        width = _SIZE[kind]
        n = count if count >= 0 else (len(body) - off) // width
        if off + n * width > len(body):
            raise SchemaError("%r: field %s runs past the body" % (chunk_id, name))
        vals = struct.unpack_from("<%d%s" % (n, kind), body, off)
        out[name] = list(vals) if count != 1 else vals[0]
        off += n * width

    # Anything the schema does not describe stays addressable rather than lost.
    if off < len(body):
        rest = body[off:]
        if len(rest) % 4 == 0:
            out[GENERIC_FIELD] = list(struct.unpack_from("<%di" % (len(rest) // 4), rest, 0))
        else:
            out[GENERIC_FIELD] = list(rest)
            out["_raw_bytes"] = True
    return out


def encode(chunk_id, props):
    """{name: value} -> body bytes. Must reproduce what decode() was given."""
    codec = CODECS.get(chunk_id)
    if codec is not None:
        return codec[1](props)

    if chunk_id in STRING_CHUNKS:
        return _encode_string_body(props)

    fields = SCHEMA.get(chunk_id)
    out = bytearray()
    if fields is not None:
        for name, kind, count in fields:
            val = props.get(name)
            seq = [val] if count == 1 else list(val or ())
            out += struct.pack("<%d%s" % (len(seq), kind), *seq)

    rest = props.get(GENERIC_FIELD)
    if rest:
        if props.get("_raw_bytes"):
            out += bytes(rest)
        else:
            out += struct.pack("<%di" % len(rest), *rest)
    return bytes(out)
