#!/usr/bin/env python3
"""Check every internal link in the built site.

    python3 docs/tools/check-links.py [public-dir] [--prefix /GkPlus]

Runs over `docs/public` after `hugo`, so it sees the whole site at once: the
Hugo pages, the injected header bar, and both generated API trees. That breadth
is the point - the three surfaces are built by three different tools and link
into each other, and nothing else checks the joins.

It resolves every root-absolute `href`/`src` in the HTML plus every `url()` in
the CSS against the built tree, and exits non-zero if any does not land on a
real file. It deliberately does NOT check external URLs: those fail for reasons
that have nothing to do with this repository, and a docs build that goes red
because someone else's server is down teaches everyone to ignore it.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Attribute values may be quoted, single-quoted, or bare - Hugo's --minify
# strips the quotes, which is exactly the case a naive pattern misses.
HREF = re.compile(rb"""(?:href|src)=(?:"([^"]+)"|'([^']+)'|([^\s>]+))""")
CSS_URL = re.compile(r"""url\(\s*['"]?([^'")]+)['"]?\s*\)""")


def resolve(pub: Path, target: str) -> bool:
    p = pub / target.lstrip("/")
    if p.is_dir():
        p = p / "index.html"
    return p.exists()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("public", nargs="?",
                    default=str(Path(__file__).resolve().parent.parent / "public"),
                    help="the built site (default: docs/public)")
    ap.add_argument("--prefix", default="",
                    help="serving prefix to strip first, e.g. /GkPlus - use "
                         "after rebase-links.py has run")
    args = ap.parse_args()

    pub = Path(args.public)
    if not pub.is_dir():
        print(f"check-links: {pub} does not exist - run hugo first", file=sys.stderr)
        return 1

    # Normalised the same way rebase-links.py normalises it, including the
    # same refusal: a POSIX shell on Windows turns a leading-slash argument
    # into a Windows path, and the resulting run reports every link broken.
    prefix = ("/" + args.prefix.strip("/")) if args.prefix.strip("/") else ""
    if ":" in prefix or "\\" in prefix or " " in prefix:
        print(f"check-links: {prefix!r} is not a URL prefix. A POSIX shell on "
              f"Windows converts a leading-slash argument to a path - pass it "
              f"without the slash ('GkPlus') or set MSYS_NO_PATHCONV=1.",
              file=sys.stderr)
        return 1
    broken: dict[str, list[str]] = {}
    checked = 0

    for f in sorted(pub.rglob("*.html")):
        for m in HREF.finditer(f.read_bytes()):
            raw = (m.group(1) or m.group(2) or m.group(3) or b"")
            raw = raw.decode("utf-8", "replace")
            if not raw.startswith("/") or raw.startswith("//"):
                continue
            checked += 1
            target = raw.split("#")[0].split("?")[0]
            if target in ("", "/"):
                continue
            if prefix:
                if not target.startswith(prefix + "/"):
                    broken.setdefault(f"{target} (missing prefix {prefix})", []).append(
                        str(f.relative_to(pub)))
                    continue
                target = target[len(prefix):]
            if not resolve(pub, target):
                broken.setdefault(target, []).append(str(f.relative_to(pub)))

    # Fonts are referenced only from CSS, so an HTML-only sweep would miss a
    # missing one entirely - and a missing font is invisible until you look.
    for f in sorted(pub.rglob("*.css")):
        for raw in CSS_URL.findall(f.read_text(encoding="utf-8", errors="replace")):
            if not raw.startswith("/") or raw.startswith("//"):
                continue
            checked += 1
            target = raw.split("#")[0].split("?")[0]
            if prefix and target.startswith(prefix + "/"):
                target = target[len(prefix):]
            if not resolve(pub, target):
                broken.setdefault(target, []).append(str(f.relative_to(pub)))

    pages = sum(1 for _ in pub.rglob("*.html"))
    print(f"check-links: {checked:,} internal references across {pages:,} pages")

    if broken:
        print(f"\n{len(broken)} broken target(s):", file=sys.stderr)
        for target, sources in sorted(broken.items()):
            print(f"  {target}", file=sys.stderr)
            for s in sources[:3]:
                print(f"      from {s}", file=sys.stderr)
            if len(sources) > 3:
                print(f"      ... and {len(sources) - 3} more", file=sys.stderr)
        return 1

    print("all resolve")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
