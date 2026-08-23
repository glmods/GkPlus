#!/usr/bin/env python3
"""Fetch and subset the webfonts the documentation site is set in.

    python3 docs/tools/fetch-fonts.py [--force] [--check]

Writes five `.woff2` files plus two licences into `docs/static/fonts/`. Those
files are **not committed** - they are third-party binaries with their own
licences, they are reproducible from here, and a repository is a poor place to
store 136 KB of font that a build step can fetch. `docs/static/fonts/README.md`
records the provenance and is committed; the binaries are gitignored.

Two faces, both under the SIL Open Font License:

  Squarish Sans CT   Tim Larson, from opensourcedesign/fonts at a pinned commit.
                     A Bank Gothic written so the Marathon community could
                     legally ship one - the same lineage as the lettering on the
                     Gunlok box and the callouts in the game's own manual, which
                     is why it is the headline face here. The upstream TTF
                     carries Greek and Hebrew and weighs 108 KB; subset to Latin
                     plus the punctuation this site actually uses it is 9 KB.

  IBM Plex           Sans (variable, 100-700, plus italic) and Mono 400/600,
                     taken as Google Fonts' pre-built `latin` subsets. Plex Mono
                     is the code face because this repository's prose is full of
                     hex - `0x004ae960` - and its 0/O and 1/l/I stay distinct.

Needs `fonttools` and `brotli` (`pip install fonttools brotli`); nothing else.
Idempotent: an existing file is left alone unless --force. With --check it
downloads nothing and just reports what is missing, exiting non-zero if any is -
which is what a build can use to fail early with a useful message.
"""

from __future__ import annotations

import argparse
import io
import re
import sys
import urllib.request
from pathlib import Path

FONTS = Path(__file__).resolve().parent.parent / "static" / "fonts"

# Pinned: this is the identity of the site, so it does not get to change under a
# rebuild. The font has not been touched upstream since 2015.
SQUARISH_COMMIT = "8c591e77ab27d79bf96ea2dda063604da5bf8840"
SQUARISH_BASE = f"https://raw.githubusercontent.com/opensourcedesign/fonts/{SQUARISH_COMMIT}/squarishSans"
SQUARISH_TTF = f"{SQUARISH_BASE}/Squarish%20Sans%20CT%20Regular.ttf"
SQUARISH_OFL = f"{SQUARISH_BASE}/OFL.txt"

# Latin, plus exactly the punctuation and symbols the site sets in this face -
# it is display type, so the range can be tight without ever showing a tofu.
SQUARISH_UNICODES = (
    "U+0020-007E,U+00A0-00FF,U+2010-2015,U+2018-201F,U+2020-2022,U+2026,"
    "U+2030,U+2039-203A,U+2044,U+20AC,U+2122,U+2190-2193,U+2212,U+25A0,"
    "U+25B6,U+00D7"
)

PLEX_CSS = (
    "https://fonts.googleapis.com/css2"
    "?family=IBM+Plex+Sans:ital,wght@0,400;0,600;1,400"
    "&family=IBM+Plex+Mono:wght@400;600"
    "&display=swap"
)
PLEX_LICENCE = "https://raw.githubusercontent.com/IBM/plex/master/LICENSE.txt"

# Google serves woff2 only to a browser-shaped request; with the default
# urllib agent it answers with TTF and the files come back four times the size.
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")

# What a complete docs/static/fonts/ contains. Also the --check manifest.
EXPECTED = [
    "squarish-sans-ct.woff2",
    "ibm-plex-sans-var.woff2",
    "ibm-plex-sans-var-italic.woff2",
    "ibm-plex-mono-400.woff2",
    "ibm-plex-mono-600.woff2",
    "OFL-SquarishSansCT.txt",
    "OFL-IBMPlex.txt",
]


def get(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def write(name: str, data: bytes) -> None:
    (FONTS / name).write_bytes(data)
    print(f"  {name:32} {len(data):>7,} bytes")


def fetch_squarish(force: bool) -> None:
    out = FONTS / "squarish-sans-ct.woff2"
    licence = FONTS / "OFL-SquarishSansCT.txt"
    if out.exists() and licence.exists() and not force:
        print(f"  {out.name:32} present")
        return

    from fontTools import subset  # imported here so --check needs no deps

    ttf = get(SQUARISH_TTF)
    print(f"  fetched Squarish Sans CT Regular.ttf ({len(ttf):,} bytes) "
          f"@ {SQUARISH_COMMIT[:12]}")

    opts = subset.Options()
    opts.flavor = "woff2"
    opts.desubroutinize = True
    opts.layout_features = ["*"]
    opts.name_IDs = ["*"]
    # The upstream TTF carries Apple layout tables the subsetter cannot rewrite;
    # dropping them is correct for a webfont and silences the warnings.
    opts.drop_tables += ["FFTM", "feat", "morx"]

    font = subset.load_font(io.BytesIO(ttf), opts)
    subsetter = subset.Subsetter(options=opts)
    subsetter.populate(unicodes=subset.parse_unicodes(SQUARISH_UNICODES))
    subsetter.subset(font)

    buf = io.BytesIO()
    subset.save_font(font, buf, opts)
    font.close()
    write(out.name, buf.getvalue())
    write(licence.name, get(SQUARISH_OFL))


def fetch_plex(force: bool) -> None:
    # Which @font-face block is which is decided by the declaration, not by the
    # order Google happens to emit them in, and only the `latin` block (the one
    # covering U+0000-00FF) is wanted - the rest are Cyrillic, Greek, Vietnamese.
    css = get(PLEX_CSS).decode("utf-8")
    wanted = {}
    for block in re.findall(r"@font-face\s*\{(.*?)\}", css, re.S):
        rng = re.search(r"unicode-range:\s*([^;]+);", block)
        if not rng or "U+0000-00FF" not in rng.group(1):
            continue
        family = re.search(r"font-family:\s*'([^']+)'", block).group(1)
        weight = re.search(r"font-weight:\s*(\d+)", block).group(1)
        style = re.search(r"font-style:\s*(\w+)", block).group(1)
        url = re.search(r"url\((https[^)]+)\)", block).group(1)
        wanted[(family, weight, style)] = url

    if not wanted:
        raise SystemExit("fetch-fonts: no latin @font-face blocks in the Google "
                         "Fonts CSS - the API response shape has changed.")

    # IBM Plex Sans upright is served as one variable file covering 100-700, so
    # the 400 and 600 declarations resolve to the same URL; downloading it once
    # is why the stylesheet declares a weight *range* rather than two faces.
    targets = {
        ("IBM Plex Sans", "400", "normal"): "ibm-plex-sans-var.woff2",
        ("IBM Plex Sans", "400", "italic"): "ibm-plex-sans-var-italic.woff2",
        ("IBM Plex Mono", "400", "normal"): "ibm-plex-mono-400.woff2",
        ("IBM Plex Mono", "600", "normal"): "ibm-plex-mono-600.woff2",
    }
    for key, name in targets.items():
        if (FONTS / name).exists() and not force:
            print(f"  {name:32} present")
            continue
        if key not in wanted:
            raise SystemExit(f"fetch-fonts: Google Fonts served no latin "
                             f"{key[0]} {key[1]} {key[2]}")
        data = get(wanted[key])
        if data[:4] != b"wOF2":
            raise SystemExit(f"fetch-fonts: {name} came back as {data[:4]!r}, "
                             f"not woff2 - the User-Agent was probably rejected.")
        write(name, data)

    licence = FONTS / "OFL-IBMPlex.txt"
    if not licence.exists() or force:
        write(licence.name, get(PLEX_LICENCE))
    else:
        print(f"  {licence.name:32} present")


def check() -> int:
    missing = [n for n in EXPECTED if not (FONTS / n).is_file()]
    for name in EXPECTED:
        p = FONTS / name
        print(f"  {name:32} {'missing' if name in missing else f'{p.stat().st_size:>7,} bytes'}")
    if missing:
        print(f"\n{len(missing)} of {len(EXPECTED)} missing. "
              f"Run: python3 docs/tools/fetch-fonts.py", file=sys.stderr)
        return 1
    print(f"\nall {len(EXPECTED)} present")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--force", action="store_true",
                    help="re-download and re-subset even if the files are there")
    ap.add_argument("--check", action="store_true",
                    help="report what is missing and exit non-zero if any is")
    args = ap.parse_args()

    FONTS.mkdir(parents=True, exist_ok=True)
    if args.check:
        return check()

    print(f"fonts -> {FONTS}")
    fetch_squarish(args.force)
    fetch_plex(args.force)
    total = sum((FONTS / n).stat().st_size for n in EXPECTED if (FONTS / n).is_file())
    print(f"done: {total:,} bytes in {FONTS.name}/")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
