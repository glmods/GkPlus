# The GkPlus documentation site

A theme-less [Hugo](https://gohugo.io/) site. Prose lives in `content/`, organised by the
four [Diátaxis](https://diataxis.fr/) quadrants; the templates in `layouts/` render it;
`static/` is served verbatim, which is where the fonts, the stylesheets and the two
generated API trees land.

## The design

The palette is measured off the game's own shipped manual (`<Gunlok>/html manual/`) rather
than invented: its `bgColor=#000000` and `bgcolor=#330033` plates, the cyan circuit lattice
in `purp_bg.jpg`, the white tracked caps of its figure callouts, and the violet frame around
every one of them. The headline face is **Squarish Sans CT** (Tim Larson, SIL OFL), a Bank
Gothic in the same lineage as the lettering on the Gunlok box, set in caps and tracked, and
used only for the brand, the navigation, eyebrows, `h1` and `h2`. Prose is **IBM Plex Sans**
and code is **IBM Plex Mono**, whose `0`/`O` and `1`/`l`/`I` stay distinct, which matters in
a repository whose pages are full of `0x004ae960`. All three are self-hosted from
`static/fonts/`: nothing is fetched from a font CDN, and the generated API trees get the same
faces as the prose.

The entry point at `/` is an index, not a project home page: a masthead, the three audience
routes (whose page counts are derived from each page's `audience:` front matter, so they
cannot drift), and then prose about how the documentation is organised. What GkPlus is and
why it is shaped like a graphics driver is an explanation page, and is linked as one.

Each quadrant carries a **channel colour** (tutorials green, how-to cyan, reference violet,
explanation amber) set from `data-section` on `<body>`. It is wayfinding rather than
decoration: a page says which of the four kinds it is before a word is read. The recurring
device is the **HUD plate**: four L-shaped corner brackets in the channel colour, drawn in
CSS, taken from the panel frames in the manual's own screenshots.

The one deliberate departure from the source: the manual's links are pure red. Red is kept
here for signals such as hover and alerts, because a documentation page carries hundreds of links and
a page of red text reads as a page of errors.

## Building it

Command 0 is not optional: **the webfonts are not in the repository** and the site does not
render without them. The rest produce the generated reference the site links to at `/api/cpp/`
and `/api/js/`.

Order matters in one place: **run the fold (command 5) after either generator and before
`hugo`.** The generators emit two self-contained little websites with their own chrome and no
way back; that command is what folds them into this one.

```bash
# 0. The webfonts, from the repository root, into docs/static/fonts/. Fetched and
#    subset rather than committed - see static/fonts/README.md. Needs fonttools
#    and brotli; --check reports what is missing without downloading anything.
python3 docs/tools/fetch-fonts.py

# 1. The site itself, from docs/. Output in docs/public/, which is the whole
#    website - Hugo copies static/ into it, so the API trees are already inside.
hugo --gc --minify

# 2. The C++ reference, from the repository root, into docs/static/api/cpp/.
#    Needs a configured build/ (it reads build/compile_commands.json) and clang-doc,
#    which ships with LLVM and is not on PATH by default on Windows. Use LLVM 22.1.0
#    or newer (the mustache HTML backend) - an older clang-doc's default backend bakes
#    this machine's own absolute output path into every page's sidebar links. See the
#    "LLVM is pinned" paragraph under Continuous integration.
clang-doc --executor=all-TUs build/compile_commands.json --format=html \
  --output=docs/static/api/cpp --project-name=GkPlus --public --ignore-map-errors \
  --extra-arg=-Xclang --extra-arg=-fparse-all-comments --extra-arg=-ferror-limit=0

# 3. The JavaScript reference, from the repository root, into docs/static/api/js/.
#    Both version pins are required; neither TypeScript nor TypeDoc is a dependency
#    of this repository.
npx -y -p typedoc@0.28 -p typescript@5.9 typedoc --options types/typedoc.json

# 4. Fold both generated trees into the site, from the repository root. Idempotent,
#    so it is safe to run when only one generator has been re-run - or twice.
#    It flattens clang-doc's api/cpp/html/ up to api/cpp/, drops its 4.4 MB JSON
#    model, and injects this site's header bar and stylesheets into all ~440
#    generated pages, so they read as part of the site rather than beside it.
python3 docs/tools/unify-api.py    # this is command 5; run it before `hugo`
```

Two more, worth running before publishing anywhere:

```bash
# Every internal link in the built site - the Hugo pages, the injected header bar
# and both generated trees at once. Exits non-zero on the first dead target.
python3 docs/tools/check-links.py

# Only for a deployment that is not at the root of a domain. Every link here is
# root-absolute, so a GitHub Pages *project* site at /<repo>/ needs the built
# output moved under that prefix. Idempotent; run after hugo, never on sources.
python3 docs/tools/rebase-links.py GkPlus
```

In PowerShell, replace the backslash continuations in command 2 with backticks or put the
invocation on one line.

`hugo server` from `docs/` serves the site at <http://localhost:1313> with live reload.

`hugo` does **not** empty `public/` first (`--gc` collects its cache, not the output tree), so a
file a previous build wrote stays there after it stops being generated. When a path changes (as
`api/cpp/html/` -> `api/cpp/` did), `rm -rf public` before rebuilding, or the old copy is still
served alongside the new one.

All the outputs (`public/`, `static/api/cpp/`, `static/api/js/`) and the fetched
`static/fonts/*.woff2` are gitignored by `docs/.gitignore`.

## Continuous integration

`.github/workflows/docs.yml` runs the whole sequence on any change to `docs/`, `types/` or
`examples/`: it fetches the fonts, generates the JavaScript reference, folds it in, builds
with a checksum-pinned Hugo, checks every internal link, and uploads the site as an artifact.
On `main` it also publishes to GitHub Pages, resolving the serving prefix from
`actions/configure-pages` and running `rebase-links.py` when the site is not at a root.

The **C++ reference** needs Windows, so it has a job of its own (`cpp-api`, `windows-latest`)
that hands the generated tree to the site job as an artifact. It is cheaper than it looks:
clang-doc only needs a CMake *configure*, because `compile_commands.json` is written at
generate time, and the two headers it would otherwise miss come from `src/gen-shaders.py` and
`src/gen-shader-abi.py`, which each take `--output`. So the job configures, runs those two
scripts, and runs clang-doc; it never compiles `d3d8.dll`. Installing the vcpkg dependencies
is the real cost, and vcpkg's GitHub Actions binary cache absorbs it after the first run.

**LLVM is pinned (`LLVM_VERSION`), installed fresh every run, never trusted off the runner
image.** clang-doc's older HTML backend bakes the build machine's own absolute output
directory into `index_json.js` as `RootPath`, and every sidebar link on every generated page
is built from it at runtime - on a GitHub-hosted runner that is `D:/a/GkPlus/GkPlus/...`, so
the published reference comes up with a sidebar that links nowhere real, and nothing in the
static HTML itself is wrong enough for `check-links.py` to catch it (the broken links exist
only as a JavaScript string, never as an `href`). This shipped once, silently, because the job
only checked that clang-doc wrote *some* pages. LLVM 22.1.0 uses the newer mustache backend
instead - real per-page relative links, no `RootPath` at all - and there is no flag to choose
between the two; it is purely a function of the LLVM version. The "Check it produced
something" step now also fails the job outright if `index_json.js` reappears, so a future pin
bump that regresses this is caught in CI rather than published.

That job is **best-effort**: it is `continue-on-error`, it runs only on `main` and manual
dispatch (Windows minutes bill at 2×, and a pull-request build is an artifact nobody
publishes), and the site job runs with `if: !cancelled()` regardless. When the tree does not
arrive, `tools/api-placeholder.py` writes a page at `/api/cpp/` saying so and giving the
command, which keeps all 118 links to it valid.

## Layout

```
docs/
  hugo.toml              site config - no theme, no external assets
  layouts/
    index.html           the entry index: masthead, audience routes, contents
    _default/
      baseof.html        the shell: header nav, section sidebar, footer
      list.html          section pages (renders the section's own _index.md)
      single.html        every other page
    partials/
      pagehead.html      eyebrow, title, audience tags, lede
      body.html          the page body, with a duplicate leading <h1> removed
  static/
    css/
      gkplus-tokens.css  palette, webfonts, .gk-lattice and .gk-plate - loaded
                         by BOTH the site and the generated API trees, which is
                         what stops the two drifting apart
      gkplus.css         the site's own typography, layout and chrome
      gkplus-api.css     the skin for clang-doc and TypeDoc: retunes each
                         generator's CSS variables to the palette above
    fonts/               Squarish Sans CT and IBM Plex, with their OFL licences
  tools/
    fetch-fonts.py       fetches and subsets the webfonts (command 0)
    unify-api.py         folds the generated API trees into the site (command 5)
    api-placeholder.py   stands in for a tree this build could not generate
    check-links.py       resolves every internal link in the built site
    rebase-links.py      moves the built output under a serving prefix
  content/
    _index.md            the compass: the four quadrants and who starts where
    tutorials/           one lesson per audience
    how-to/
      modding/           guides for players and mod authors
      development/       guides for people working on d3d8.dll
    reference/
      cpp/               clang-doc: the generator, its flags, the namespace map
      javascript/        TypeDoc: the "gk" module and the ImGui interface
      data/              hand-written - env vars, settings, mods, the CLIs
    explanation/         the design records
  static/
    api/cpp/             generated (command 2)
    api/js/              generated (command 3)
```

## Conventions

- **Every page carries front matter**: `title`, `description`, `weight` (ordering within its
  section), and `audience`, which is one or more of `player`, `mod-author`, `developer`.
- **Internal links are site-absolute and extensionless**: `/reference/data/settings-json/`,
  not a relative `.md` path. Links into the generated trees keep their `.html`.
- **The repository's `*_notes.md` files are cited by filename, not linked.** They are the
  reverse-engineering record, they live at the repository root, and they are outside this
  site.
- **No page writes down a hand-maintained count.** Where one would go, the page gives the
  command that re-derives it. See
  `content/explanation/why-nothing-here-writes-down-a-count.md`.

## What a new page needs

Put it in the quadrant matching the reader's need, not the subject: a lesson in
`tutorials/`, a goal in `how-to/`, a description in `reference/`, an argument in
`explanation/`. Then add its one-line entry to that section's `_index.md`, since the section
templates render the section's own prose rather than an automatic list, so a page nobody
links is reachable only from the sidebar.
