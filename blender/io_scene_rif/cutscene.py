"""Cutscenes as a model, between the chunk tree and the Blender scene.

A cutscene is a camera path, a cast of participants and a timed event list,
stored inside the level ``.rif`` under ``REBENVDT/SPECLOBJ``. The chunk layouts
are in :mod:`schema`; what lives here is the *shape* -- turning the flat
``[(path, props)]`` absorbed form into nested objects and back, so that
:mod:`scene` can build Blender datablocks from something structured.

This module imports no ``bpy``, so ``tests/test_cutscene.py`` exercises the whole
parse/emit round trip with Blender absent.

Three things it owns beyond the nesting:

**Time.** A point carries the duration of the interval that *starts* at it, in
milliseconds, and every shipped value is a multiple of 40 -- the engine's 25 Hz
tick. So at :data:`FPS` one frame is exactly one tick and a keyframe number is a
lossless representation of an authored duration. That is what lets the camera
path live in F-curves rather than in a parallel array of times that could
desync.

**The phantom control points.** The path is a uniform Catmull-Rom spline and the
loader synthesises two extra control points by reflection before evaluating it.
An exporter must therefore write only the authored points -- emitting the
phantoms too would add a spurious segment at each end -- while a *preview* has
to add them back or it draws a curve the game will not follow.

**Ending.** Running off the end of a track does not end a cutscene; only an
event does (kind 3 with payload 0). A cutscene authored without one plays its
path and then sits there with the camera locked, so :func:`end_event` exists to
make the correct thing easy.

Layouts, addresses and the measurements behind all of this:
rif_chunk_format.md, "The cutscene chunks".
"""

try:                                  # inside the addon package
    from . import rif
    from . import schema
except ImportError:                   # loaded directly, as the tests do
    import rif
    import schema

#: Milliseconds per engine tick. Every one of the 763 shipped point durations is
#: a multiple of this.
TICK_MS = 40

#: Frames per second at which one frame is exactly one tick, so a duration in
#: milliseconds and a keyframe number convert without loss.
FPS = 1000 // TICK_MS

#: The chunk that holds every cutscene in a file.
SPECLOBJ = "SPECLOBJ"

#: Event kinds, named from `Cutscene_FireEvent` @ 0x005bee20.
EVENT_ANIM = 2
EVENT_CONTROL = 3
EVENT_SOUND = 5          # parsed, never dispatched -- see the module docs
EVENT_SUBTITLE = 7
EVENT_ANIM_ALT = 8
EVENT_FADE = 10
EVENT_PARTICLES = 11
EVENT_TIME_SCALE = 12
EVENT_CONSOLE = 13

#: `EVENT_CONTROL` payloads.
CONTROL_END = 0
CONTROL_NEXT_SEGMENT = 1
CONTROL_NEXT_LEVEL = 3


class CutsceneError(Exception):
    pass


# --------------------------------------------------------------------------
# paths
# --------------------------------------------------------------------------

def _seg(path, depth):
    """The ``id:index`` segment at ``depth``, or ``None``."""
    parts = path.split("/")
    return parts[depth] if depth < len(parts) else None


def _split_seg(seg):
    cid, _, idx = seg.rpartition(":")
    return cid, int(idx or 0)


def is_cutscene_path(path):
    return "/CUTSHEAD:" in path or path.endswith("/CUTSHEAD") or path.startswith("CUTSHEAD:")


def split_absorbed(entries):
    """``(cutscene_entries, everything_else)``, order preserved in both."""
    mine, rest = [], []
    for path, props in entries:
        (mine if is_cutscene_path(path) else rest).append((path, props))
    return mine, rest


# --------------------------------------------------------------------------
# the model
# --------------------------------------------------------------------------

class Track:
    """One ``CUTTRACK``: a path segment of one participant."""

    def __init__(self, index=0):
        self.index = index
        self.name = ""
        self.name_fields = [0, 0]
        #: ``[(x, y, z, packed_time)]`` in rif units; see :func:`point_frames`.
        self.points = []
        self.start_quat = [0.0, 0.0, 0.0, 1.0]
        self.end_quat = [0.0, 0.0, 0.0, 1.0]
        self.has_start_quat = 0
        self.has_end_quat = 0
        self.unread = [0, 0]
        #: Degrees, or ``None`` when the track carries no ``CUTTRFOV`` -- which
        #: the engine reads as 90.
        self.fov_degrees = None
        self.fov_fields = [0, 0]
        #: ``[(index, props)]`` -- one entry per ``CUTEVENT`` chunk, kept in the
        #: decoded form :mod:`schema` produces.
        self.events = []
        #: Index of the CUTPOINT / CUTTRFOV chunks, so a round trip keeps them.
        self._point_index = 0
        self._fov_index = 0
        self._name_index = 0

    @property
    def positions(self):
        return [(p[0], p[1], p[2]) for p in self.points]

    @property
    def durations(self):
        """Milliseconds per interval, one per point."""
        return [schema.point_time_ms(p[3]) for p in self.points]

    @property
    def spares(self):
        """The ignored top byte of each point's packed time."""
        return [(p[3] >> 24) & 0xff for p in self.points]


class Participant:
    """One ``CUTSCUSR``: an actor, or one half of the camera."""

    def __init__(self, index=0):
        self.index = index
        self.rif_name = ""
        self.field_0 = 0
        self.anim_id = -1
        self.user_id = 0
        self.field_3 = 0
        #: 0 marks the camera-position track.
        self.is_camera = 1
        #: Bit 0 marks the camera look-at track.
        self.flags = 0
        self.fields_6_11 = [0] * 6
        self.hierarchy = None          # (name, [3 ints]) or None
        self.sound_props = None        # [6 ints] or None
        self.tracks = []
        self._data_index = 0
        self._hier_index = 0
        self._sound_index = 0

    @property
    def is_camera_position(self):
        return self.is_camera == 0

    @property
    def is_camera_target(self):
        return bool(self.flags & 1) and not self.is_camera_position


class Cutscene:
    """One ``CUTSHEAD``."""

    def __init__(self, index=0):
        self.index = index
        self.name = ""
        self.position = [0, 0, 0]
        self.reserved = [0, 0]
        self.participants = []
        self.speclobj_index = 0
        self.prefix = ""               # e.g. "REBENVDT:1/SPECLOBJ:3"
        self._data_index = 0

    def camera_position_track(self):
        for p in self.participants:
            if p.is_camera_position:
                return p
        return None

    def camera_target_track(self):
        for p in self.participants:
            if p.is_camera_target:
                return p
        return None


# --------------------------------------------------------------------------
# parse
# --------------------------------------------------------------------------

def parse(entries):
    """``[(path, props)]`` -> ``[Cutscene]``.

    Only the cutscene entries are consulted; pass the whole absorbed list and
    the rest is ignored (use :func:`split_absorbed` to keep them).
    """
    mine, _ = split_absorbed(entries)
    scenes = {}

    def scene_for(path):
        parts = path.split("/")
        at = next(i for i, s in enumerate(parts) if s.startswith("CUTSHEAD:"))
        prefix = "/".join(parts[:at])
        _, idx = _split_seg(parts[at])
        key = (prefix, idx)
        if key not in scenes:
            cs = Cutscene(idx)
            cs.prefix = prefix
            scenes[key] = cs
        return scenes[key], parts[at + 1:]

    for path, props in mine:
        cs, rest = scene_for(path)
        if not rest:
            continue
        cid, idx = _split_seg(rest[0])

        if cid == "CUTSCDAT":
            cs.name = props["name"]
            cs.position = list(props["position"])
            cs.reserved = list(props["reserved"])
            cs._data_index = idx
            continue
        if cid != "CUTSCUSR":
            raise CutsceneError("unexpected %s under CUTSHEAD" % cid)

        part = _participant(cs, idx)
        cid2, idx2 = _split_seg(rest[1])
        if cid2 == "CTUSRDAT":
            part.rif_name = props["name"]
            part.field_0 = props["field_0"]
            part.anim_id = props["anim_id"]
            part.user_id = props["user_id"]
            part.field_3 = props["field_3"]
            part.is_camera = props["is_camera"]
            part.flags = props["flags"]
            part.fields_6_11 = list(props["fields_6_11"])
            part._data_index = idx2
        elif cid2 == "CTUSRHIE":
            part.hierarchy = (props["name"], list(props["fields"]))
            part._hier_index = idx2
        elif cid2 == "CTUSNDPR":
            part.sound_props = list(props["sound_properties"])
            part._sound_index = idx2
        elif cid2 == "CUTTRACK":
            track = _track(part, idx2)
            cid3, idx3 = _split_seg(rest[2])
            if cid3 == "CUTTRNAM":
                track.name = props["name"]
                track.name_fields = list(props["fields"])
                track._name_index = idx3
            elif cid3 == "CUTPOINT":
                pts = list(props["points"])
                track.points = [tuple(pts[k:k + 4]) for k in range(0, len(pts), 4)]
                track.start_quat = list(props["start_quat"])
                track.end_quat = list(props["end_quat"])
                track.has_start_quat = props["has_start_quat"]
                track.has_end_quat = props["has_end_quat"]
                track.unread = list(props["unread"])
                track._point_index = idx3
            elif cid3 == "CUTTRFOV":
                track.fov_degrees = props["fov_degrees"]
                track.fov_fields = list(props["fields"])
                track._fov_index = idx3
            elif cid3 == "CUTEVENT":
                track.events.append((idx3, dict(props)))
            else:
                raise CutsceneError("unexpected %s under CUTTRACK" % cid3)
        else:
            raise CutsceneError("unexpected %s under CUTSCUSR" % cid2)

    out = [scenes[k] for k in sorted(scenes)]
    for cs in out:
        cs.participants.sort(key=lambda p: p.index)
        for p in cs.participants:
            p.tracks.sort(key=lambda t: t.index)
            for t in p.tracks:
                t.events.sort(key=lambda e: e[0])
    return out


def _participant(cs, index):
    for p in cs.participants:
        if p.index == index:
            return p
    p = Participant(index)
    cs.participants.append(p)
    return p


def _track(part, index):
    for t in part.tracks:
        if t.index == index:
            return t
    t = Track(index)
    part.tracks.append(t)
    return t


# --------------------------------------------------------------------------
# emit
# --------------------------------------------------------------------------

def emit(cutscenes):
    """``[Cutscene]`` -> ``[(path, rif.Chunk)]`` for ``scene._emit_from``'s `extra`.

    Only leaves are emitted; the containers above them are created from the path
    by the caller, exactly as the texture table's injection works.
    """
    out = []

    def add(path, cid, props):
        out.append((path, rif.Chunk(cid, schema.encode(cid, props))))

    for cs in cutscenes:
        head = "%s/CUTSHEAD:%d" % (cs.prefix, cs.index) if cs.prefix else \
               "CUTSHEAD:%d" % cs.index
        add("%s/CUTSCDAT:%d" % (head, cs._data_index), b"CUTSCDAT", {
            "position": cs.position,
            "name": cs.name,
            "reserved": cs.reserved,
            # Regenerated, never carried: it is a pure function of the name, and
            # carrying it would let a renamed cutscene keep an id that no longer
            # matches -- the same rule SHPHEAD1's counts follow.
            "name_hash": schema.cutscene_name_hash(cs.name),
        })

        for part in cs.participants:
            base = "%s/CUTSCUSR:%d" % (head, part.index)
            add("%s/CTUSRDAT:%d" % (base, part._data_index), b"CTUSRDAT", {
                "name": part.rif_name,
                "field_0": part.field_0,
                "anim_id": part.anim_id,
                "user_id": part.user_id,
                "field_3": part.field_3,
                "is_camera": part.is_camera,
                "flags": part.flags,
                "fields_6_11": part.fields_6_11,
            })
            if part.hierarchy is not None:
                add("%s/CTUSRHIE:%d" % (base, part._hier_index), b"CTUSRHIE",
                    {"name": part.hierarchy[0], "fields": part.hierarchy[1]})
            if part.sound_props is not None:
                add("%s/CTUSNDPR:%d" % (base, part._sound_index), b"CTUSNDPR",
                    {"sound_properties": part.sound_props})

            for track in part.tracks:
                tb = "%s/CUTTRACK:%d" % (base, track.index)
                add("%s/CUTTRNAM:%d" % (tb, track._name_index), b"CUTTRNAM",
                    {"name": track.name, "fields": track.name_fields})
                flat = []
                for p in track.points:
                    flat.extend(p)
                add("%s/CUTPOINT:%d" % (tb, track._point_index), b"CUTPOINT", {
                    "points": flat,
                    "start_quat": track.start_quat,
                    "end_quat": track.end_quat,
                    "has_start_quat": track.has_start_quat,
                    "has_end_quat": track.has_end_quat,
                    "unread": track.unread,
                })
                if track.fov_degrees is not None:
                    add("%s/CUTTRFOV:%d" % (tb, track._fov_index), b"CUTTRFOV",
                        {"fov_degrees": track.fov_degrees,
                         "fields": track.fov_fields})
                for idx, props in track.events:
                    add("%s/CUTEVENT:%d" % (tb, idx), b"CUTEVENT", props)
    return out


# --------------------------------------------------------------------------
# time
# --------------------------------------------------------------------------

#: What the loader substitutes for a stored duration of zero on a non-final
#: point (`CameraTrack_LoadFromCutscene` uses `1.0f` there). So a stored 0 and a
#: stored 1000 describe the *same* playback, and 129 of the 341 shipped
#: non-final points store the 0. Frames are built from the effective value --
#: otherwise two control points land on one keyframe and Blender keeps one,
#: silently shortening the path.
ZERO_DURATION_MS = 1000


def effective_durations(track):
    """Durations as the engine sees them: a non-final 0 means one second."""
    out = list(track.durations)
    for i in range(len(out) - 1):
        if out[i] == 0:
            out[i] = ZERO_DURATION_MS
    return out


def point_frames(track):
    """Keyframe number per point, at :data:`FPS`.

    Point 0 sits at frame 0 and each subsequent point is offset by the previous
    point's *effective* duration. Exact for shipped data, because every duration
    is a whole number of ticks.
    """
    frames = [0]
    for ms in effective_durations(track)[:-1]:
        frames.append(frames[-1] + ms // TICK_MS)
    return frames


def durations_from_frames(frames, final_ms=0):
    """Inverse of :func:`point_frames`; ``final_ms`` is the last point's value.

    The final point has no interval after it, so its stored duration is unread
    by the engine -- it is 0 in 94% of shipped multi-point tracks.
    """
    out = []
    for a, b in zip(frames, frames[1:]):
        if b < a:
            raise CutsceneError("keyframes must not go backwards: %d then %d"
                                % (a, b))
        out.append((b - a) * TICK_MS)
    out.append(final_ms)
    return out


def zero_flags(track):
    """Per point: was a one-second interval stored as the literal 0?

    Both encodings play identically -- the loader substitutes 1.0 s for a
    non-final 0 -- so this only chooses which of two equivalent bytes to write.
    That is why it is safe to carry alongside the keyframes: if it falls out of
    step with an edited path it can pick the *other* encoding of the same
    duration, and nothing else. 129 of the 341 shipped non-final points store
    the 0 and two store a literal 1000, so neither is a canonical form.
    """
    return [1 if ms == 0 else 0 for ms in track.durations]


def pack_points(positions, frames, spares=(), final_ms=0, zeros=()):
    """``[(x, y, z, packed)]`` from integer positions and keyframe numbers."""
    if len(positions) != len(frames):
        raise CutsceneError("%d positions but %d frames"
                            % (len(positions), len(frames)))
    durations = durations_from_frames(frames, final_ms)
    spares = list(spares) + [0] * (len(positions) - len(spares))
    zeros = list(zeros) + [0] * (len(positions) - len(zeros))
    out = []
    for i, p in enumerate(positions):
        ms = durations[i]
        if ms == ZERO_DURATION_MS and zeros[i] and i < len(positions) - 1:
            ms = 0
        out.append((int(p[0]), int(p[1]), int(p[2]),
                    schema.pack_point_time(ms, spares[i])))
    return out


# --------------------------------------------------------------------------
# the spline
# --------------------------------------------------------------------------

def control_points(positions):
    """The authored points plus the two phantoms the loader synthesises.

    ``P[0] = 2*P[1] - P[2]`` and the mirror at the tail, or a duplicate when
    there are fewer than two points. Preview only -- never write these to a
    ``CUTPOINT``.
    """
    pts = [tuple(p) for p in positions]
    if not pts:
        return []
    if len(pts) < 2:
        return [pts[0], pts[0], pts[0], pts[0]]
    head = tuple(2 * pts[0][i] - pts[1][i] for i in range(3))
    tail = tuple(2 * pts[-1][i] - pts[-2][i] for i in range(3))
    return [head] + pts + [tail]


def sample_segment(p0, p1, p2, p3, t):
    """Uniform Catmull-Rom, exactly as `CatmullRomSample` @ 0x005c1a40."""
    out = []
    for i in range(3):
        a = -p0[i] + 3 * p1[i] - 3 * p2[i] + p3[i]
        b = 2 * p0[i] - 5 * p1[i] + 4 * p2[i] - p3[i]
        c = p2[i] - p0[i]
        out.append(p1[i] + 0.5 * (c * t + b * t * t + a * t * t * t))
    return tuple(out)


def sample_path(positions, per_segment=12):
    """Points along the whole path, for drawing what the engine will follow."""
    q = control_points(positions)
    if len(q) < 4:
        return list(q)
    out = []
    for j in range(1, len(q) - 2):
        for s in range(per_segment):
            out.append(sample_segment(q[j - 1], q[j], q[j + 1], q[j + 2],
                                      s / float(per_segment)))
    out.append(tuple(q[-2]))
    return out


# --------------------------------------------------------------------------
# building a cutscene from scratch
# --------------------------------------------------------------------------

def _event(kind, payload, delay_ms=0, headers=None, text=None):
    """One CUTEVENT chunk holding a single record."""
    head = list(headers or [delay_ms, 0, 0, 0])
    return {
        "position": 0.0,
        "fields": [0, 0],
        "name": "",
        "kinds": [kind],
        "headers": head,
        "payload": list(payload),
        "payload_counts": [len(payload)],
        "strings": text if text is not None else "",
    }


def end_event(position=0.0, name="end"):
    """The event that ends a cutscene.

    Without one the path plays out and the camera stays locked at the last
    point -- the engine does not end a cutscene when a track runs out.
    """
    ev = _event(EVENT_CONTROL, [CONTROL_END, 0, 0])
    ev["position"] = float(position)
    ev["name"] = name
    return ev


def console_event(command, position=0.0, name="console"):
    """An event that queues a console line -- kind 13."""
    ev = _event(EVENT_CONSOLE, [0, 0], text=command)
    ev["position"] = float(position)
    ev["name"] = name
    return ev


def new_cutscene(name, prefix, index=0, camera_name="camera",
                 target_name="target", fov_degrees=90.0):
    """A minimal playable cutscene: a camera pair, one segment each, an end event.

    Positions are left empty; the caller fills them from the scene. The two
    participants are what the engine needs -- `is_camera == 0` marks the one it
    takes the camera position from, and `flags` bit 0 the one it looks at.
    """
    cs = Cutscene(index)
    cs.prefix = prefix
    cs.name = name

    for is_cam, flags, tname in ((0, 0, camera_name), (1, 1, target_name)):
        part = Participant(len(cs.participants))
        part.is_camera = is_cam
        part.flags = flags
        part.user_id = len(cs.participants)
        part.anim_id = -1
        track = Track(0)
        track.name = tname
        if is_cam == 0:
            track.fov_degrees = float(fov_degrees)
            track.events.append((0, end_event()))
        part.tracks.append(track)
        cs.participants.append(part)
    return cs


def scale_to_rif(vec, scale, origin=(0.0, 0.0, 0.0)):
    """Blender metres (Z up) -> rif units (Y down), as integers.

    The inverse of the importer's ``(x, y, z) -> (x, z, -y)`` swizzle.
    """
    x = (vec[0] - origin[0]) / scale
    y = (vec[1] - origin[1]) / scale
    z = (vec[2] - origin[2]) / scale
    return (int(round(x)), int(round(-z)), int(round(y)))


def rif_to_scene(pt, scale, origin=(0.0, 0.0, 0.0)):
    """rif units -> Blender metres."""
    return (pt[0] * scale + origin[0],
            pt[2] * scale + origin[1],
            -pt[1] * scale + origin[2])
