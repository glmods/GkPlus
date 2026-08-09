# Lighting maps from an image model

Turns one Gunlok `.RIM` into the companion `<stem> lighting.dds` that GkPlus's
Vulkan renderer already knows how to find: three prompts to an image-editing model
over OpenRouter, three greyscale replies, one packed DDS.

```
uv run python -m gklightmap.cli albedo  "ground/gunlok rust.rim"   # look first, no API
uv run python -m gklightmap.cli gen     "ground/gunlok rust.rim"   # the three calls
uv run python -m gklightmap.cli pack    "ground/gunlok rust.rim"   # rebuild, no API
uv run python -m gklightmap.cli install "ground/gunlok rust.rim" --mod preview
uv run python -m gklightmap.cli install --remove --mod preview
```

`OPENROUTER_API_KEY` is read from the environment, and failing that from a file of
that name at the repository root (which is where this checkout keeps one, and which
`.git/info/exclude` already keeps out of commits). `GUNLOK_DIR` overrides install
detection, `GKLIGHTMAP_OUT` the output directory. Only `gen` reaches a model;
`albedo`, `pack` and `install` are arithmetic and file copies.

## The scope, and what it deliberately is not

This is **`pbr/` with the intelligence removed on purpose**. That tool segments an
atlas into regions from the shipped geometry's UVs, classifies each region with a
vision model, derives most maps arithmetically from the albedo's own pixels, and
gates everything a model returns. This one asks three questions about the whole
sheet and packs the answers.

What that costs, stated plainly so nobody re-discovers it as a bug:

- **An atlas gets one answer per channel.** `units/baddies3.rim` holds robot plate,
  silo walls, switches and terrain side by side; a single roughness answer for it is
  wrong by construction. The prompts tell the model the input may be an atlas and to
  judge each patch on its own terms, which is a request, not a mechanism.
- **Nothing checks registration.** `pbr/gkpbr/metrics.py` measures whether a returned
  map still lines up with its albedo, and rejects or un-rolls it when it does not.
  Here the only instrument is your eyes, so `albedo` exists and the three PNGs are
  kept. Registration held on the one texture measured below; a sample of one is a
  sample of one.
- **Nothing is cached against a fingerprint.** Re-running `gen` re-asks and re-spends.
  `--map <name>` re-asks for one channel and reuses the others from disk, and editing
  a channel PNG by hand then running `pack` is the supported repair.
- **No de-lighting, no seam repair, no tiling test.** A sheet the game wrap-samples
  will show its seams.

The exchange for all of that is that the whole thing is four hundred lines and needs
nothing but the texture.

## The three channels are `src/VkLighting.h`'s, not PBR's

This is the part worth getting right, because two of the three do not mean what their
names mean anywhere else, and a wrong channel is silent — the renderer reads whatever
is in each one as that channel's quantity.

| | channel | what the renderer does with it |
|---|---|---|
| **bump** | R | A **height field**. The normal is derived at draw time from its gradient against a tangent frame taken from the fragment's own derivatives — so an RGB normal map here would light as noise, and the prompt asks for greyscale relief. |
| **metallic** | G | The **intensity** of the highlight, straight through. Not a metal/dielectric switch; nothing about the base colour changes with it. So the prompt asks "how strong a highlight", not "is this metal". |
| **roughness** | B | The **sharpness** of the highlight, as a specular exponent — 0 sharpest (`gloss_max`, 256), 1 broadest (`gloss_min`, 4), interpolated in log2. This is the one channel where the ordinary meaning is the right one. |

The alpha channel is unread. `gklightmap/prompts.py` is the whole prompt set and is
meant to be edited; the three docstrings there restate the above so the file stands
on its own.

## The file name is the entire interface

Nothing registers a lighting map. `src/VkLighting.cpp`'s `Candidates` takes a live
image's `.rim` name, strips the extension, and probes four paths — `graphics/<stem>`
and a bare `<stem>`, times `" lighting.dds"` and `"_lighting.dds"` — through the mod
VFS first and then the real install. So `install` writes

```
<Gunlok>\gkplus\mods\<mod>\Graphics\<stem> lighting.dds
```

which is the first candidate, served the way every other modded asset is
(`mod_loading_notes.md`). Dropping the file next to the `.RIM` works too.

**`install --remove` is not a nicety.** A leftover mod goes on serving in every later
session and the game looks *fine*; `pbr/README.md` records the session that cost.

To see whether the engine took it, `render.describe_lighting()` over the REPL prints
every name probed and what came of it — a texture with no companion is the normal case
and is therefore silent by design, so "nothing happened" and "it was never found" look
identical from the outside without it.

## Why the DDS is uncompressed

`src/VkLighting.h` recommends DXT1 to a modder and that is right for artwork. It is
wrong here, twice:

- Two of the three channels are **masks**. DXT1 quantises a 4×4 block's three channels
  against one pair of endpoints, so a metallic edge and a roughness edge inside one
  block drag each other around — the failure is a smear on a value that selects between
  two reflectance models.
- There is no S3TC compressor in this tool's dependencies. `pbr`'s `preview` shells out
  to `utils/rimutil` for that, which writes `.RIM` and not `.dds`. So the real choice
  was between correct and absent.

`gklightmap/dds.py` therefore writes **24-bit B,G,R with a full mip chain down to
1×1**, which `src/Dds.cpp` accepts: the 4×4 mip floor documented there is an S3TC rule
(the engine's row loop decrements by 4 and can only terminate on a multiple of it) and
uncompressed levels are explicitly exempt. Nothing about that floor binds the Vulkan
path, which decodes these itself — but a file legal to both codecs is worth more than
one legal to one.

The chain is not optional. `pbr`'s preview run measured what a mip-less map with
texel-scale content does at minification, and the answer was full-amplitude speckle.

Cost: 4.2 MB for a 1024² map against DXT1's ~0.7.

## What was measured

`ground/gunlok rust.rim`, 1024², `google/gemini-3.1-flash-image`, one run:

| | seconds | mean | std | cost |
|---|---|---|---|---|
| bump | 14.7 | 0.477 | 0.115 | $0.0680 |
| metallic | 7.6 | 0.002 | 0.002 | $0.0678 |
| roughness | 9.3 | 0.598 | 0.071 | $0.0682 |

**$0.204 a texture**, which is three times `pbr`'s per-image figure because it is three
images. Across the 365 textures a `BMPNAMES` table names that is about $75.

Registration held: the bump and roughness maps' vertical streaks land on the albedo's
vertical streaks, checked by eye on a contact sheet. Both are plausible for rust — a
high roughness with wear variation, relief that follows the streaking rather than the
paint.

**Metallic came back essentially black (mean 0.002).** For rust that is a defensible
answer and the prompt does ask the model to be decisive rather than hedge at 0.5. It is
also exactly the shape of result that is indistinguishable from the model having
ignored the question, and there is no gate here to tell the two apart. Look at the
picture. (A second run of the same model on the same texture returned 0.084 instead,
which is the other thing to know: nothing here is deterministic and no `seed` is sent.)

## Which model, measured

Same texture, same prompts, five models, one run each:

```
for M in qwen/qwen-image-3-pro openai/gpt-image-2 google/gemini-3.1-flash-image \
         x-ai/grok-imagine-image-quality black-forest-labs/flux.2-max; do
  GKLIGHTMAP_OUT="out/models/${M//\//_}" \
    uv run python -m gklightmap.cli gen "ground/gunlok rust.rim" --model "$M"
done
uv run python tools/contact_sheet.py out/models out/models/sheet.png
```

All five returned three greyscale images at 1024², so "it ran" separates nothing. Three
numbers do, and none of them is in the tool:

- **`grad`** — correlation of gradient magnitude with the albedo's, and the best whole-pixel
  shift. This is registration: high and at (0,0) means the model edited the picture rather
  than re-drawing something like it.
- **`fit`** — the residual after fitting `a·luma + b` to the map. **This is the one that
  matters**, and it is not obvious in advance: the dominant failure is not a bad answer,
  it is the model desaturating the input and handing it straight back. That scores
  *perfectly* on registration.
- **`b~r`** — correlation between the bump and roughness answers. Three questions that got
  one answer is the same failure seen from the other side.

| model | $ | grad (b/m/r) | shift | fit residual /255 | b~r | verdict |
|---|---|---|---|---|---|---|
| `openai/gpt-image-2` | 0.063 | 0.71 / 0.50 / 0.58 | (0,0) | 12 / **48** / 13 | **-0.43** | three genuinely different answers, registered |
| `google/gemini-3.1-flash-image` | 0.205 | 0.50 / 0.82 / 0.87 | (0,0) | — | 0.54 | registered; **metallic and roughness are 0.96 correlated with each other** |
| `black-forest-labs/flux.2-max` | 0.300 | 0.70 / **0.09** / 0.28 | metallic **(14,0)** | — | 0.52 | bump registered; **metallic drifted 14 rows and is uncorrelated** |
| `qwen/qwen-image-3-pro` | 0.129 | 0.24 / 0.18 / 0.19 | (0,-1) | 15 / 17 / 19 | **0.93** | the albedo, softened. Slope ~0.85 on all three |
| `x-ai/grok-imagine-image-quality` | 0.180 | 0.97 / 0.90 / 0.98 | (0,0) | **6 / 12 / 7** | **0.99** | the albedo, contrast-stretched. `bump = 1.52·luma − 1.2` |

**`grok` and `qwen` did not answer the question at all.** Grok's bump map is the albedo's
luminance times 1.52, to within 5.5/255 — and it scores 0.97 on registration, better than
anything else in the table, because a copy is perfectly registered. A gate built on
alignment alone would have passed it.

**`gpt-image-2` is the one to use** for two of the three channels — see the bump section
below, which is why the recommendation is per channel and not per model. Its roughness fits
`−0.52·luma + 178`: dark streaks come out *rougher*, which is a real inference about rust
rather than a re-tint. Its metallic is the only map in the sweep that is not a function of
the albedo's luminance at all.

### Cost, from billing rather than from the price list

This section said `gpt-image-2` cost "a third of Gemini's price", from one 1024² texture.
Over 1,218 billed calls it is about **half**, and the gap depends on the size:

| source size | `gemini-3.1-flash-image` | `gpt-image-2` | gpt cheaper by |
|---|---|---|---|
| 1024² | $0.0683 | $0.0245 | 2.8× |
| 512² | $0.0462 | $0.0376 | 1.2× |
| 256² | $0.0456 | $0.0195 | 2.3× |
| every call made | **$0.0640** (454, $29.05) | **$0.0306** (764, $23.38) | **2.1×** |

The 512² row is the interesting one, and it is a measurement rather than noise:
**`gpt-image-2` refuses any request below 1024², `512x512` included** — "Requested
resolution is below the current minimum pixel budget" — so for every 256² and 512² texture
the `size` this tool asks for is discarded by the fallback in `openrouter.generate` and the
image is produced at the provider's own default, then downsampled here. That is why its
512² calls cost *more* than its own 1024² ones. Gemini honours all three sizes, which is
what closes the gap at the bottom.

Neither number decides anything on its own: the bump channel is bought from the more
expensive model because the cheaper one cannot produce a height field at all.

**Gemini's failure is subtler and worth knowing**, because it is the default: bump is
independent, but metallic and roughness came back as the same pattern at two different
levels. The channels are still usable — highlight intensity and highlight sharpness are
allowed to correlate on a uniform material — but on an atlas that would mean one answer
where the point was to get two.

`tools/contact_sheet.py` lays a directory of runs out as one row per model. There is no
gate in this tool, so that sheet plus the three numbers above is the whole instrument.

## The metallic channel came back far too high

The first bulk run — 224 of the 298 textures under `Graphics/Ground`, `gpt-image-2` —
put the metallic channel's **median at 0.366, with 64% of textures above 0.30**, on a
directory that is almost entirely concrete, rock, gravel, sand and asphalt. It should
have been overwhelmingly near zero.

The asset names make it plain, and they also make the answers look arbitrary rather
than merely biased:

| texture | metallic |
|---|---|
| `city wall conc floor 01` | 0.977 |
| `city ruins tranch` | 0.974 |
| `concrete1024` | 0.851 |
| `beige rock 1024` | 0.846 |
| `city wall conc floor 02` — *the same material* | 0.021 |

### The obvious fix is the wrong one

Putting the asset's own name in the prompt is the obvious move — Gunlok's artists
named these usefully — and it does not work. A three-way A/B, metallic only, on eight
ground textures whose right answer is legible from the picture (six matte, two hulls):

| variant | matte mean (6) |
|---|---|
| the shipped prompt | 0.330 |
| **\+ the asset name** | **0.404** |
| \+ the asset name and a calibration paragraph | **0.036** |

**The name on its own made it worse.** What fixed it is the paragraph now at the top
of `METALLIC`: black is the default answer, name the specific shiny thing or answer
black, and a photograph of a rough material is full of bright specks that are its
texture rather than gloss. Four of the six matte textures came back at exactly 0.000.

### And the check that the fix is not just "always black"

A prompt that collapses to black would score perfectly on that table, which is the same
trap `grok` fell into above. So the same prompt was run against surfaces that genuinely
are metal:

| | mean | std |
|---|---|---|
| `units/gunlok_mk2_1024` | 0.256 | 0.313 |
| `units/maskelyn_mk2_512` | 0.219 | 0.365 |
| `structures/eog_cylinder` | 0.452 | 0.457 |
| ground textures, same prompt | 0.000–0.13 | — |

The **standard deviation** is the answer: these are masks with plates bright and the
rest dark, not flat washes. The model is discriminating.

### Why the name is on one prompt only

`WANTS_CONTEXT` is `{"metallic"}`. The name earned its place there as half of a
combination that was measured; nothing was measured about what it does to a height
field or a roughness map, and adding it to those would have invalidated **448
already-bought answers, about $11**, on an untested intuition.

### What it did to the set

All 298 textures under `Graphics/Ground`, regenerated:

| metallic mean | before | after |
|---|---|---|
| median | 0.366 | **0.036** |
| above 0.30 | 64% | **10.8%** |
| below 0.05 | 4% | **52.7%** |

The tail is the check that it did not simply go black: what is still high is
`prison floor 01 512` (0.747), `drainage` (0.747), `s3 level 1k 11_floor panels`
(0.694) — metal floor plates and drains, which is where a highlight belongs.

That number is the reason `prompt_for` assembles its blocks with a single `"\n"`.
A prompt is what a stored answer is fingerprinted against, so **a block that did not
change must render byte-identically** — joining with a blank line instead, which is
what the first version of this change did, silently marked every bump and roughness
answer stale over whitespace. `tests/test_prompts.py` pins that form.

## The unit atlases: variance, not wording

The same channel came back too high again on `Graphics/Units` — `gunlok_mk2_1024`
at mean 0.446 with **50% of the sheet above 0.5**, `frend_MK2_512` at 54%, every body
panel painted white with black only in the gaps between parts.

The reflex is to reach for the prompt again, and it is wrong here. A clause saying
white is rare, that painted or anodised panels belong at 0.2–0.4, and that a robot is
a few bright trim pieces on a mid-grey body — A/B'd with four ground textures as a
regression control — moved two of four units the *wrong* way (gunlok 0.226 → 0.491,
ELINT 0.201 → 0.474) and the other two down. That pattern is noise.

**Six draws of the identical prompt, model and texture** for gunlok's metallic:

```
0.226   0.255   0.256   0.262   0.356   0.446
```

The spread between runs is far wider than anything wording moved, and the map that
had been installed was the worst of the six. Ground does not behave this way —
`CONCRETE1024` lands 0.000–0.092 across runs — because "concrete is matte" is
unambiguous where "how shiny is a robot" is not. **A question the model itself is
unstable about cannot be settled by asking it more precisely.**

So `--samples N` takes N draws and keeps the **per-pixel median**. Median and not
mean, because the failure is an outlier draw and a mean would carry a third of it
through. `--samples 1` is bit-identical to not passing it, so nothing already bought
goes stale.

| goodie sheet | 1 sample | median of 3 |
|---|---|---|
| `gunlok_mk2_1024` | 0.446 / 50.0% > 0.5 | **0.257 / 20.6%** |
| `frend_MK2_512` | 0.436 / 54.0% | **0.290 / 21.2%** |
| `maskelyn_mk2_512` | 0.265 / 18.3% | **0.144 / 16.4%** |
| `ELINT_MK2_512` | 0.226 / 15.6% | 0.202 / 15.0% |

The per-draw means are kept in `meta.json` as `sample_means`, which is what makes the
variance visible at all: `sneeker 512` drew 0.641 / 0.232 / 0.087.

**Two things this does not fix.** These atlases have highlights and form shading
painted into them already, so a specular response is *added* to a painted one and no
metallic map can subtract that — de-lighting would, and this tool has none. And the
engine over-drives its lights, which is why `LightingMapParams::specular_scale`
defaults to 0.25. If the result is still too hot in game, `render.specular_scale` and
`render.specular_from_diffuse` are live knobs over the REPL and cost nothing to sweep.

## gpt-image-2 returns a relief *render*, not a height field

Looked at side by side, its bump maps for the unit sheets have a bright border along
the top-left of every panel and a dark one along the bottom-right. That is a baked
light direction — a picture of how the relief would *look* lit, rather than how high
it is. `src/VkLighting` differentiates the red channel, so an emboss manufactures a
ridge/valley pair at every panel edge and bakes a second light direction into a
surface the engine is already lighting.

`google/gemini-3.1-flash-image` does not do this: its panels are flat faces at
distinct levels with hard steps between them, which is what a height field looks like.
So the Units bump channel is bought from gemini and the other two from `gpt-image-2`;
`meta.json` records the model per map, and mixing them costs nothing because each
channel is a separate request.

Two numbers over all 57 sheets support what the pictures show:

| | albedo-fit residual (median) | std (median) |
|---|---|---|
| `gpt-image-2` | 13.9/255 | 0.100 |
| `gemini-3.1-flash-image` | **27.8/255** | 0.187 |

A *higher* residual is better here: it means less of the map is explained by a linear
function of the albedo's luminance, which is the "luminance is not depth" failure. The
doubled standard deviation is the other side of the same coin — more relief, and worth
knowing because `bump_scale` is per texel, so a gemini map drives the derived normal
about twice as hard as a `gpt-image-2` one at the same setting.

**This was not reducible to a metric, and two attempts are recorded here as failures
rather than quietly dropped.** A "correlate the high-frequency band against the
directional derivative of the low-frequency one" detector scored ±0.00 on every one of
355 maps, embossed or not, because a bevel is an antisymmetric doublet about the edge
and a blurred step's gradient is symmetric, so the product integrates away. A
short-range anisotropic autocorrelation did no better — `concrete1024` scored like
`gunlok_mk2_1024`, and `frend_MK2_512`, which is plainly embossed, scored lowest of
all. Neither is in `tools/`: a metric that reads zero on a defect you can see is worse
than no metric, because someone will trust it.

## What is generated, and by which model

| directory | sheets | bump | metallic | roughness |
|---|---|---|---|---|
| `Graphics/Ground` | 298/298 | gemini | gpt-image-2 | gpt-image-2 |
| `Graphics/Units` | 56/57 | gemini | gpt-image-2 ×3 | gpt-image-2 |
| `Graphics/Structures` | 99/100 | gemini | gpt-image-2 ×3 | gpt-image-2 |

453 files, all parseable, **1.48 GB**. The emboss is a property of the model and not of
the subject matter — `concrete1024`'s bolt heads each carried a bright top-left crescent
and a dark bottom-right one, and `city wall conc floor 01` drew every crack as a dark line
paired with a bright one — so Ground was re-bought from gemini along with the rest.

The two absentees are both persistent moderation refusals, and neither costs anything:
`Units/Custom Screen bg 1k 01` is an unlit UI sheet whose map would be inert, and
`Structures/level04` keeps no map at all. A texture with no companion is the normal case.

**A partial run is legal.** `gen --map X` no longer requires the other two to exist; it
buys what was asked, prints `not packed: no metallic or roughness yet`, and packs only once
all three are present. Before that, generating a fresh directory one channel at a time
needed a throwaway full pass purely to satisfy the check.

## Two things a bulk run turns up that a single texture never will

**A trailing space in an asset name.** `Ground\outskirts robot ring .RIM` and
`Ground\city wall conc transitions .RIM` both ship with one, and Windows will not
store it at the end of a path component — `os.makedirs("out/x ")` *succeeds*, creating
a directory called `x`, and the next `open("out/x /albedo.png")` raises
`FileNotFoundError`. It reads as a missing file rather than a refused name. Only
`slug()` trims; `stem()` must not, because it is the engine's own lookup key, and the
installed name keeps the space happily because `<stem> lighting.dds` puts it in the
*middle*. `tests/test_source.py` covers it, including a case that confirms the
untrimmed form still fails.

**Provider moderation fires on photographs of gravel.** Eight of the 298 came back
`HTTP 400 ... rejected by the safety system` on `gpt-image-2` — `Beige Rock 1024`,
`crashland rock1`, `city ruins road 1_a` and five more. **All eight passed on a plain
re-run**, which says the classifier is nondeterministic rather than that the content
is objectionable. The client deliberately does *not* fold a safety 400 into its retry
set even so: an automatic loop around a refusal is the wrong default whatever the
subject, and re-running the batch is a person deciding. The driver reports them, and a
second pass costs cents.

## Size

The `Ground` set is **1.1 GB** installed — 298 files, most of them 1024² at 4.2 MB.
That is the uncompressed-plus-mips choice above, and it is a lot to ship. DXT1 would be
about a sixth of it and needs an S3TC compressor this tool does not have.

## Tests

```
uv run python tests/test_dds.py
```

No arguments, no Gunlok install, no network. It checks the writer against a
**re-derivation** of `src/Dds.cpp`'s parser rather than against `dds.py` — a reader
written from the writer would agree with a wrong file as readily as a right one — plus
the channel order, the B,G,R store, non-power-of-two sizes, the mip chain, and the
greyscale/resize normalisation on a reply.

It is **not** written in `pbr/tests`' style, and that is deliberate: those append to a
module-level `FAILURES` list instead of asserting, which makes them report green under
pytest collection whatever fails. Here a failure is an `assert` and the exit code is
right under any runner. `test_the_reader_can_actually_fail` breaks a header on purpose,
and the runner was confirmed to report and exit 1 on a deliberately broken assertion.

Lint is `uv run --group dev ruff check .`.

## The boundary with the addon

`gklightmap/source.py` imports `blender/io_scene_rif`'s `rim.py` for the decode rather
than reimplementing it — the same seam `pbr/` uses, and the same hazard: an addon
change breaks this silently. `pbr/tests/test_addon_boundary.py` is the test that exists
for it, and it covers both entry points used here (`rim.load`, `rim.TextureIndex`).
