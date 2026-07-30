"""Which textures does one level use?

A level's asset closure is not a directory listing. ``level01.gls`` names its own
terrain ``.rif`` and then ``#include``s the ``.gsh`` headers for every unit, pickup
and structure that can appear in it; each of those names its own ``.rif``; and each
``.rif`` carries the ``BMPNAMES`` table that finally names the textures. Resolving
that chain is what makes "run the pipeline on level01" a well-defined request instead
of a guess.

The scrape is deliberately textual rather than going through the game's parser. The
parser is destructive global state and a single syntax error poisons it for the whole
process (see ``gls_system_notes.md``), which is far too much machinery for reading two
kinds of line. What is needed here is only ``#include "x.gsh"`` and ``file "y.rif"``,
and both are unambiguous once comments are stripped -- which they must be, because
``level01.gls`` comments out ``skorn.gsh``, ``pulsox.gsh`` and ``warflash.gsh``, and
treating those as included would pull in units the level never loads.
"""

import os
import re

INCLUDE_RE = re.compile(r'#include\s+"([^"]+)"', re.IGNORECASE)
FILE_RE = re.compile(r'\bfile\s+"([^"]+)"', re.IGNORECASE)
BITMAP_RE = re.compile(r'\bbitmap\s+"([^"]+)"', re.IGNORECASE)


def strip_comments(text):
    """Remove ``/* */`` blocks and ``//`` line comments.

    Load-bearing rather than tidiness: three of ``level01.gls``'s ``#include`` lines
    are commented out, and counting them would add whole unit sets to the closure.
    """
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def _read(path):
    # A .gls is ANSI, like everything else the engine reads as bytes.
    with open(path, encoding="cp1252", errors="replace") as fh:
        return strip_comments(fh.read())


def script_closure(scripts_dir, entry):
    """Every script ``entry`` pulls in, following ``#include`` transitively."""
    seen = {}
    pending = [entry]
    while pending:
        name = pending.pop()
        key = name.lower()
        if key in seen:
            continue
        path = os.path.join(scripts_dir, name)
        if not os.path.isfile(path):
            seen[key] = None
            continue
        text = _read(path)
        seen[key] = text
        for inc in INCLUDE_RE.findall(text):
            pending.append(inc.replace("\\", os.sep))
    return seen


def level_assets(game_dir, level):
    """``level01`` -> ``(rif paths, direct .rim names, missing)``.

    ``.rif`` names in a script are relative to the install root (``levels\\x.rif``,
    ``units\\y.RIF``) and live under ``RIF``; a ``bitmap`` field names a ``.rim``
    directly and is returned separately, because that one is the level's map image
    rather than a surface any polygon samples.
    """
    scripts = os.path.join(game_dir, "scripts")
    entry = level if level.lower().endswith(".gls") else level + ".gls"
    if not os.path.isfile(os.path.join(scripts, entry)):
        raise SystemExit("no such level script: %s" % os.path.join(scripts, entry))

    texts = script_closure(scripts, entry)
    rifs, bitmaps = set(), set()
    for text in texts.values():
        if text is None:
            continue
        for ref in FILE_RE.findall(text):
            if ref.lower().endswith(".rif"):
                rifs.add(ref.replace("\\", "/").lower())
        for ref in BITMAP_RE.findall(text):
            # The GLS escapes backslashes inconsistently ("bitmaps\\LEVEL01.rim").
            if ref.lower().replace("\\\\", "/").endswith(".rim"):
                bitmaps.add(ref.replace("\\\\", "/").replace("\\", "/").lower())

    index = _rif_index(os.path.join(game_dir, "RIF"))
    resolved, missing = {}, []
    for ref in sorted(rifs):
        path = index.get(ref) or index.get(os.path.basename(ref))
        if path:
            resolved[ref] = path
        else:
            missing.append(ref)
    return resolved, sorted(bitmaps), missing, [k for k, v in texts.items() if v is None]


def _rif_index(root):
    """Case-insensitive lookup of a script's ``.rif`` reference under ``RIF``."""
    out = {}
    for dirpath, _, names in os.walk(root):
        rel = os.path.relpath(dirpath, root).replace(os.sep, "/")
        prefix = "" if rel == "." else rel.lower() + "/"
        for name in names:
            if not name.lower().endswith(".rif"):
                continue
            full = os.path.join(dirpath, name)
            out.setdefault(prefix + name.lower(), full)
            out.setdefault(name.lower(), full)
    return out


def level_textures(game_dir, level):
    """``level01`` -> the set of ``BMPNAMES`` texture names its assets can show."""
    from . import assets

    resolved, bitmaps, missing, unread = level_assets(game_dir, level)
    textures = set()
    per_rif = {}
    for ref, path in sorted(resolved.items()):
        names = assets.table_names(path)
        per_rif[ref] = names
        textures |= names
    return {
        "textures": textures,
        "per_rif": per_rif,
        "rifs": resolved,
        "bitmaps": set(bitmaps),
        "missing_rifs": missing,
        "missing_scripts": unread,
    }
