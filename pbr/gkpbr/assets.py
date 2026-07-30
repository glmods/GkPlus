"""What the shipped geometry says about each ``.RIM``.

The generator's whole advantage over pointing an image model at a directory of
textures is that the ``.rif`` files know things the image does not: which textures
are used at all, which polygons use each one, *where* in the sheet they land, and
what the parts using them are called. This module recovers that; :mod:`atlas`
turns it into regions.

Two facts make the UV side work, both from ``rif_chunk_format.md``:

- **A ``SHPUVCRD`` UV is a texel coordinate, not a fraction**, so it is already in
  the named texture's pixel space and needs no scaling here.
- **V grows downward** (Direct3D convention), and :class:`rim.Texture` also puts
  row 0 at the top, so a UV lands on row ``v`` with no flip. The addon's import
  flips V for Blender's sake; nothing in this pipeline does, and a normal map
  written from here is therefore in image space -- which is where the green-channel
  convention has to be reconciled, not here.

Junk UVs are real and have to be rejected rather than clamped: the shipped range
runs to ``-42118..204800``, concentrated in the ``_shadow`` meshes whose polygons
carry meaningless texture *and* UV indices because they are never textured.
"""

import collections
import os
import sys

#: The addon's decoders are pure Python and import no ``bpy``, so they are reused
#: rather than reimplemented. Added to ``sys.path`` instead of imported as a
#: package because Blender loads that directory as a flat module set.
_ADDON = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))), "blender", "io_scene_rif")
if _ADDON not in sys.path:
    sys.path.insert(0, _ADDON)

import bmpnames  # noqa: E402
import rif  # noqa: E402
import rim  # noqa: E402
import shapes as shp  # noqa: E402

#: A UV this far outside the texture is junk, not wrapping. Legitimate tiling
#: rarely exceeds a few multiples; the shadow meshes are off by thousands.
UV_SLACK = 4.0

#: Meshes that are never textured, whose texture and UV indices are both junk.
SHADOW_MARKER = "_shadow"


class Reference:
    """One polygon's use of a texture: its UV triangle and who owns it."""

    __slots__ = ("uv", "shape", "part", "rif")

    def __init__(self, uv, shape, part, rif_rel):
        self.uv = uv
        self.shape = shape
        self.part = part
        self.rif = rif_rel


def find_install(start=None):
    """Gunlok's directory, from ``GUNLOK_DIR`` or the Steam registry."""
    env = os.environ.get("GUNLOK_DIR")
    if env and os.path.isdir(env):
        return env
    if start and os.path.isdir(start):
        return start
    for base in (r"C:\Program Files (x86)\Steam", os.environ.get("STEAM_PATH", "")):
        cand = os.path.join(base, "steamapps", "common", "Gunlok")
        if os.path.isdir(cand):
            return cand
    raise SystemExit("cannot find Gunlok; set GUNLOK_DIR")


#: ``OBJHEAD1`` offsets, from ``rif_chunk_format.md``. ``+0x04`` is a 16-byte
#: ``lock_user`` -- **the editor's lock holder, not the object's name** -- and
#: reading a name from there yields ``Player`` for 6,383 of the 9,313 shipped
#: objects, which is what the first version of this did: it made ``player`` the
#: second-largest region of every unit atlas. The name is the trailing string.
OBJHEAD1_SHAPE_ID = 0x38
OBJHEAD1_NAME = 0x3C


def _trailing_name(body, offset):
    """A NUL-terminated, 4-byte-padded name at ``offset``, or ``""``.

    Rejects anything non-printable outright rather than decoding it: several of
    these fields are uninitialised in the shipped files, and a name of mojibake is
    worse than no name because it becomes its own bogus region.
    """
    if len(body) <= offset:
        return ""
    text = bytes(body[offset:]).split(b"\0")[0]
    if not text or not all(32 <= b < 127 for b in text):
        return ""
    return text.decode("ascii").strip()


def _object_names(root):
    """``shape_id_no`` -> object name, so a shape can be reported as a named part.

    ``OBJHEAD1+0x38`` matches ``SHPHEAD1+0x14``. That **id** match, not document
    order, is how an object finds its shape -- the two lists disagree in 86 of the
    563 shipped files, so pairing positionally attaches names to the wrong geometry
    in one file in seven.
    """
    names = {}
    for chunk in root.walk():
        if chunk.id != b"OBJHEAD1" or len(chunk.body) < OBJHEAD1_NAME:
            continue
        name = _trailing_name(chunk.body, OBJHEAD1_NAME)
        if not name:
            continue
        shape_id = int.from_bytes(
            bytes(chunk.body[OBJHEAD1_SHAPE_ID:OBJHEAD1_SHAPE_ID + 4]), "little", signed=True)
        names.setdefault(shape_id, name)
    return names


def _shape_id(chunk):
    """``SHPHEAD1+0x14``, the shape's own file-local id."""
    for kid in chunk.children:
        if kid.id == b"SHPHEAD1" and len(kid.body) >= 0x18:
            return int.from_bytes(bytes(kid.body[0x14:0x18]), "little", signed=True)
    return None


def collect(game_dir, wanted=None):
    """Walk every ``.rif`` and return ``{texture name: [Reference]}``.

    ``wanted`` restricts the walk to a set of lowercase forward-slash texture
    names, which is what makes a single-texture run cheap.

    **A ``_shadow`` file contributes its table but not its polygons.** Those meshes
    are never textured, so their polygons carry junk texture *and* UV indices and
    would poison every region -- but the ``BMPNAMES`` table itself is a real list of
    real files, and 17 textures in the shipped set are named by nothing else.
    Skipping shadow files outright therefore dropped 17 textures that may well be
    displayed; they come through here with an empty reference list, which segments
    into a single whole-sheet region, which is the honest answer for a texture no
    usable geometry points at.
    """
    refs = collections.defaultdict(list)
    for dirpath, _, names in os.walk(game_dir):
        for name in sorted(names):
            if not name.lower().endswith(".rif"):
                continue
            shadow = SHADOW_MARKER in name.lower()
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, game_dir).replace(os.sep, "/")
            try:
                root = rif.load(path)
            except Exception:  # noqa: BLE001
                continue
            table = next((c for c in root.walk() if c.id == b"BMPNAMES"), None)
            if table is None:
                continue
            try:
                _, entries = bmpnames.decode(table.body)
            except Exception:  # noqa: BLE001
                continue
            by_index = {e["index"]: e["name"].replace("\\", "/").lower() for e in entries}
            if wanted is not None and not (set(by_index.values()) & wanted):
                continue

            # Register every name the table holds before reading any geometry, so a
            # texture is *known* even when nothing samples it usably. Three separate
            # cases land here and all three were silently dropped by keying only off
            # polygons: a ``_shadow`` file's table (real names, junk polygon
            # indices), an entry no polygon references at all, and an entry whose
            # polygons carry no UV entry. Ten shipped textures in ordinary level
            # files -- ``mplay_zorro``'s ``building site 00``, ``tanker lift``'s
            # ``hull 22`` -- are in the last two groups. They come out as single
            # whole-sheet regions, which beats vanishing from the manifest.
            for tex in by_index.values():
                if wanted is None or tex in wanted:
                    refs[tex]  # noqa: B018 - defaultdict touch, deliberate
            if shadow:
                continue

            owners = _object_names(root)

            for shape in shp.iter_shapes(root):
                part = owners.get(_shape_id(shape.chunk))
                for poly in shape.polys:
                    tex = by_index.get(poly.texture_index)
                    if tex is None or (wanted is not None and tex not in wanted):
                        continue
                    uv = shape.uvs_for(poly)
                    if uv is None or len(uv) < 3:
                        continue
                    refs[tex].append(Reference(uv, shape, part, rel))
    return refs


def usable(uv, width, height):
    """Is this UV triangle inside the texture, allowing for a little tiling?"""
    us = [p[0] for p in uv]
    vs = [p[1] for p in uv]
    if min(us) < -UV_SLACK * width or max(us) > (1.0 + UV_SLACK) * width:
        return False
    if min(vs) < -UV_SLACK * height or max(vs) > (1.0 + UV_SLACK) * height:
        return False
    # A zero-area island is a polygon whose UV entry exists but says nothing --
    # three copies of (0,0) and a real entry are different on the wire and both
    # occur, so this is a content test rather than a missing-entry test.
    return (max(us) - min(us)) > 0.5 and (max(vs) - min(vs)) > 0.5


def table_names(rif_path):
    """The lowercase texture names in one ``.rif``'s ``BMPNAMES`` table.

    The whole table, not just the entries a polygon references -- 36 of the 563
    shipped files have no table at all, and among those that do, an entry nothing
    references is still a texture the file declares. See the note in ``collect``.
    """
    try:
        root = rif.load(rif_path)
    except Exception:  # noqa: BLE001
        return set()
    chunk = next((c for c in root.walk() if c.id == b"BMPNAMES"), None)
    if chunk is None:
        return set()
    try:
        _, entries = bmpnames.decode(chunk.body)
    except Exception:  # noqa: BLE001
        return set()
    return {e["name"].replace("\\", "/").lower() for e in entries}


def load_texture(game_dir, name, index=None):
    """A ``BMPNAMES`` name -> a decoded :class:`rim.Texture`, or ``None``."""
    index = index or rim.TextureIndex(os.path.join(game_dir, "Graphics"))
    path = index.resolve(name)
    if not path:
        return None
    return rim.load(path)
