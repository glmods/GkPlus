# PBR maps from Gunlok's `.RIM` textures

Generates `color` / `roughness` / `metallic` / `normal` / `emissive` (+ `height`) map
sets for the textures Gunlok's shipped geometry actually uses. Output is plain PNGs
plus a manifest, so the consumer — the Blender addon's materials, a real-time PBR
path, or anything else — binds to it later without this tool being in the loop.

This is a **uv project with dependencies**, deliberately separate from
`blender/io_scene_rif`, which must stay importable by Blender's own interpreter with
nothing installed. The generator imports the addon's pure-Python decoders; nothing
in the addon imports the generator.

```
uv run python -m gkpbr.cli inventory      # what is used, and its regions
uv run python -m gkpbr.cli probe          # does the image model hold registration?
                                          #   (run this first; see the probe section)
uv run python -m gkpbr.cli classify       # stage 1: material JSON per texture
uv run python -m gkpbr.cli maps           # stages 2-4: the PNGs
```

`GUNLOK_DIR` overrides install detection, `GKPBR_OUT` the output directory,
`GEMINI_API_KEY` is required by `classify` and by `maps --generate`.

## The shape of the problem

Measured, not assumed — `inventory` prints all of it:

| | |
|---|---|
| `.RIM` files on disk | 513 (479 DXT1, 11 DXT3, 23 carry no S3TC image) |
| Named by a shipped `BMPNAMES` table, decodable | **364** |
| Named but unusable | 4 not on disk, 1 (`structures/eog_cylinder`) carries no S3TC |
| Sizes | 269 × 1024², 61 × 512², 33 × 256², 1 × 128² |
| With any alpha | 3 |
| UV samples resolved | 1,359,645 polygon references — 4.0% junk, 1.5% whole-sheet |
| Distinct part names sampling them | 6,299 |
| Segmented into regions | 336 textures → **1,741 regions**; 28 are single-material |
| Regions per texture | 166 have 1, 77 have 2–4, 42 have 5–12, 40 have 13–23, 11 at the cap |

**Almost nothing here is a wrap-tiled texture.** 246 of the 364 sheets have a tiling
fraction of exactly zero and only **6** are above 0.5 — the game samples
sub-rectangles nearly everywhere. That matters twice: it is why per-region
classification is the whole game, and it is why the seam-repair pass
(`blend_seamless`, which doubles generation cost) applies to six textures rather than
to a third of them.

A pixel-based estimate of the same question said 99 of 364 "plausibly tile". It was
measuring the wrong thing — see the note on tiling below.

The naive process — one prompt per file per map — fails for three reasons, in
descending order of how much they constrain the design.

**Most of these sheets are atlases.** `units/baddies3.rim` is named by 133 `.rif`
files and sampled by 94,301 polygons belonging to 569 distinct named parts. One
roughness answer for that sheet is wrong by construction; it holds robot plate,
silo walls, switches and terrain side by side.

**A generative image model does not preserve pixel registration.** It re-renders
rather than edits. Roughness can survive a few texels of drift; a **normal map
cannot**, and neither can an emissive mask, which is mostly black and so lights the
wall beside the lamp instead of the lamp.

**Nothing constrains a model's RGB output to be a valid normal map.** A normal map
holds unit vectors in a tangent basis. An invalid one lights as noise.

## The pipeline

**The model decides what a material is. Arithmetic decides what every pixel is.**

### 0 · `inventory` — regions, from the geometry

Walks every `.rif`, resolves each `BMPNAMES` entry against the install, and for each
texture rasterizes the UV triangles that sample it into an occupancy mask —
**grouped by the `OBJHEAD1` name of the part that samples them**. Writes the albedo
PNG, a full-resolution label image, and `manifest.json`.

So a region is "the texels `l7#gun` uses", which is both a mask to paint into and a
phrase to hand a classifier. Three wrong versions preceded this one, and each is
recorded in `gkpbr/atlas.py` because each is easy to repeat:

- **Grouping by connected components of the mask does not work.** These atlases have
  no gutters — `baddies3.rim` is 99% covered — so every patch touches its neighbours
  and one flood fill swallows the sheet.
- **Grouping by mesh adjacency does not work.** RIF UVs are an *indexed* list, not
  parallel to the vertices, so two polygons sharing a vertex may sit anywhere in the
  sheet. A union-find over vertex indices produced regions spanning the whole texture.
- **UVs must be translated as a whole, never wrapped per vertex.** A triangle
  spanning u = 1020..1030 on a 1024 sheet has its second vertex land at 6 under a
  modulo, and then fills the entire width.
- **Rasterize at full texel resolution, not coarsely.** Discovery was done on a
  1/8-scale mask for speed and then that same mask was reused for *painting*,
  upsampled by pixel replication — so every region boundary snapped to an 8-texel
  grid and `derive` painted roughness and metalness into visibly stepped blocks.
  Coarse is fine for deciding which regions exist; it is not fine for deciding which
  texels belong to one. It is affordable because the reference list collapses:
  `baddies3.rim`'s 94,301 polygon references are 4,107 distinct UV triangles, and
  `elint_mk2_512`'s 7,843 are 4,079, so every triangle is rasterized at most twice
  whatever the sheet size. Region *ranking* still needs no rasterization at all —
  `_polygon_area` is a shoelace sum.

Labels ship as an **8-bit PNG** rather than a `.npy`: full-resolution labels are 1 MB
raw per 1024 sheet and ~350 MB across the set, while a label image is almost all flat
runs — **338 of them come to 1.18 MB, median 2.7 kB**. A PNG is also directly viewable,
which matters for a mask nobody could otherwise check.

And one trap that is already documented in `rif_chunk_format.md`, which is where to
look first when a name is wrong: **`OBJHEAD1+0x04` is `lock_user`, the editor's lock
holder, not the object's name.** Reading a name from there yields `Player` for 6,383
of the 9,313 shipped objects, and made `player` the second-largest region of every
unit atlas. The name is the trailing string at `+0x3c`.

**Tiling is decided from the UVs, not from the pixels.** A wrap-seam metric on an
image asks "would this tile *well*", which is a question about the artist's skill;
the UVs say what the game actually does with the sheet. The two disagree sharply
here — pixels say 99 of 364 tile, UVs say 6 — and the UVs are right, because a
texture whose edges happen to match but which is never sampled across a wrap does
not need seam-free treatment.

The test is on **relative extent, not on UVs exceeding the sheet**, and that is
measured rather than assumed: Gunlok tiles a ground texture by giving each terrain
quad the full `0..size` range and repeating the quad, not by handing one polygon
`0..8*size`. Testing for UVs past the edge called every ground texture an atlas, and
the full-sheet quads then filled the occupancy mask and buried the real patches.

**A region needs a minimum absolute size, not a minimum fraction of the sheet.**
The floor was `MIN_AREA_FRACTION` alone, which gave 13 texels on a 256 sheet against
210 on a 1024 — so the sheets least able to afford tiny regions were the ones that
admitted them. `ground/cracks.rim` (256²) came out with 24 regions whose tail ran down
to a **30-texel** patch, and the classifier dutifully invented a material for each:
"metal plate edge", "metal plate corner", "metal wall joint", all paraphrases of one
surface, with metalness scattered from 0.0 to 0.7 because nothing legible was there to
judge. Those hedged values painted hard-edged specks into the metalness map.

The free-text `material` field *hid* this — 24 regions produced 24 "distinct" strings,
and only the numbers showed they were the same surface. `MIN_AREA_TEXELS = 512`
(~22×22 px in the image the model sees) takes `cracks.rim` to 13 regions whose
metalness is now uniformly 0.0 and whose materials read as concrete with one genuine
outlier, a painted oil drum at roughness 0.35–0.60 against the concrete's 0.60–0.90.
`elint_mk2_512` (512²) keeps 23 of its 24, and its real variety with them.

**The floor must apply to what survives painting, not to the analytic area that got a
region shortlisted.** Painting goes largest-first, so a large region can clear the
floor on its own area and then be shredded into slivers by the smaller, more specific
regions drawn over it. Filtering only on analytic area left regions of **one and two
texels** in the output — worse than the 30-texel ones the floor was added to remove.
Slivers now revert to region `0`.

**Two limits worth knowing.** The kept regions capture **80.7%** of placed polygons;
the rest belong to regions below the floor and fall into region `0`, classified as
"the rest of this sheet". **11 textures hit the 24-region cap** and lose their tail
the same way.

**A texture is registered from the table, not from the polygons that sample it.**
Keying off polygons alone lost 27 of the 364, in three groups that all look the same
from the manifest and are not the same thing: a `_shadow` file's table (real names,
junk polygon indices, because those meshes are never textured), an entry no polygon
references at all, and an entry whose polygons carry no UV entry. Ten of them are in
ordinary level files — `mplay_zorro`'s `building site 00`, `tanker lift`'s
`hull 22` — so "unreferenced" is not a synonym for "unused". They come out as single
whole-sheet regions rather than vanishing.

That the count now reconciles exactly with an independent walk of the tables (364
both ways) is the check that nothing is being dropped quietly.

### 1 · `classify` — a vision model, answering in JSON

Not the image model. Deciding that a region is painted steel with roughness around
0.4 is recognition, worth about $0.001 on `gemini-3.6-flash`, and its answer is a small
JSON document: reviewable, diffable, **editable by hand when it is wrong**, and
cacheable forever because the input never changes. That last property is what makes
the pipeline re-runnable — `derive` can be rewritten and every map regenerated with
no further model calls.

The model gets the albedo, **a colour-coded region map**, and the part names. Colours
rather than bounding boxes because a part's UV footprint is scattered: `siloa` on
`baddies3.rim` touches texels across a box of (0,192)-(1024,832) while covering about
5% of it, so the box says nearly nothing and the colour says exactly.

### 2 · `maps` — derived, and therefore correct by construction

`roughness`, `metallic` and `emissive` are arithmetic on the albedo's own pixels,
parameterised per region by stage 1. Two properties follow for free: **registered**,
because the detail comes from the very pixels being described, and **tiling exactly
as well as the albedo**, because a per-pixel function of a tiling image tiles.

`metallic` stays a per-region constant on purpose. Metalness is physically
near-binary, and a constant over a correctly-segmented region is *more* right than a
texture hedging across a surface that is entirely one or the other.

`normal` is always derived from a height field by Sobel, never generated. That is
what guarantees every texel is a unit vector, and the gradient wraps so a tiling
height map yields a tiling normal map.

### 3 · the image model, for the two things arithmetic cannot do

Only **height** and **de-lighting**, under `maps --generate` / `--delight`.

Height is the one map carrying information the albedo lacks — luminance is not depth,
a dark painted stripe is not a groove — and it is asked for as **grayscale relief**,
because a grayscale field has no validity constraint to violate.

Misregistration is handled twice. `derive.fuse_height` keeps the model's contribution
to the **low frequencies**, where drift is invisible, and takes high-frequency detail
from the albedo, which is aligned by definition. Then `metrics.accept` rejects or
un-rolls whatever drifted anyway.

### 4 · the gates

Nothing a model returns is accepted without passing `gkpbr/metrics.py`:

- **`align`** — Pearson correlation of gradient-magnitude fields at 96², after
  removing the best whole-pixel shift found by phase correlation. The shift is
  reported separately from the correlation because the distinction matters: a
  uniformly shifted map is recoverable by rolling it back, a decorrelated one is not
  recoverable by anything.
- **`seam_energy`** — the wrap discontinuity relative to the image's own interior
  gradient, so a busy texture is not penalised for being busy.

A failed gate falls back to the derived map. That is why a bad model day is a quality
regression and never a broken run.

## Normal-map convention

`derive.normal_from_height` takes `green_down` and **refuses to guess**. RIF's V grows
downward and the addon flips it on import, so a map written in image space has
+G = "up in the image", which is −V in RIF and +V after the addon's flip. Getting it
backwards inverts lighting on one axis everywhere — subtle enough to survive review
and miserable to find later. Default is OpenGL/Blender (+G up); `--green-down` gives
the DirectX convention.

## Cost

Cost is not the constraint, which is worth knowing before optimising for it. At
current list prices, across all ~364 textures:

| Stage | Model | Per texture | Total |
|---|---|---|---|
| `classify` | `gemini-3.6-flash` | ~$0.001 | ~$0.40 |
| `maps --generate` (1K height) | `gemini-3.1-flash-image` | $0.067 | ~$24 |
| `maps --delight` (1K) | `gemini-3.1-flash-image` | $0.067 | ~$24 |
| seam-repair second pass | as above | $0.067 | ~$0.40 (6 textures) |
| escalation on a failed gate | `gemini-3-pro-image` | $0.134 | per failure |

The whole run is tens of dollars either way, which argues for escalating to the pro
image model on anything that fails a gate rather than budgeting the cheap one. The
seam-repair line is nearly free only because the UV measurement above shrank it from
99 textures to 6.

Stage 1 is what makes re-runs free: its JSON is cached per texture and the input never
changes, so `derive` can be rewritten and every map regenerated with no model calls.

## What the probe measured

`probe` asks for a height map and prints the gate numbers. It is the measurement the
design rests on, and it has now been run — ten textures against
`gemini-3.1-flash-image`.

**Registration holds.** Every accepted result came back at shift `(0, 0)`. The model
does not crop, pad or re-compose, so the fear that drove the whole low-frequency-only
design did not materialise in its strong form. The `fuse_height` mitigation stays,
because it costs nothing and the sample is ten textures rather than 364.

**But the first version of the gate rejected correct answers, and that is the finding
worth keeping.** On `structures/cratetextf 512` the albedo's luminance std is 0.335
and the returned height std 0.055: the model refused to read a crate's painted
light/dark pattern as geometry, which is exactly what it is asked to do. An absolute
gradient-correlation floor of 0.35 scored that 0.34 and threw it away, then fell back
to a derived map that manufactured relief out of the paint. So the gate now tests
**registration**, not fidelity — is the best match at zero offset, or materially
better somewhere else — with a low floor purely for "no relation at any offset", and
a separate escape for a map too flat to correlate at all. `cratetextf` passes.

**The seam repair works on real output.** `ground/wetsand` (tiling fraction 1.00)
came back with an absolute edge step of 0.008 after the half-offset blend. And the
seam gate catches genuine failures: `ground/rock2a 00` was rejected at a 0.51 step,
top row mean 0.527 against bottom row 0.015 — a map fading to black, not a metric
artifact.

Three API facts the published docs get wrong, all of which cost a round trip:

- **Output is JPEG only.** `image/png` is a 400. The loss is high-frequency, which is
  the band `fuse_height` discards anyway, so it lands where it is thrown away.
- **The smallest size is the literal `512`**, not `0.5K` as documented.
- **`additionalProperties` is rejected** by the Developer API, so stage 1's regions
  are a list carrying an `id` rather than an object keyed by id.

One prompt correction came out of reading the output rather than the scores:
`metallic` came back at 0.6–0.8 across a robot's plating, hedging in a range that is
physically meaningless because the value selects between two reflectance models.
Saying so explicitly moved it to 14 × 1.0, 4 × 0.0 and a few genuine mixed-coverage
values — and improved the material reading as a side effect, with `ground/cracks`
going from "grimy metal floor grid" to "weathered concrete floor".
