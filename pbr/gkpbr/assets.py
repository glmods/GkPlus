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

#: **The shipped geometry is exactly ``<Gunlok>\RIF``**, and the walk is rooted there
#: rather than at the install root on purpose. All 563 shipped ``.rif`` live under it
#: (47 ``Levels``, 348 ``Objects``, 163 ``Units``, 5 ``User Interface``); walking the
#: install instead also picked up ``gkplus\mods\*\RIF\**\*.rif``, so the manifest
#: depended on which mods happened to be installed on the machine that built it. It
#: cost nothing measurable here only by luck: the one ``.rif`` in this machine's
#: leftover ``rimutil-body-test`` mod carries no ``BMPNAMES`` table, and the loop
#: below skips a file without one. A mod that replaced a level would have landed in
#: the manifest. A mod is content *about* the shipped set, not part of it.
SHIPPED_RIF_DIR = "RIF"

#: Where the ``.RIM`` files live, and the root a ``BMPNAMES`` name is relative to.
TEXTURE_DIR = "Graphics"


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


def collect(game_dir, wanted=None, rif_root=None):
    """Walk every shipped ``.rif`` and return ``{texture name: [Reference]}``.

    ``wanted`` restricts the walk to a set of lowercase forward-slash texture
    names, which is what makes a single-texture run cheap. ``rif_root`` narrows the
    walk to a subtree, which is what makes a smoke test cheap; it defaults to
    :data:`SHIPPED_RIF_DIR` under ``game_dir`` and is spelled out by the caller
    rather than inferred, because the whole point of the default is that it is not
    "whatever ``.rif`` files happen to be under the install".

    **A ``_shadow`` file contributes its table but not its polygons.** Those meshes
    are never textured, so their polygons carry junk texture *and* UV indices and
    would poison every region -- but the ``BMPNAMES`` table itself is a real list of
    real files, and **7** textures in the shipped set are named by nothing else.
    Skipping shadow files outright therefore dropped 7 textures that may well be
    displayed; they come through here with an empty reference list, which segments
    into a single whole-sheet region, which is the honest answer for a texture no
    usable geometry points at.

    The walk is rooted at ``RIF`` rather than at the install -- see
    :data:`SHIPPED_RIF_DIR`, and note that ``rif`` paths in the returned references
    stay relative to ``game_dir`` so they read the same as before.
    """
    refs = collections.defaultdict(list)
    for dirpath, _, names in os.walk(rif_root or os.path.join(game_dir, SHIPPED_RIF_DIR)):
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
            # texture is *known* even when nothing samples it usably. 17 shipped
            # textures land here and keying only off polygons dropped all of them:
            # 7 are named only by a ``_shadow`` file's table (real names, junk
            # polygon indices) and 10 carry an index no polygon in an ordinary file
            # ever names -- ``mplay_zorro``'s ``building site 00``, ``tanker lift``'s
            # ``hull 22``. They come out as single whole-sheet regions, which beats
            # vanishing from the manifest.
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


#: Routes by which the engine reaches a ``.RIM`` that no ``BMPNAMES`` table names.
#: Both are measured by searching for the file's own path, so neither is a guess
#: about intent -- but they are also the *only* two routes there are, because
#: ``BMPNAMES`` is the sole name/index binding in the RIF format (``SHPTEXFN`` is
#: registered and appears in no shipped file, see ``rif_chunk_format.md``). A
#: texture no table names therefore cannot be bound by any polygon of any shipped
#: ``.rif``: it is reachable only as front-end art, and only if something spells its
#: name out.
ROUTE_SCRIPT = "a script under scripts\\ names it"
ROUTE_EXE = "gl.exe holds its name as a literal"
ROUTE_NONE = "nothing in the install names it"


def textures_on_disk(game_dir):
    """Every shipped ``.RIM``, as a lowercase forward-slash path under ``Graphics``."""
    root = os.path.join(game_dir, TEXTURE_DIR)
    out = []
    for dirpath, _, names in os.walk(root):
        for name in names:
            if name.lower().endswith(".rim"):
                rel = os.path.relpath(os.path.join(dirpath, name), root)
                out.append(rel.replace(os.sep, "/").lower())
    return sorted(out)


def _folded(blob):
    """Lowercase, and collapse every path spelling onto a single backslash.

    Both directions matter. ``gl.exe`` writes ``bitmaps\\lava.rim``; a ``.gls``
    writes ``bitmap "bitmaps\\\\LEVEL02.rim"`` with the backslash **doubled**, and
    testing for the single-backslash form alone found **2 of the 27** files the
    scripts name -- every level-map bitmap read as "nothing names this" while
    ``Maze.gls`` and its fifteen siblings named it plainly.
    """
    return blob.lower().replace(b"/", b"\\").replace(b"\\\\", b"\\")


def _mentions(blob, rel):
    needle = rel.replace("/", "\\").encode()
    if b"\\" in needle:  # directory-qualified: no shorter name can end in it
        return needle in blob
    # A bare ``multi buttons.rim`` at the texture root needs a left boundary, or it
    # would also match the tail of any longer path.
    start = 0
    while True:
        i = blob.find(needle, start)
        if i < 0:
            return False
        if i == 0 or not (0x20 <= blob[i - 1] < 0x7F) or blob[i - 1:i] in (b'"', b"'"):
            return True
        start = i + 1


def name_routes(game_dir, rels):
    """``{relative .RIM path: one of the ROUTE_* constants}``.

    Answers "if no geometry names this texture, what does?" by searching the two
    places a name can be spelled out: ``gl.exe`` (26 ``.rim`` literals, the front
    end, the fonts, the HUD and the liquid-surface effects) and everything under
    ``scripts\\`` (the ``bitmap`` field that gives a level its map image, plus the
    briefing and credits screens).

    **This route is not a synonym for "front-end art"**, which is what it looked like
    until the running game was asked. Six of the 17 files it accounts for are drawn,
    and three of those -- ``bitmaps/lava``, ``oil`` and ``swamp`` -- are ordinary world
    surfaces laid down by the ``LAVA``/``OIL``/``SWAMP`` console commands, which supply
    the texture name from inside the engine and so need no ``BMPNAMES`` entry. See
    :mod:`gkpbr.renderstate` and the README.
    """
    haystacks = []
    exe = os.path.join(game_dir, "gl.exe")
    if os.path.isfile(exe):
        with open(exe, "rb") as fh:
            haystacks.append((ROUTE_EXE, _folded(fh.read())))

    chunks = []
    for dirpath, _, names in os.walk(os.path.join(game_dir, "scripts")):
        for name in sorted(names):
            try:
                with open(os.path.join(dirpath, name), "rb") as fh:
                    chunks.append(_folded(fh.read()))
            except OSError:
                continue
    if chunks:
        haystacks.insert(0, (ROUTE_SCRIPT, b"\n".join(chunks)))

    out = {}
    for rel in rels:
        out[rel] = next((route for route, blob in haystacks if _mentions(blob, rel)),
                        ROUTE_NONE)
    return out


def load_texture(game_dir, name, index=None):
    """A ``BMPNAMES`` name -> a decoded :class:`rim.Texture`, or ``None``."""
    index = index or rim.TextureIndex(os.path.join(game_dir, TEXTURE_DIR))
    path = index.resolve(name)
    if not path:
        return None
    return rim.load(path)
