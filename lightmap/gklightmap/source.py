"""Finding the install, and turning one ``.RIM`` into an RGB array.

The decode is the addon's, not a reimplementation: ``blender/io_scene_rif/rim.py``
is pure Python, imports no ``bpy`` and already reads both families the game ships
(DXT1/DXT3 and palettized ``BODY``). ``pbr/gkpbr/assets.py`` reaches for it the same
way and for the same reason, which is also why the boundary is worth stating: an
addon change can break this silently. There is no test here that would catch it --
``pbr/tests/test_addon_boundary.py`` is the one that exists, and it covers the two
entry points this file uses (``rim.load`` and ``rim.TextureIndex``).
"""

import os
import sys

_ADDON = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "blender", "io_scene_rif")
if _ADDON not in sys.path:
    sys.path.insert(0, _ADDON)

import rim  # noqa: E402

import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

#: Where the ``.RIM`` files live, and the root a ``BMPNAMES`` name is relative to.
TEXTURE_DIR = "Graphics"


def find_install(start=None):
    """Gunlok's directory, from ``GUNLOK_DIR`` or the Steam registry.

    Copied from :mod:`gkpbr.assets` rather than imported: ``pbr/`` is a separate uv
    project with its own dependency set, and reaching across would make this tool
    unrunnable whenever that one is.
    """
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


def resolve(texture, game_dir=None):
    """``(path on disk, name relative to Graphics)`` for whatever the user typed.

    Either spelling works: a path to a file (``C:\\...\\Graphics\\Ground\\cracks.RIM``,
    or a relative one), or a ``BMPNAMES``-style name (``ground/cracks.rim``,
    ``Ground\\cracks.RIM``) resolved case-insensitively under ``<Gunlok>\\Graphics``.

    The **relative** name is what matters downstream: it is the stem the engine will
    look a companion up by, so it decides the output file's name.
    """
    if os.path.isfile(texture):
        path = os.path.abspath(texture)
        root = os.path.join(game_dir or find_install(), TEXTURE_DIR)
        rel = os.path.relpath(path, root).replace(os.sep, "/")
        if rel.startswith(".."):
            # Outside the install: the engine could never name it, so its stem is
            # just the file's own name. Useful for a texture that is being authored.
            rel = os.path.basename(path)
        return path, rel
    root = os.path.join(game_dir or find_install(), TEXTURE_DIR)
    index = rim.TextureIndex(root)
    path = index.resolve(texture)
    if not path:
        raise SystemExit("no such texture: %s (looked under %s)" % (texture, root))
    return path, os.path.relpath(path, root).replace(os.sep, "/")


def stem(rel):
    """``Ground/cracks.RIM`` -> ``ground/cracks``, the engine's own lookup key.

    Lowercased because every consumer of it is case-insensitive (``src/Vfs`` by
    construction, Windows' filesystem by nature) and a stable spelling makes the
    output directory stable too.
    """
    out = rel.replace("\\", "/").lower()
    dot = out.rfind(".")
    slash = out.rfind("/")
    return out[:dot] if dot > slash else out


#: Trailing characters Windows silently drops from a path component.
#:
#: **This is not defensive tidying.** Two shipped textures are named with a trailing
#: space -- ``Ground\\city wall conc transitions .RIM`` and
#: ``Ground\\outskirts robot ring .RIM`` -- and the failure mode is nasty:
#: ``os.makedirs("out/x ")`` *succeeds*, creating a directory called ``x``, and the
#: very next ``open("out/x /albedo.png")`` raises ``FileNotFoundError``. So it looks
#: like a missing file rather than a name the filesystem refused to store.
_UNSTORABLE_TAIL = " ."


def slug(rel):
    """``Ground/cracks.RIM`` -> ``ground__cracks``, a flat directory name.

    Only the *local scratch* name is sanitised, never :func:`stem`. The stem is the
    engine's own lookup key -- ``src/VkLighting.cpp`` builds ``graphics/<stem>
    lighting.dds`` out of it -- so trimming there would point the output at a file
    the engine never asks for. The installed name keeps the space, which is legal
    because it lands in the *middle* of ``outskirts robot ring  lighting.dds``;
    Windows only objects at the end of a component.
    """
    return stem(rel).replace("/", "__").rstrip(_UNSTORABLE_TAIL)


def load_albedo(path):
    """A ``.RIM`` (or a PNG/JPEG) -> ``HxWx3`` uint8, row 0 at the top.

    Alpha is dropped rather than composited. A lighting map has no use for it, and
    the model is being asked about the *surface*, which is what the RGB holds even
    under a fully transparent texel.
    """
    if path.lower().endswith(".rim"):
        tex = rim.load(path)
        arr = np.frombuffer(tex.rgba, dtype=np.uint8).reshape(tex.height, tex.width, 4)
        return np.ascontiguousarray(arr[..., :3])
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)
