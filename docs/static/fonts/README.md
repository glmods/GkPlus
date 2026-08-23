# Webfonts

**Nothing in this directory except this file is committed.** The `.woff2` and the two
licence texts are fetched and subset by `docs/tools/fetch-fonts.py`, and are gitignored:
they are third-party binaries with their own licences, they are reproducible byte-for-byte
from the sources below, and a repository is a poor place to keep 119 KB that a build step
can produce. The site does not render correctly without them, so the fetch runs before
`hugo` — in CI and in `docs/README.md`'s build sequence alike.

```bash
pip install fonttools brotli
python3 docs/tools/fetch-fonts.py           # fetch what is missing
python3 docs/tools/fetch-fonts.py --check   # report only; non-zero if any is missing
```

## What is fetched, and from where

| File | Face | Source |
|---|---|---|
| `squarish-sans-ct.woff2` | Squarish Sans CT Regular, subset to Latin + the punctuation this site sets | [opensourcedesign/fonts](https://github.com/opensourcedesign/fonts/tree/master/squarishSans), pinned at commit `8c591e77` |
| `ibm-plex-sans-var.woff2` | IBM Plex Sans, variable 100–700 | Google Fonts, `latin` subset |
| `ibm-plex-sans-var-italic.woff2` | IBM Plex Sans Italic, variable 100–700 | Google Fonts, `latin` subset |
| `ibm-plex-mono-400.woff2` | IBM Plex Mono Regular | Google Fonts, `latin` subset |
| `ibm-plex-mono-600.woff2` | IBM Plex Mono SemiBold | Google Fonts, `latin` subset |

## Licences

Both faces are under the **SIL Open Font License 1.1**, which is why they can be served
from this site at all. The full texts are fetched alongside the fonts as
`OFL-SquarishSansCT.txt` and `OFL-IBMPlex.txt`.

- **Squarish Sans CT** — Tim Larson, 2011. Written so the Aleph One project could ship a
  Bank Gothic legally, no version of which existed under a free licence. That lineage is
  the reason it is the headline face here: it is the same family of letterform as the
  wordmark on the Gunlok box and the tracked caps callouts in the game's own manual.
- **IBM Plex Sans / Mono** — IBM, under the OFL. Plex Mono is the code face because this
  repository's prose is dense with hex addresses like `0x004ae960`, and its `0`/`O` and
  `1`/`l`/`I` stay distinct at body size.

## Changing them

The `@font-face` declarations live in `docs/static/css/gkplus-tokens.css`, which is loaded
by both the Hugo templates and the two generated API trees — so a face changed there
changes every surface at once. The Squarish subset range is `SQUARISH_UNICODES` in the
fetch script; widen it if a page ever needs a glyph the display face does not carry.
