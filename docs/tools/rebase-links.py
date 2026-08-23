#!/usr/bin/env python3
"""Move the built site under a serving prefix.

    python3 docs/tools/rebase-links.py /GkPlus [public-dir]

Every link on this site is root-absolute - `/how-to/modding/`, `/css/gkplus.css`,
`/fonts/ibm-plex-sans-var.woff2`. That is deliberate: the Markdown is written
that way so it reads correctly in the repository as well as on the site, and the
injected header bar has to work from inside two generated API trees whose page
depth varies. It also means the site only works served from the root of a
domain.

A GitHub Pages *project* site is not at a root: it is at
`https://<user>.github.io/<repo>/`. Hugo's `baseURL` cannot fix this on its own,
because most of these links come from Markdown and from a Python injector rather
than from a template, and neither passes through Hugo's URL rewriting.

So this rewrites them, once, over the built output - never over the sources.
Run it after `hugo` and before publishing. Idempotent: a path already under the
prefix is left alone, so running it twice does not produce `/GkPlus/GkPlus/`.

Not needed for a user/organisation Pages site or a custom domain, both of which
serve from the root.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# A root-absolute path, and not a protocol-relative `//host` URL. The trailing
# character class keeps the closing quote or bracket out of the match.
HTML_REF = re.compile(rb"""((?:href|src)=)(["']?)/(?!/)""")
CSS_REF = re.compile(rb"""(url\(\s*["']?)/(?!/)""")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("prefix", help="serving prefix, e.g. /GkPlus")
    ap.add_argument("public", nargs="?",
                    default=str(Path(__file__).resolve().parent.parent / "public"),
                    help="the built site (default: docs/public)")
    args = ap.parse_args()

    prefix = "/" + args.prefix.strip("/")
    if prefix == "/":
        print("rebase-links: empty prefix, nothing to do")
        return 0

    # A POSIX shell on Windows rewrites a leading-slash argument into a Windows
    # path before this process ever sees it, so `/GkPlus` arrives as
    # `C:/Program Files/Git/GkPlus` and silently corrupts every URL in the
    # build. Refuse rather than write it out.
    if ":" in prefix or "\\" in prefix or " " in prefix:
        print(f"rebase-links: {prefix!r} is not a URL prefix. A POSIX shell on "
              f"Windows converts a leading-slash argument to a path - pass it "
              f"without the slash ('GkPlus') or set MSYS_NO_PATHCONV=1.",
              file=sys.stderr)
        return 1

    pub = Path(args.public)
    if not pub.is_dir():
        print(f"rebase-links: {pub} does not exist - run hugo first", file=sys.stderr)
        return 1

    p = prefix.encode()
    # Already-prefixed paths must not be prefixed again - and the test has to
    # match the syntax being rewritten. An HTML-shaped test applied to a
    # stylesheet never fires, which is how a second run over the CSS produced
    # /prefix/prefix/fonts/.
    rules = {
        ".html": (HTML_REF, rb"\1\g<2>" + p + b"/",
                  re.compile(rb"""(?:href|src)=["']?""" + re.escape(p) + rb"/")),
        ".css": (CSS_REF, rb"\1" + p + b"/",
                 re.compile(rb"""url\(\s*["']?""" + re.escape(p) + rb"/")),
    }
    changed_files = 0
    changed_refs = 0

    for f in sorted(list(pub.rglob("*.html")) + list(pub.rglob("*.css"))):
        pattern, repl, done = rules[f.suffix]
        data = f.read_bytes()
        if done.search(data):
            continue
        out, n = pattern.subn(repl, data)
        if n:
            f.write_bytes(out)
            changed_files += 1
            changed_refs += n

    print(f"rebase-links: {changed_refs:,} references in {changed_files:,} files "
          f"moved under {prefix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
