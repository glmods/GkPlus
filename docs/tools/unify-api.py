#!/usr/bin/env python3
"""Fold the two generated API trees into the docs site.

clang-doc and TypeDoc each emit a self-contained little website. Dropped into
Hugo's `static/` they are *served* from the same origin as the hand-written
pages, but they are not *part* of the same site: they carry their own chrome,
they offer no way back to the tutorials or the how-to guides, and clang-doc
buries its entry point one directory deeper than its own URL suggests.

This script is the seam. Run it after either generator and before `hugo`:

    python3 docs/tools/unify-api.py

It does four things, all of them idempotent - running it twice changes nothing
the second time, which matters because the generators are re-run far more often
than this is:

  1. Flattens `api/cpp/html/` up into `api/cpp/`, so the C++ reference lives at
     `/api/cpp/` like the JavaScript one lives at `/api/js/`. clang-doc's
     internal links are relative and the whole directory moves together, so
     they keep resolving.
  2. Deletes `api/cpp/json/`. That is clang-doc's intermediate model, 4.4 MB of
     it, more than half the tree - useful to a tool, meaningless to a reader,
     and there is no reason to publish it. It also strips the Google Fonts
     `@import` out of clang-doc's stylesheet, since the site serves its own
     faces and an offline reader should not block on a font they will not get.
  3. Injects the site's header bar into every generated page, so a reader who
     lands deep in the API reference from a search engine can still reach the
     rest of the documentation.
  4. Injects `/css/gkplus-tokens.css` and `/css/gkplus-api.css`, and tags each
     `<body>` with `gkplus-cpp` or `gkplus-js`. Both generators drive their
     appearance from CSS custom properties, so the skin retunes those to the
     site's palette rather than fighting their layouts. The tokens file is the
     same one the Hugo templates load, which is what stops the palette and the
     webfonts drifting between the three surfaces.
"""

from __future__ import annotations

import re
import shutil
import sys
from pathlib import Path

# Bumping this re-injects the banner into pages that already carry an older one,
# which is what lets the bar change without regenerating the API trees first.
BANNER_VERSION = "3"
MARKER = f"<!-- gkplus-site-banner v{BANNER_VERSION} -->"
ANY_MARKER = re.compile(r"<!-- gkplus-site-banner v\d+ -->.*?<!-- /gkplus-site-banner -->",
                        re.DOTALL)
ANY_STYLE = re.compile(
    r"[ \t]*<(?:style|link)[^>]*data-gkplus-banner[^>]*>(?:.*?</style>)?\n?",
    re.DOTALL)

BODY_OPEN = re.compile(r"<body\b[^>]*>", re.IGNORECASE)
HEAD_CLOSE = re.compile(r"</head>", re.IGNORECASE)

# Two stylesheets rather than an inline block: the tokens file is the very one
# the Hugo templates load, so the palette and the webfonts cannot drift between
# the site and the generated trees, and the API skin retunes each generator's
# own CSS variables. Injected last in <head>, after the generator's own styles,
# which is what lets a plain :root override win.
BANNER_STYLE = (
    '<link rel="stylesheet" href="/css/gkplus-tokens.css" data-gkplus-banner>\n'
    '<link rel="stylesheet" href="/css/gkplus-api.css" data-gkplus-banner>'
)

def banner_html(where: str) -> str:
    """The bar itself. `where` names which generated set the reader is inside."""
    return (
        f'{MARKER}\n'
        '<div class="gkplus-banner">\n'
        '  <a class="gkplus-brand" href="/">GkPlus</a>\n'
        '  <nav>'
        '<a href="/tutorials/">Tutorials</a>'
        '<a href="/how-to/">How-to</a>'
        '<a href="/reference/">Reference</a>'
        '<a href="/explanation/">Explanation</a>'
        '</nav>\n'
        f'  <span class="gkplus-where">{where}</span>\n'
        '</div>\n'
        '<!-- /gkplus-site-banner -->'
    )


def flatten_cpp(cpp: Path) -> list[str]:
    """Move `cpp/html/*` up to `cpp/`, and drop the JSON model."""
    notes = []

    html = cpp / "html"
    if html.is_dir():
        for item in list(html.iterdir()):
            target = cpp / item.name
            if target.exists():
                # A previous run already flattened; the generator then rebuilt
                # `html/`. The fresh copy wins.
                if target.is_dir():
                    shutil.rmtree(target)
                else:
                    target.unlink()
            shutil.move(str(item), str(target))
        html.rmdir()
        notes.append("flattened api/cpp/html/ -> api/cpp/")

    # clang-doc's stylesheet pulls Inter from fonts.googleapis.com. The site
    # ships its own faces, and an offline reader should not get a blocking
    # request for one it will not use.
    css = cpp / "clang-doc-mustache.css"
    if css.is_file():
        body = css.read_text(encoding="utf-8", errors="replace")
        stripped = re.sub(r'@import\s+"https://fonts\.googleapis\.com[^"]*";?\s*', "", body)
        if stripped != body:
            css.write_text(stripped, encoding="utf-8")
            notes.append("removed the Google Fonts @import from clang-doc's stylesheet")

    json_dir = cpp / "json"
    if json_dir.is_dir():
        shutil.rmtree(json_dir)
        notes.append("removed api/cpp/json/ (clang-doc's intermediate model)")

    return notes


def inject(path: Path, where: str, body_class: str) -> bool:
    """Put the banner and the stylesheets into one page.

    Returns True if the file changed. `body_class` is what lets one skin
    retune two different generators without their rules colliding."""
    try:
        text = path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        return False

    if MARKER in text and "data-gkplus-banner" in text:
        return False

    # Strip any older banner before inserting the current one, so a version
    # bump replaces rather than stacks.
    text = ANY_MARKER.sub("", text)
    text = ANY_STYLE.sub("", text)

    head = HEAD_CLOSE.search(text)
    if head:
        text = text[: head.start()] + BANNER_STYLE + "\n" + text[head.start() :]

    body = BODY_OPEN.search(text)
    if not body:
        return False

    # Tag the body so gkplus-api.css can scope itself to one generator, without
    # disturbing any class the generator put there.
    opening = body.group(0)
    if body_class not in opening:
        if 'class="' in opening:
            opening = opening.replace('class="', f'class="{body_class} ', 1)
        else:
            opening = opening[:-1] + f' class="{body_class}">'
    text = text[: body.start()] + opening + "\n" + banner_html(where) + text[body.end() :]

    path.write_text(text, encoding="utf-8")
    return True


def main() -> int:
    root = Path(__file__).resolve().parent.parent  # docs/
    api = root / "static" / "api"
    if not api.is_dir():
        print(f"nothing to do: {api} does not exist - run the generators first",
              file=sys.stderr)
        return 1

    for note in flatten_cpp(api / "cpp"):
        print(note)

    trees = [
        (api / "cpp", "C++ reference &middot; clang-doc", "gkplus-cpp"),
        (api / "js", "JavaScript reference &middot; TypeDoc", "gkplus-js"),
    ]

    total = 0
    for tree, where, body_class in trees:
        if not tree.is_dir():
            print(f"skipped {tree.relative_to(root)}: not generated")
            continue
        changed = sum(1 for page in sorted(tree.rglob("*.html"))
                      if inject(page, where, body_class))
        pages = sum(1 for _ in tree.rglob("*.html"))
        total += changed
        print(f"{tree.relative_to(root)}: banner on {changed} of {pages} pages"
              f"{' (already current)' if changed == 0 else ''}")

    print(f"done: {total} page(s) rewritten")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
