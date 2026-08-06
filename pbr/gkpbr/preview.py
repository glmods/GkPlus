"""A generated map, on screen in the running game.

Every map this tool writes is a PNG that nothing has ever looked at in situ. The
gates in :mod:`metrics` say a height map is registered to its albedo; they cannot
say a normal map's relief points the right way, and no arithmetic here can. So
this module is the shortest honest path from ``out/maps/<slug>/<kind>.png`` to
pixels: pack the PNG as a ``.RIM`` and drop it into a mod so the engine loads it
**in place of the sheet it was derived from**. Then look.

**Why replacing the asset and not `render.material_override`.** The override is
the obvious candidate and it cannot do this: it re-points every draw sampling one
loaded image at *another loaded image*, keyed on a substring of the ``.rim`` path.
It has no way to introduce an image the engine never loaded, and a PNG on disk is
exactly that. A generated map has to enter through the asset loader, and
``src/Vfs`` is the only seam into it. Two consequences worth having anyway:

- the swap works under **every** renderer, including ``GKPLUS_RENDERER=d3d9``,
  which is the one ``PrintWindow`` can screenshot (``utils/rendertest``);
- it is what a real consumer would do, so what is on screen is what a mod
  shipping these maps would show.

``material_override`` is still useful *after* this, and only after: with the mod
installed, both sheets are loaded and an override can A/B them inside one session
under Vulkan. The honest A/B is still mod-in against mod-out.

**The formats are not a free choice**, and one of them is measured here rather
than assumed -- see :data:`DEFAULT_FORMATS` and ``pbr/README.md``.
"""

import os
import shutil
import subprocess

#: Where a mod's files sit relative to the install, mirroring the game's own tree
#: (``mod_loading_notes.md``). ``Graphics`` is also the root a ``BMPNAMES`` name is
#: relative to, which is why a manifest key needs nothing done to it but a join.
TEXTURE_DIR = "Graphics"

#: The mod this writes, under ``<Gunlok>\gkplus\mods``. Named for what it is so a
#: leftover one is recognisable: a mod silently changing what the game draws is
#: the bug commit 6655629 spent a session chasing.
DEFAULT_MOD = "gkpbr-preview"

#: Which ``.RIM`` encoding each map should go through, and it is a real decision.
#:
#: Gunlok accepts **DXT1 and DXT3 only** and drops any other fourcc silently
#: (``rif_chunk_format.md``), so the S3TC choice is between those two -- and for a
#: map with no alpha they are *the same picture*: measured on this pipeline's own
#: normal map for ``ground/city ruins ground 1_a``, DXT1 and DXT3 came back
#: byte-identical in RGB, because the two share a block encoding and only the
#: alpha differs. So "use DXT3, it is higher quality" buys nothing at twice the
#: size, and the interesting comparison is S3TC against no S3TC:
#:
#: | encoding | normal-vector error, mean / p99 / max |
#: |---|---|
#: | DXT1 and DXT3, identical | 2.53 deg / 8.27 / 17.25 |
#: | ``body`` on a 16-bit surface (R5G6B5) | 1.15 deg / 1.89 / 2.33 |
#:
#: A normal map is a field of unit vectors, and S3TC's error is not spread evenly
#: -- it is concentrated in whichever 4x4 blocks straddle a gradient, which is
#: exactly where the relief is. Hence ``body`` for ``normal``. It is lossless on
#: disk and *not* lossless on screen: an uncompressed image lands on whatever
#: surface ``ChooseSurfaceFormatForImage`` picks, and the 32-bit candidates are
#: gated on ``Use32BitTextures``, which is 0 in a retail build -- so the table's
#: second row assumes R5G6B5 and is the pessimistic reading.
#:
#: Everything else is DXT1: a roughness or height map is one channel of smooth
#: data and 1.03/255 mean error on it is nothing, at a quarter of ``body``'s size.
DEFAULT_FORMATS = {
    "normal": "body",
    "color": "dxt1",
    "roughness": "dxt1",
    "metallic": "dxt1",
    "height": "dxt1",
    "emissive": "dxt1",
}

#: Built by ``cmake --build build``; overridable because a Release build puts it
#: somewhere else and a caller may have one on PATH.
RIMUTIL_RELPATH = os.path.join("build", "utils", "rimutil", "Debug", "rimutil.exe")


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def find_rimutil(explicit=None):
    """The ``rimutil`` executable, or ``None``.

    Returned rather than raised so a caller can report the whole plan and then say
    what is missing, instead of dying on the first line of it.
    """
    for cand in (explicit, os.environ.get("GKPBR_RIMUTIL"),
                 os.path.join(repo_root(), RIMUTIL_RELPATH),
                 shutil.which("rimutil")):
        if cand and os.path.isfile(cand):
            return cand
    return None


def mod_relative_path(texture, index=None):
    """A manifest key -> the path a mod must use, e.g. ``Graphics/Ground/x.RIM``.

    The VFS folds case (``mod_loading_notes.md``: PhysicsFS is case-sensitive
    inside an archive, so ``src/Vfs`` keeps a lowercased index), so any casing
    resolves. The **shipped** casing is used anyway when a ``TextureIndex`` is
    available, because a mod directory that looks exactly like the install is one
    less thing to wonder about when it does not work -- and because a mod packed
    into a ``.zip`` later would then already be right.
    """
    rel = texture.replace("\\", "/").lstrip("/")
    if index is not None:
        on_disk = index.resolve(texture)
        if on_disk:
            rel = os.path.relpath(on_disk, index.root).replace(os.sep, "/")
    return TEXTURE_DIR + "/" + rel


def target_path(game_dir, texture, mod=DEFAULT_MOD, index=None):
    return os.path.join(game_dir, "gkplus", "mods", mod,
                        *mod_relative_path(texture, index).split("/"))


def format_for(kind, explicit=None):
    return explicit or DEFAULT_FORMATS.get(kind, "dxt1")


def pack(rimutil, png_path, rim_path, fmt):
    """``rimutil compress`` into place. Returns its stdout, raises on failure."""
    os.makedirs(os.path.dirname(rim_path), exist_ok=True)
    proc = subprocess.run([rimutil, "compress", png_path, rim_path,
                           "--format", fmt],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError("rimutil compress failed (%d): %s"
                           % (proc.returncode, (proc.stderr or proc.stdout).strip()))
    return (proc.stdout or "").strip()


def remove(game_dir, mod=DEFAULT_MOD):
    """Delete the whole preview mod. Returns what was removed, or ``None``.

    Cleanup is a first-class command rather than an instruction in a README
    because the failure it prevents is silent: a leftover mod goes on replacing
    an asset in every later session, and the game looks *fine*.
    """
    path = os.path.join(game_dir, "gkplus", "mods", mod)
    if not os.path.isdir(path):
        return None
    shutil.rmtree(path)
    return path


#: What to paste into the REPL once the mod is in place. Printed rather than run:
#: this module has no business owning a socket, and the operator is going to be
#: driving `utils/rendertest` anyway.
REPL_HINT = """\
  levels.start({script: "level02.gls", console: "level02.gcs"})
  mods.served                       // the count must go up by one
  mods.recent                       // and name %s
"""
