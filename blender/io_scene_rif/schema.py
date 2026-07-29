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
        ("time", I32, 1),          # normalized 16.16, 0 at the first frame
        ("frame_index", I32, 1),   # 0-based within the sequence
        ("flags", I32, 1),         # only the top byte is ever set
        ("field_0x28", I32, 1),    # always zero
    ],
    b"STDLIGHT": [
        ("light_id", I32, 1),
        ("position", I32, 3),      # rif units
        ("orientation", I32, 9),   # orthonormal 3x3, 16.16, row major, in 100% of 3,794
        ("brightness", I32, 1),    # 16.16, 0.2 .. 2.0
        ("field_0x38", I32, 1),
        ("range", I32, 1),         # rif units
        ("colour", U32, 1),        # 0x00RRGGBB
        ("flags", I32, 1),         # 3 or 7
        ("field_0x48", I32, 1),    # always 1
        ("field_0x4c", I32, 2),    # always zero
    ],
    b"SHPCENTR": [
        ("centre", I32, 3),        # (min+max)/2, truncating toward zero
        ("radius", F32, 1),        # furthest vertex from the ORIGIN, not the centre
    ],
    # --- single scalars, unambiguous -----------------------------------------
    b"RIFVERIN": [("version", I32, 1)],
    b"AMBIENCE": [("ambience", I32, 1)],
    b"BMNAMVER": [("version", I32, 1)],
    b"OBASEQFL": [("flags", I32, 1)],
    b"OBASEQTM": [("duration_ms", I32, 1)],
    b"OBASEQSP": [("speed", I32, 3)],
    b"CUTTRFOV": [("fov", F32, 3)],
    b"ENVSDSCL": [("sound_scale", F32, 2)],
    b"CTUSNDPR": [("sound_properties", I32, 6)],
    b"HIERBBOX": [("bounds_min", I32, 3), ("bounds_max", I32, 3)],
}

#: Chunk ids whose body is one NUL-terminated string padded to a 4-byte boundary.
STRING_CHUNKS = frozenset(
    (b"RIFFNAME", b"OBHIERNM", b"CUTTRNAM", b"CTUSRHIE", b"SOUNDDIR",
     b"SHPEXTFN", b"SHPFNAME", b"SHPFRGTP", b"TRSNDCAT", b"EXTOBJNM")
)

#: The name of the fallback property for a chunk with no named schema.
GENERIC_FIELD = "data"

_SIZE = {I32: 4, U32: 4, F32: 4}


class SchemaError(Exception):
    pass


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
