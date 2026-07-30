"""The record chunks that carry names, ids and counts. Imports no ``bpy``.

``OBJHEAD1``, ``SHPHEAD1``, ``OBJHIERD``, ``OBASEQHD`` and ``OBASEQFR`` are the
chunks that make something in a ``.rif`` *identifiable* or *countable*: the name
the engine resolves by ``strcmp``, the id pairing an object with its shape, the
name a hierarchy node binds to, the id matching one sequence across every node,
and the position of a keyframe within its sequence. All of it is byte layout, so
it lives outside :mod:`scene` and ``tests/test_heads.py`` measures it against
every shipped file with Blender absent.

Layouts are AvP's, read off ``3dc/win95/OBCHUNK.CPP``, ``SHPCHUNK.CPP`` and
``animobs.cpp`` (``fill_data_block`` writes them, the from-buffer constructor
reads them back), and confirmed against the shipped assets -- see the agreement
figures on :func:`sync_shphead` and :func:`make_seqhead`.

**A name is a trailing NUL-terminated string, padded to a 4-byte boundary**, and
the padding is *not* reliably zero: ``SQUARE.RIF``'s ``OBJHEAD1`` ends
``'SQUARE\\0C'``. So a name is read up to its first NUL and rewritten with fresh
padding; nothing depends on what was in those bytes.
"""

import math
import struct

# --------------------------------------------------------------------------
# OBJHEAD1 -- AvP's Object_Header_Chunk::fill_data_block
# --------------------------------------------------------------------------
#
#   0x00 flags            0x20 orientation float[4] (x, y, z, w)
#   0x04 lock_user[16]    0x30 index_num
#   0x14 location int[3]  0x34 version_no
#                         0x38 shape_id_no
#                         0x3c o_name, NUL-terminated, padded to 4
OBJHEAD1_FLAGS = 0x00
OBJHEAD1_LOCK_USER = 0x04
OBJHEAD1_LOCATION = 0x14
OBJHEAD1_ORIENT = 0x20
OBJHEAD1_INDEX_NUM = 0x30
OBJHEAD1_VERSION = 0x34
OBJHEAD1_SHAPE_ID = 0x38
OBJHEAD1_NAME = 0x3C
OBJHEAD1_HEADER = 0x3C

#: What every shipped ``OBJHEAD1`` that carries geometry has in ``flags``. The
#: field is AvP's ``object_flags`` bitfield, whose only documented bit is
#: ``locked``; 0x400 is not that bit and its meaning is not recovered, so a new
#: object copies what the assets do rather than inventing a value.
DEFAULT_OBJECT_FLAGS = 0x400

#: The 16 bytes at ``OBJHEAD1+0x04`` are AvP's ``lock_user`` -- who holds the
#: editor's lock on this object -- **not** the object's name, which is the
#: trailing string at 0x3c. 8,136 of the 9,313 shipped objects carry something
#: here (``Player`` in most, uninitialised junk in the rest), so it is authored
#: data and gets carried through an edit rather than regenerated.

# --------------------------------------------------------------------------
# SHPHEAD1 -- AvP's Shape_Header_Chunk::fill_data_block
# --------------------------------------------------------------------------
#
#   0x00 flags            0x24 max.x  0x28 min.x
#   0x04 lock_user[16]    0x2c max.y  0x30 min.y
#   0x14 file_id_num      0x34 max.z  0x38 min.z
#   0x18 num_verts        0x3c version_no
#   0x1c num_polys        0x40 number of associated object names
#   0x20 radius (float)   0x44 that many NUL-terminated names, padded to 4
SHPHEAD1_FLAGS = 0x00
SHPHEAD1_LOCK_USER = 0x04
SHPHEAD1_FILE_ID = 0x14
SHPHEAD1_NUM_VERTS = 0x18
SHPHEAD1_NUM_POLYS = 0x1C
SHPHEAD1_RADIUS = 0x20
SHPHEAD1_BOUNDS = 0x24
SHPHEAD1_VERSION = 0x3C
SHPHEAD1_NUM_NAMES = 0x40
SHPHEAD1_HEADER = 0x44

LOCK_USER_SIZE = 16


# --------------------------------------------------------------------------
# names
# --------------------------------------------------------------------------

def pad_name(name):
    """One trailing name as it goes on the wire: bytes, NUL, padded to 4.

    ``(len + 4) & ~3`` is AvP's own rule and always leaves at least one NUL, so
    a name is never left unterminated by the padding arithmetic.
    """
    raw = name.encode("latin-1", "replace")
    return raw + b"\0" * (((len(raw) + 4) & ~3) - len(raw))


def trailing_name(body, offset):
    """The NUL-terminated name at ``offset``, or ``""`` if there is not one.

    Non-printable content reads as absent rather than as a name, because an
    uninitialised tail is common and a control character is never a real name.
    """
    if len(body) <= offset:
        return ""
    text = body[offset:].split(b"\0")[0]
    if not text or not all(32 <= b < 127 for b in text):
        return ""
    return text.decode("ascii")


def _names_at(body, offset, count):
    out = []
    pos = offset
    for _ in range(count):
        if pos >= len(body):
            break
        end = body.find(b"\0", pos)
        if end < 0:
            end = len(body)
        out.append(body[pos:end].decode("latin-1"))
        pos = end + 1
    return out


# --------------------------------------------------------------------------
# OBJHEAD1
# --------------------------------------------------------------------------

def make_objhead(name, location=(0, 0, 0), orientation=(0.0, 0.0, 0.0, 1.0),
                 shape_id=-1, flags=DEFAULT_OBJECT_FLAGS, index_num=0, version=0,
                 lock_user=b""):
    """A complete ``OBJHEAD1`` body for a new object."""
    body = bytearray(OBJHEAD1_HEADER)
    struct.pack_into("<i", body, OBJHEAD1_FLAGS, flags)
    body[OBJHEAD1_LOCK_USER:OBJHEAD1_LOCK_USER + LOCK_USER_SIZE] = \
        bytes(lock_user)[:LOCK_USER_SIZE].ljust(LOCK_USER_SIZE, b"\0")
    struct.pack_into("<3i", body, OBJHEAD1_LOCATION, *(int(v) for v in location))
    struct.pack_into("<4f", body, OBJHEAD1_ORIENT, *(float(v) for v in orientation))
    struct.pack_into("<i", body, OBJHEAD1_INDEX_NUM, index_num)
    struct.pack_into("<i", body, OBJHEAD1_VERSION, version)
    struct.pack_into("<i", body, OBJHEAD1_SHAPE_ID, shape_id)
    return bytes(body) + pad_name(name)


def objhead_name(body):
    return trailing_name(body, OBJHEAD1_NAME)


def set_objhead_name(body, name):
    """``body`` with its trailing name replaced. The fixed header is untouched."""
    head = bytes(body[:OBJHEAD1_HEADER]).ljust(OBJHEAD1_HEADER, b"\0")
    return head + pad_name(name)


def objhead_shape_id(body):
    if len(body) < OBJHEAD1_SHAPE_ID + 4:
        return -1
    return struct.unpack_from("<i", body, OBJHEAD1_SHAPE_ID)[0]


def set_objhead_shape_id(body, shape_id):
    out = bytearray(bytes(body).ljust(OBJHEAD1_SHAPE_ID + 4, b"\0"))
    struct.pack_into("<i", out, OBJHEAD1_SHAPE_ID, shape_id)
    return bytes(out)


# --------------------------------------------------------------------------
# SHPHEAD1
# --------------------------------------------------------------------------

def shape_bounds(verts):
    """``(min, max)`` over integer RIF vertices."""
    if not verts:
        return (0, 0, 0), (0, 0, 0)
    lo = tuple(min(v[k] for v in verts) for k in range(3))
    hi = tuple(max(v[k] for v in verts) for k in range(3))
    return lo, hi


def shape_radius(verts):
    """Distance from the **origin** to the furthest vertex, not from the centre.

    The same quantity ``SHPCENTR`` carries -- and the two are byte-identical in
    all 9,244 shipped shapes that have both -- which is why they are regenerated
    with one formula rather than two.
    """
    return max((math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2) for v in verts), default=0.0)


def make_shphead(names, file_id, verts, num_polys, flags=0, lock_user=b"", version=0):
    """A complete ``SHPHEAD1`` body for a new shape, derived from its geometry."""
    lo, hi = shape_bounds(verts)
    body = bytearray(SHPHEAD1_HEADER)
    struct.pack_into("<i", body, SHPHEAD1_FLAGS, flags)
    body[SHPHEAD1_LOCK_USER:SHPHEAD1_LOCK_USER + LOCK_USER_SIZE] = \
        bytes(lock_user)[:LOCK_USER_SIZE].ljust(LOCK_USER_SIZE, b"\0")
    struct.pack_into("<i", body, SHPHEAD1_FILE_ID, file_id)
    struct.pack_into("<i", body, SHPHEAD1_NUM_VERTS, len(verts))
    struct.pack_into("<i", body, SHPHEAD1_NUM_POLYS, num_polys)
    struct.pack_into("<f", body, SHPHEAD1_RADIUS, shape_radius(verts))
    struct.pack_into("<6i", body, SHPHEAD1_BOUNDS,
                     hi[0], lo[0], hi[1], lo[1], hi[2], lo[2])
    struct.pack_into("<i", body, SHPHEAD1_VERSION, version)
    struct.pack_into("<i", body, SHPHEAD1_NUM_NAMES, len(names))
    tail = b"".join(n.encode("latin-1", "replace") + b"\0" for n in names)
    pad = (-len(tail)) % 4
    return bytes(body) + tail + b"\0" * pad


def shphead_file_id(body):
    if len(body) < SHPHEAD1_FILE_ID + 4:
        return -1
    return struct.unpack_from("<i", body, SHPHEAD1_FILE_ID)[0]


def shphead_names(body):
    if len(body) < SHPHEAD1_NUM_NAMES + 4:
        return []
    count = struct.unpack_from("<i", body, SHPHEAD1_NUM_NAMES)[0]
    if count < 0 or count > 64:  # a corrupt count must not drive an allocation
        return []
    return _names_at(body, SHPHEAD1_HEADER, count)


def sync_shphead(body, file_id, verts, num_polys, names):
    """``body`` with every field derived from the geometry brought up to date.

    ``flags``, ``lock_user`` and ``version_no`` are carried through: they are
    authored (or editor bookkeeping), not derived. Everything else is recomputed,
    because a stale count or bound is worse than a regenerated one -- AvP's
    loader reads all six straight into its shape record, and Gunlok derives a
    role's collision extents from the bounds when the GLS gives ``radius``/
    ``height`` as 0.

    Regenerating is safe for an *unedited* shape: across the 9,357 shipped shapes
    the recomputed ``num_verts``, ``num_polys`` and both bound corners reproduce
    the stored values in **100%** of cases, so the whole header comes back
    byte-identical but for one word.

    That word is ``radius``, bit-exact in 42% and drifting in the rest -- median
    7e-7 relative, p90 5e-4, and only 4 shapes past 1%. It is the same quantity
    ``SHPCENTR`` holds, the two are byte-identical in all 9,244 shipped shapes
    that have both, and the exporter already recomputes ``SHPCENTR``. So
    regenerating this one from the same formula is what keeps the pair agreeing;
    carrying it instead would make every export disagree with its own
    ``SHPCENTR`` in 58% of shapes.
    """
    raw = bytes(body).ljust(SHPHEAD1_HEADER, b"\0")
    flags = struct.unpack_from("<i", raw, SHPHEAD1_FLAGS)[0]
    lock_user = raw[SHPHEAD1_LOCK_USER:SHPHEAD1_LOCK_USER + LOCK_USER_SIZE]
    version = struct.unpack_from("<i", raw, SHPHEAD1_VERSION)[0]
    return make_shphead(names, file_id, verts, num_polys,
                        flags=flags, lock_user=lock_user, version=version)


# --------------------------------------------------------------------------
# OBJHIERD -- the name a hierarchy node binds to
# --------------------------------------------------------------------------
#
#   0x00 num_extra_data (0 in all 5,250 shipped nodes)
#   0x04 name, NUL-terminated, padded to 4
#
# AvP's `Object_Hierarchy_Data_Chunk::find_object_for_this_section` resolves it
# with `strcmp` against each object's name, and it is the only binding there is:
# without this chunk a node drives nothing. Every one of the 5,250 shipped
# OBJCHIER nodes has exactly one.
OBJHIERD_NAME = 0x04


def make_objhierd(name, num_extra_data=0):
    return struct.pack("<i", num_extra_data) + pad_name(name)


def objhierd_name(body):
    return trailing_name(body, OBJHIERD_NAME)


# --------------------------------------------------------------------------
# OBASEQHD -- one animation sequence's header
# --------------------------------------------------------------------------
#
#   0x00 num_frames   0x08 sub_sequence_number   0x10 name
#   0x04 sequence_number  0x0c num_extra_data
#
# AvP's field names (`animobs.cpp`), but **Gunlok does not use `num_frames` as a
# count**: it is 65536 in all 29,550 shipped sequences, i.e. 1.0 in the same
# 16.16 timebase `OBASEQFR.time` is expressed in. Read it as the span a frame's
# time is a position within. `sequence_number` is 0 in all 29,550 and
# `num_extra_data` likewise, so the name always starts at 0x10.
SEQHEAD_NUM_FRAMES = 0x00
SEQHEAD_SEQUENCE_NUMBER = 0x04
SEQHEAD_SUB_SEQUENCE = 0x08
SEQHEAD_NUM_EXTRA = 0x0C
SEQHEAD_NAME = 0x10
SEQHEAD_HEADER = 0x10

#: The full 16.16 span. Every shipped sequence declares it, and no frame's
#: ``time`` ever reaches it -- the largest observed is 65530.
SEQUENCE_SPAN = 65536


def make_seqhead(name, sub_sequence_number, num_frames=SEQUENCE_SPAN,
                 sequence_number=0):
    """One ``OBASEQHD``.

    ``sub_sequence_number`` is **a per-file sequence id, not a counter**, which
    is measurable rather than guessable: it is distinct among the sequences of
    each of the 4,270 shipped ``OBANSEQS`` nodes, and identical across nodes for
    the same sequence name in all 912 (file, name) pairs with no exceptions. So
    every node's copy of ``Seq_Walk`` carries one number, and that is how a
    sequence is matched across the skeleton. A new sequence needs a fresh one,
    not a zero.
    """
    body = struct.pack("<4i", num_frames, sequence_number, sub_sequence_number, 0)
    return body + pad_name(name)


def seqhead_name(body):
    return trailing_name(body, SEQHEAD_NAME)


def seqhead_fields(body):
    """``(num_frames, sequence_number, sub_sequence_number)``, defaulted if short."""
    if len(body) < SEQHEAD_NUM_EXTRA:
        return SEQUENCE_SPAN, 0, 0
    num_frames, seq_no, sub = struct.unpack_from("<3i", body, 0)
    return num_frames, seq_no, sub


# --------------------------------------------------------------------------
# OBASEQFR -- one keyframe's position in its sequence
# --------------------------------------------------------------------------
#
# The body itself is described by `schema.SCHEMA[b"OBASEQFR"]`; what lives here
# is the part that has to be *generated*.
#
# `flags` is not opaque. AvP splits it (`animobs.hpp`) into a sound index in bits
# 24..30 and a flag mask in bits 0..23, and both hold in Gunlok: across all
# 323,334 shipped frames the low 24 bits are zero in 323,323, and the sound index
# is 0 in 322,796 with small values (1..17) in the 538 frames that trigger one.
#
# **Bit 31 is outside both of AvP's masks and marks the sequence's origin
# frame** -- set in 9,693 frames. `BuildSequence` scans a sequence for the first
# frame carrying it and rebases every frame onto that one, then the engine skips
# the offset pass it would otherwise apply. So it is never generated here:
# marking the wrong frame silently re-anchors a whole animation, and it is
# preserved rather than offered as an edit. Full trace in `rif_chunk_format.md`.
FRAME_SOUND_INDEX_MASK = 0x7F000000
FRAME_FLAG_MASK = 0x00FFFFFF
FRAME_ORIGIN = 0x80000000


def frame_sound_index(flags):
    return (flags & FRAME_SOUND_INDEX_MASK) >> 24


# --------------------------------------------------------------------------
# OBASEQFL -- one sequence's flags
# --------------------------------------------------------------------------
#
# AvP's names (`animobs.hpp`). Gunlok ships exactly four values across the 722
# chunks that have one -- 0x4, 0x8, 0x84, 0x88 -- so in practice it is
# loop-or-not plus bit 0x80, and the two loop bits are never set together.
SEQ_FLAG_UPPER = 0x00000001      # Mummy-specific in AvP; unused here
SEQ_FLAG_LOWER = 0x00000002      # likewise
SEQ_FLAG_LOOPS = 0x00000004
SEQ_FLAG_NO_LOOP = 0x00000008
SEQ_FLAG_NO_INTERPOLATION = 0x00000010
SEQ_FLAG_HALF_FRAME_RATE = 0x00000020

#: The bits this addon lets you set. Everything else in the word -- including the
#: 0x80 that 181 shipped chunks carry and nobody has explained -- is preserved
#: from the file and never generated.
SEQ_FLAG_KNOWN = (SEQ_FLAG_LOOPS | SEQ_FLAG_NO_LOOP
                  | SEQ_FLAG_NO_INTERPOLATION | SEQ_FLAG_HALF_FRAME_RATE)


def seq_loop_mode(flags):
    """``'LOOP'``, ``'ONCE'`` or ``'UNSET'`` for a sequence flags word."""
    if flags & SEQ_FLAG_LOOPS:
        return "LOOP"
    if flags & SEQ_FLAG_NO_LOOP:
        return "ONCE"
    return "UNSET"


def set_seq_loop_mode(flags, mode):
    """``flags`` with the loop bits replaced. Every other bit is left alone."""
    out = flags & ~(SEQ_FLAG_LOOPS | SEQ_FLAG_NO_LOOP)
    if mode == "LOOP":
        out |= SEQ_FLAG_LOOPS
    elif mode == "ONCE":
        out |= SEQ_FLAG_NO_LOOP
    return out


def sequence_times(frames, anchors, extent=None):
    """Where each Blender frame number sits in a sequence's 16.16 timebase.

    ``frames`` is the sorted list of this bone's keyframe positions; ``anchors``
    is the ``[(frame, time)]`` an import recorded, which may be empty; ``extent``
    is ``(first, last)`` over the **whole sequence**, every bone included.

    **The extent has to be the sequence's, not this bone's.** A sequence is one
    clip shared by the skeleton, so if one bone is keyed over frames 0..20 and
    another only over 0..10, both have to agree that frame 10 is halfway. Scaling
    each bone by its own key range instead puts the second bone's last key at the
    end of the clip, and the parts drift apart.

    **Anchored rather than recomputed, because the shipped times are authored.**
    Only 3,712 of the 27,731 non-trivial shipped sequences match
    ``floor(k * 65536 / n)`` and none match ``k * 65536 / (n-1)``, so there is no
    formula to re-derive them with -- regenerating every time would rewrite 87%
    of the animation in the game on a round trip that changed nothing. So a key
    that came from the file keeps its exact time, and only a key that did not is
    placed, by linear interpolation between its neighbours.

    With no anchors at all -- a sequence authored from scratch -- the fallback is
    ``floor(k * 65536 / span)`` over ``span = last - first + 1`` frames. That is
    the dominant shipped generator (it is the top pattern for both the 31-frame
    and 2-frame sequences) and, by counting the loop-back frame in the span, it
    never reaches 65536, which nothing shipped does either.
    """
    if not frames:
        return []

    # Matched with a tolerance, not by equality: a keyframe position survives a
    # .blend round trip as the float it was inserted at, but nothing guarantees
    # bit-identity through the evaluation that put it there.
    def anchor_at(f):
        for af, at in anchors:
            if abs(af - f) < 1e-4:
                return int(at)
        return None

    exact = [(f, anchor_at(f)) for f in frames if anchor_at(f) is not None]

    if not exact:
        first, last = extent if extent else (frames[0], frames[-1])
        span = max((last - first) + 1.0, 1.0)
        return [max(0, min(int(SEQUENCE_SPAN * (f - first) / span), SEQUENCE_SPAN - 1))
                for f in frames]

    out = []
    for f in frames:
        hit = anchor_at(f)
        if hit is not None:
            out.append(hit)
            continue
        before = [(af, at) for af, at in exact if af < f]
        after = [(af, at) for af, at in exact if af > f]
        if before and after:
            (f0, t0), (f1, t1) = before[-1], after[0]
            out.append(int(round(t0 + (t1 - t0) * (f - f0) / (f1 - f0))))
        elif before:
            # Past the last anchor: continue at the rate the anchors establish,
            # or fall back to the whole-sequence rate when there is only one.
            (f0, t0) = before[-1]
            rate = ((t0 - exact[0][1]) / (f0 - exact[0][0])) if len(exact) > 1 else \
                (SEQUENCE_SPAN / max((frames[-1] - frames[0]) + 1.0, 1.0))
            out.append(min(int(round(t0 + rate * (f - f0))), SEQUENCE_SPAN - 1))
        else:
            (f1, t1) = after[0]
            rate = ((exact[-1][1] - t1) / (exact[-1][0] - f1)) if len(exact) > 1 else \
                (SEQUENCE_SPAN / max((frames[-1] - frames[0]) + 1.0, 1.0))
            out.append(max(int(round(t1 - rate * (f1 - f))), 0))

    # Monotonic by construction everywhere but the extrapolated ends, where a
    # clamp can flatten two keys onto one time. Nudging keeps the order the
    # animator authored.
    for i in range(1, len(out)):
        if out[i] <= out[i - 1]:
            out[i] = min(out[i - 1] + 1, SEQUENCE_SPAN - 1)
    return out


# --------------------------------------------------------------------------
# int32 arrays -- how Blender stores a body it has no schema for
# --------------------------------------------------------------------------
#
# `rif_objhead` and an absorbed chunk's `data` field are both int32 lists, so
# authoring one means going through these two.

def to_words(body):
    """Body bytes -> the signed int32 list Blender stores."""
    raw = bytes(body)
    if len(raw) % 4:
        raw += b"\0" * (-len(raw) % 4)
    return list(struct.unpack("<%di" % (len(raw) // 4), raw))


def from_words(words):
    return struct.pack("<%di" % len(words), *(int(w) for w in words))
