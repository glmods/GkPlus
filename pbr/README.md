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
uv run python -m gkpbr.cli observed       # what the running game draws each sheet with
uv run python -m gkpbr.cli probe          # does the image model hold registration?
                                          #   (run this first; see the probe section)
uv run python -m gkpbr.cli classify       # stage 1: material JSON per texture
uv run python -m gkpbr.cli maps           # stages 2-4: the PNGs
uv run python -m gkpbr.cli preview <tex>  # one map into the running game; --remove after
```

`GUNLOK_DIR` overrides install detection, `GKPBR_OUT` the output directory,
`GEMINI_API_KEY` is required by `probe`, by `classify` and by `maps --generate`.
Nothing else calls a model: `inventory`, `observed`, `preview` and a plain `maps` are
arithmetic, and even `maps --delight` without `--generate` falls back to
`derive.delight`.

## The shape of the problem

Measured, not assumed — `inventory` prints all of it:

| | |
|---|---|
| `.RIM` files on disk | 513 — 479 DXT1, 11 DXT3, 23 palettized `BODY`+`CMAP`, and **none carries both** |
| Named by a shipped `BMPNAMES` table, decodable | **365** |
| Named but not on disk | 4 |
| On disk but named by no table | 148 — none reachable from a polygon, see below |
| Sizes | 269 × 1024², 61 × 512², 34 × 256², 1 × 128² |
| With any alpha | 4 |
| UV samples resolved | 1,359,725 polygon references — 4.0% junk, 1.5% whole-sheet |
| Distinct part names sampling them | 6,300 |
| Segmented into regions | 336 textures → **1,741 regions**; 29 are single-material |
| Regions per texture | 166 have 1, 77 have 2–4, 42 have 5–12, 40 have 13–23, 11 at the cap |

**The 23 palettized files are decodable, and one of them is in the manifest.** They
were listed here as unusable while the addon's `rim.py` handled only S3TC; it learned
`BODY`+`CMAP` in a533756 and all 23 decode today. Exactly one is named by a table —
`structures/eog_cylinder`, 256², alpha, the manifest's only non-S3TC entry — and it is
the whole difference between the 364 this said before and the 365 it says now: 80
polygon references, one part name, 256² and one alpha, all of which the rows above
absorb. The other 22 are in the 148, and what they turned out to be is below.

**Almost nothing here is a wrap-tiled texture.** 246 of the 365 sheets have a tiling
fraction of exactly zero and only **6** are above 0.5 — the game samples
sub-rectangles nearly everywhere. That matters twice: it is why per-region
classification is the whole game, and it is why the seam-repair pass
(`blend_seamless`, which doubles generation cost) applies to 29 textures rather than
to a third of them. **29 and not 6**, because the pass is gated on the manifest's
`tiling` flag, which is `tiling_fraction >= 0.5` **or** no surviving region at all —
so the 23 sheets that segment into no region go through it too, each treated as one
whole-sheet surface.

A pixel-based estimate of the same question said **99** of the sheets "plausibly
tile". It was measuring the wrong thing — see the note on tiling below.

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
  no gutters — `baddies3.rim` is 96% covered — so every patch touches its neighbours
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
here — pixels say 99 tile, UVs say 6 — and the UVs are right, because a
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
Keying off polygons alone loses **17** of the 365, in two groups that look identical
from the manifest and are not the same thing: **7** are named only by a `_shadow`
file's table (real names, junk polygon indices, because those meshes are never
textured), and **10** carry an index no polygon in an ordinary file ever names —
`mplay_zorro`'s `building site 00`, `tanker lift`'s `hull 22` — so "unreferenced" is
not a synonym for "unused". A third group survives registration and then loses
everything to the UV filter instead: **2** more (`ground/concretetexturef`,
`ground/rock and moss`) are referenced only by polygons whose UVs are junk. All 19
come out as single whole-sheet regions rather than vanishing.

(This paragraph said 27 in three groups, one of them "polygons that carry no UV
entry". Re-measured against the current walk it is 17 + 2, and the no-UV-entry group
is empty — every polygon that names one of these has a UV entry, it is the *values*
in two of them that are unusable.)

That the count now reconciles exactly with an independent walk of the tables (365
both ways) is the check that nothing is being dropped quietly.

**And the mirror image: 148 of the 513 files on disk are named by no table at all —
and not one of them can be reached from a polygon.** That is structural, not a limit
of this walk. `BMPNAMES` is the only name-to-index binding in the RIF format, and the
one alternative the loader registers, `SHPTEXFN`, appears in **no shipped file** (see
`rif_chunk_format.md`), so a texture no table names has nothing that could tie it to a
triangle. What can still reach one is a name spelled out somewhere, so `inventory`
searches the two places there are and prints the split rather than staying quiet:

| what names it | n | what they are |
|---|---|---|
| a file under `scripts\` | 27 | the `bitmap` field that gives a level its map image, plus the briefing and credits screens |
| a literal in `gl.exe` | 17 | the front end, the fonts, the HUD, and three **liquid surfaces** — see below |
| nothing in the install | 104 | authoring leftovers |

Getting that split right needs the haystack folded, not the needle: `gl.exe` writes
`bitmaps\lava.rim`, while a `.gls` writes `bitmap "bitmaps\\LEVEL02.rim"` with the
backslash **doubled**. Searching for the single-backslash form alone found 2 of the
27 — every level-map bitmap read as "nothing names this" while the scripts named it
plainly.

**`bitmaps\lava`, `bitmaps\oil` and `bitmaps\swamp` are world surfaces, and that is
now measured rather than guessed.** This paragraph used to say it would need the
running game; it did, and the running game says all three are bound as the stage-0
texture of ordinary world draws:

| | draws observed | in |
|---|---|---|
| `bitmaps\lava.rim` | 31,608 | level01, level02, level04, level15, prison |
| `bitmaps\swamp.rim` | 17,911 | cityruins, junkyard, level02, level11 |
| `bitmaps\oil.rim` | 2,816 | level02, level07 |
| `bitmaps\water.rim` | 40,761 | level01, level02, level03, level05, level06, level10 |

The mechanism is the console command family `WATER` / `LAVA` / `OIL` / `SEA` /
`SWAMP`, which lays a liquid surface over the rectangle between two named dummy
objects — `level04.gcs` line 23 is `lava 1 "lava_a" "lava_b" 1` — so the engine
supplies the texture name itself and no `BMPNAMES` table is involved. That is why
they are unreachable from a polygon and still painted on the world.

**The shipped scripts corroborate the observation exactly.** `LAVA` appears in
`junkyard.gcs`, `level01.gcs`, `level04.gcs`, `level15.gcs` and `prison.gcs`; `SWAMP`
in `cityruins.gcs`, `junkyard.gcs` and `level11.gcs`; `OIL` in `level07.gcs` and
`level10.gcs`. Every level in the table above is on those lists, and the two that are
on the lists but not in the table — junkyard's lava and level10's oil — are levels the
camera tour did not reach that part of. `level02` appears in the lava, swamp and oil
rows only because the harvest issued those three commands there by hand, against the
dummy objects `level02.gcs` already uses for its own `WATER`; every other level in the
table is the shipped campaign drawing them unprompted, which is the stronger evidence
and the reason the manual probe is not what this rests on.

`bitmaps\water.rim` is the fourth of the family and was never in question, because a
`BMPNAMES` table happens to name it as well — it is already one of the 365 and is not
in the `gl.exe` row above. The four are not drawn alike: **water blends and
alpha-tests on 100% of its draws and is the only one of the four with an alpha channel
(DXT3 against DXT1); lava, oil and swamp are drawn fully opaque**, depth writes on,
alpha test off.

That last fact cuts against the obvious next move, and is worth stating plainly:
**nothing in the draw log says lava emits light.** It is drawn exactly as concrete is.
The case for an emissive map on it rests entirely on the picture — 1024² of orange
and yellow crust — which is a stage-1 judgement, not a measurement, and is precisely
the division of labour the rest of this file argues for.

The three are **not** adopted into the manifest, and that is a boundary rather than an
oversight: `inventory` walks `BMPNAMES`, a liquid surface has no UV triangles to
segment, and a whole-sheet region is exactly what the pipeline already produces for
the 19 textures no usable polygon reaches. Adopting them would be a small change and a
real one — they are tiling 1024² world surfaces and a PBR set for each is meaningful
in a way that one for a load screen is not. It is **not done here**; what is done is
that they are named, counted and measured instead of being filed under "front-end
art".

**The 22 palettized files in that group were the reason for looking**, and they land in
two of the three rows — one in `scripts\` and twenty-one in "nothing at all":

- **`bitmaps/mplay_maze`** is `Maze.gls` line 202, `bitmap "bitmaps\\mplay_maze.rim"` —
  a multiplayer level's map image, which `levels.py` already keeps separate from the
  surfaces a polygon samples.
- **`units/save screen`, `units/english load save`, `units/Command Wheel 01`** are
  front-end art, and superseded at that. `gl.exe` holds `units\Command wheel 01 512.rim`
  — the 512 variant, which 24 `.rif` tables also name — and mentions neither the
  unsuffixed wheel nor either load/save screen, which is why all three are in the
  last row. Decoded, the two screens are the LOAD/SAVE/DELETE/EXIT mock-up in the
  HUD's green.
- **`ground/tree_bark`, `ground/tree_alpha` and the sixteen `ground/city_*fmv_*` sheets**
  are named by *nothing*: no table, no script, no `.rif` chunk of any id, no literal in
  `gl.exe` or in the five `glres<lang>.dll`. They are also photographs — bark, foliage,
  glass-curtain facades, parked cars — in a register no in-game surface uses, and
  `city_dest_fmv_road_1024` is 512² despite its name. The `fmv` in sixteen of the
  eighteen, next to the four shipped `.bik` movies, says what they were for; **that last
  step is inference and everything before it is measurement.**

So a PBR map set for any of the 22 would be for a load screen, a level map image or a
cutscene pre-render, and none of them is adopted into the manifest. They are counted
and named on every `inventory` run instead, because silence is exactly what let the 17
above go missing.

**The walk is rooted at `<Gunlok>\RIF`, not at the install.** All 563 shipped `.rif`
live under it — 47 `Levels`, 348 `Objects`, 163 `Units`, 5 `User Interface` — while
walking the install root also reached `gkplus\mods\*\RIF\**\*.rif`, so the manifest
depended on which mods happened to be installed on the machine that built it. It
changed no number here, and only by luck: the one `.rif` in this machine's leftover
`rimutil-body-test` mod carries no `BMPNAMES` table and `collect` skips a file without
one. A mod that replaced a level would not have been so polite.

### 0.5 · `observed` — what the engine actually does with each sheet

Stage 1 guesses at two things the running game can simply be asked: whether a surface
emits light, and whether it is a cut-out. GkPlus's D3D8 capture layer already logs,
for every draw of the last complete frame, the stage-0 texture's `.rim` path together
with `ALPHABLENDENABLE`, `SRCBLEND`, `DESTBLEND`, `ZENABLE`, `ZWRITEENABLE`,
`CULLMODE`, `ALPHATESTENABLE`, the FVF, the viewport and the primitive count. It is
built from the shadow state, so it exists under `GKPLUS_RENDERER=d3d8` as well —
**no Vulkan renderer is needed for any of this.**

`render.debug.frame_draws()` holds one frame, so a single dump is one camera's worth of
draws. `utils/rendertest/harvest-draws.ps1` merges every frame into a running
per-texture map **inside the game** and brings back only the totals;
`gkpbr.cli observed --from <dump>` folds that into the checked-in
`pbr/render_profile.json`. There is no C++ change and no `main.mjs`: the REPL
evaluates in global scope and `var`/`function` persist between lines, so the
accumulator lives in the REPL context's own globals.

#### What the run measured

`render_profile.json` is one session on 2026-08-06 against the `d3d8.dll` built the
day before, under `GKPLUS_RENDERER=d3d9`, in two processes:

| | |
|---|---|
| rendered frames sampled | **168,496**; a further 67 went past between polls *while sampling*, 0.04% |
| draws folded | 12,185,030 |
| levels | 17 + the front end: level01–level07, level09–level12, level15, prison, junkyard, cityruins, Maze, mplay_atlantic |
| per level | briefing, settle-to-rest, a two-pass arrow-key camera tour (scroll/rotate/zoom/elevate), then a no-argument effects battery |
| distinct sheets bound | **289** |
| of the manifest's 365 | 282 seen, **83 not seen in this run** |
| bound but not in the manifest | 7 |
| distinct render-state tuples over all 12.2M draws | **7** |
| never blended / blended on every draw | 274 / 5 |
| ever drawn additively / **only** additively | 2 / **0** |
| ever alpha-tested / always alpha-tested | 15 / 4 |

The two sheets that are ever additive are `bitmaps/particles.rim` (100% blended, 20%
additive, 20% with depth writes off) and `units/alpha junk.rim` (100% blended, 5%
additive). The five blended on every draw are those two plus `bitmaps/water.rim` and
the two font sheets, and the four always alpha-tested are the same five without
particles.

#### Three traps, each of which produces a wrong answer first

- **`SRCBLEND`/`DESTBLEND` mean nothing while `ALPHABLENDENABLE` is 0**, and the game
  leaves stale factors in them constantly. `units/plates 2 1024.rim` — the game's
  third most-drawn sheet at 947,189 draws — sits at `SRCALPHA -> ONE` with blending *off*
  for 134,325 of them. Read the destination factor without checking the enable and the
  HUD becomes the most emissive thing Gunlok owns.
- **A render state persists between draws.** `ALPHATESTENABLE` is not an attribute of
  the texture bound at the time; it is whatever the last caller left it as.
  `units/baddies3.rim` is alpha-tested on 0.3% of its draws, which is telling you
  about its neighbours in the queue. That is why every field in the profile is a
  **count** and the fractions are reported rather than thresholded into a boolean.
  The 50% figures on the unit sheets are a different thing again and are real: a unit
  is submitted twice, once opaque and once into the translucent pass.
- **Absence is much weaker evidence than presence.** 83 unseen is a property of this
  run — 47 `ground`, 19 `structures`, 16 `units`, 1 `bitmaps` — and it is dominated
  by two things the run could not reach: the multiplayer maps' terrain
  (`carpark road_01`, the `oil rig … _blue` variants) and content further into a
  level than a camera tour goes (`skorn_mk2_1024`, `reapor 512`, `rampagor512`).
  Nothing in the pipeline drops a texture for being unseen; it only sorts one down.

#### What the profile is allowed to decide, and what it is not

**The granularity does not line up.** A draw binds one texture for a whole primitive
batch, so every number here is per *sheet*, while gkpbr's materials are per *region
within* a sheet. A 1024 atlas holding a lamp housing and eleven other things is not
"emissive" because one draw of it was additive. So:

- **It goes into `classify.build_prompt` as measured context**, phrased as counts and
  percentages of observed draws and never as a verdict, with the per-sheet caveat and
  the state-persistence caveat stated in the prompt itself. `SYSTEM` gained one rule
  telling the model to use it in one direction only: a sheet none of whose draws add
  to the framebuffer is very unlikely to hold a self-lit surface, and a sheet with a
  large additive fraction is drawn as glow sprites so its `height_scale` and `relief`
  should be near nothing. It may never conclude that a *region* emits.
- **It ranks what a run spends on.** `_select` orders by observed draw count, so an
  interrupted run has done the useful half; `--seen-only` on `classify` and `maps` is
  the operator asserting they want the rest skipped. Naming a texture explicitly
  bypasses both — being told the sheet you asked for was skipped would be absurd.
- **It is not in the fingerprint as a separate key**, because it is *in the prompt*:
  re-harvesting the game moves the rendered text and a cached answer goes stale by the
  same route a re-segmentation does, with `cache.explain` already having words for it.
  A texture with no observation produces a byte-identical prompt to the one it
  produced before the profile existed, so adding this invalidated nothing.
- **It never paints a texel.** `derive` does not read it.

**What was deliberately not done.** Skipping whole sheets that are only ever drawn
additively was the obvious action, and the measurement removed it: **zero of the 289
sheets are drawn only additively**, so the rule would have an empty candidate set. A
branch that never fires is worse than none — it is untested at runtime and reads like
a working safeguard. The number is reported by `observed` instead. Nothing is skipped
for being alpha-tested either, for the persistence reason above. And nothing derived
from the profile reaches `derive`, `metrics` or the map arithmetic at all.

#### The seven sheets drawn that the manifest does not carry

`inventory` now prints this join, for the same reason it prints the 148: a manifest
that simply omits a texture looks identical whether the game never draws it or the
walk cannot reach it.

| sheet | draws | what it is |
|---|---|---|
| `units/plates 2 1024.rim` | 947,189 | **the HUD** — portraits, health bars, weapon icons, the digit strip. The game's third most-drawn sheet, and correctly excluded |
| `bitmaps/small font.rim`, `bitmaps/large font.rim` | 41,271 | the two font sheets |
| `bitmaps/lava`, `swamp`, `oil` | 52,335 | the liquid surfaces, above |
| `bitmaps/level01.rim` | 5,445 | a level's map image on the briefing screen |

`units/plates 2 1024.rim` is worth dwelling on because it briefly looked like a hole
in the manifest — the biggest consumer of draws outside the manifest and the third
biggest in the game, behind two unit atlases the manifest does carry
(`gunlok_mk2_1024` at 1,179,804 and `elint_mk2_512` at 1,028,230), named by a `gl.exe`
literal and by no table. Decoding it settles it: it is the HUD sheet, and "the front
end, the fonts, the HUD" in the table above was right about it all along. The three
liquid surfaces are the part of that row that was wrong.

One normalisation detail, because it silently deletes a whole class otherwise: the
engine spells a bound texture the way whatever asked for it spelled one, so a
`BMPNAMES` name arrives as `units\Command Wheel 01 512.RIM` while a `.gls`-supplied
one arrives as `bitmaps\\LEVEL01.rim` with **the separator doubled** — the same
doubled-backslash trap the name search above already records. `renderstate.normalise`
collapses runs of separators; without that, every level-map bitmap reads as a texture
nothing on disk matches.

#### Re-running it

`utils/rendertest/harvest-draws.ps1`'s header is the list of things that waste a run.
The four that cost one here:

- **Four multiplayer maps crash the game on `levels.start`** — `mplay_bombsite`,
  `mplay_canyon`, `mplay_dockyard` and `mplay_tf_oilrig01`. This is
  `game_defects_notes.md` §9: they `#include` unit headers whose `.RIF` was never
  shipped, and the game's own `ToRole` dereferences the resulting null `Hierarchy`.
  It presents as a hang only because the REPL socket dies with the process; it is an
  immediate `0xc0000005` with a WER dump. **The other seven load fine** —
  `mplay_atlantic`, `mplay_carpark`, `mplay_machine`, `mplay_mountain`,
  `mplay_rorschasch`, `mplay_warehouse`, `mplay_zorro`, which are exactly the seven
  the exe hardcodes — so a re-run can take six more maps of texture coverage than
  this profile has. (This paragraph previously listed `mplay_carpark` as a hang and
  said nothing else came up; carpark loads, and the run that recorded it had already
  been killed by one of the four above.) Nothing in the campaign set faults, cutscene
  or not — a `PLAY CUTSCENE` level is *fine* for coverage, and its flying camera is a
  bonus. `level01` is in the profile for exactly that reason.
- **Do not press Escape at the top-level main menu.** It exits the game — cleanly, so
  `GLkeys.cfg` is rewritten — and takes the accumulator with it, since it lives in the
  REPL context.
- **Drive the camera with the arrow keys**, per `GLkeys.cfg`, not by assigning
  `camera.position`. Setting the position directly puts the camera where the level
  is not and the frame goes black, which looks exactly like a renderer fault;
  `camera.center_on` is no help either, because `CENTRE` takes a unit *number* and
  answers "Invalid (negative) unit number" to a name.
- **Screenshots need `GKPLUS_RENDERER=d3d9`.** Under `d3d8` the draw log is identical
  but `PrintWindow` comes back black, so a sanity check on where the camera is
  looking is impossible. The harvest itself works under either.

And one that lost a run's accumulator: iterating `actors` and calling `frag()` on
everything crashes the game. `attack_position` on every actor is a safe way to force
a firefight; `frag` is not.

### 1 · `classify` — a vision model, answering in JSON

Not the image model. Deciding that a region is painted steel with roughness around
0.4 is recognition, worth about $0.001 on `gemini-3.6-flash`, and its answer is a small
JSON document: reviewable, diffable, **editable by hand when it is wrong**, and cached
against a fingerprint of everything that produced it. That last property is what makes
the pipeline re-runnable — `derive` can be rewritten and every map regenerated with
no further model calls.

The model gets the albedo, **a colour-coded region map**, and the part names. Colours
rather than bounding boxes because a part's UV footprint is scattered: `siloa` on
`baddies3.rim` touches texels across a box of (0,192)-(1024,832) while covering about
5% of it, so the box says nearly nothing and the colour says exactly.

#### The cache key is the inputs, not the file name

This section used to say the JSON was "cacheable forever because the input never
changes", and the code behind it skipped a texture on `os.path.exists` — so the file
*name* was the whole key, and the sentence was an assertion about a world in which
four things hold still. None of them does:

- **Region ids are patch labels from `atlas.segment`.** Move `MIN_AREA_TEXELS`, the
  24-region cap, the rasterizer or the tiling test and id 3 denotes a *different*
  region. The cached answer is then silently re-pointed and `derive` paints roughness
  and metalness into the wrong texels, with no error anywhere. This is not
  hypothetical: `MIN_AREA_TEXELS` has already moved once, taking `cracks.rim` from 24
  regions to 13.
- **The albedo is not immutable either.** It comes from the addon's decoder, which is
  a separate world with its own reasons to change — `rim.py` gained the palettized
  `BODY` path in a533756 and `structures/eog_cylinder` entered the manifest as a
  result, which is the same seam `tests/test_addon_boundary.py` exists for.
- **The prompt and the response schema are inputs.** The `metallic` correction at the
  bottom of this file *is* an edit to `SYSTEM`, and an edit to `SYSTEM` invalidates
  every answer taken before it. Nothing noticed that one, and nothing would have
  noticed the next.
- **The check that would have caught the first of these ran only on the fresh path.**
  A cached answer is the only kind that can have been written against a different
  region list, so the MISSING/EXTRA reconciliation was installed on the one path where
  it had nothing to catch. It now runs on every entry, cached ones included.

So an entry carries one digest per input: the albedo PNG's bytes, the label image's
bytes, the **rendered prompt**, `SYSTEM`, the response schema, and the model name
verbatim. The rendered prompt rather than the fields behind it, because it is exactly
what the model saw — ids, part names, polygon counts, boxes and sheet statistics, in
the arrangement they were asked in. Per input rather than one rolled-up hash, because
"your prompt changed" and "the albedo changed" mean very different things to whoever
is reading the report, and telling them apart costs one more sha256 over a few
kilobytes.

**What is deliberately outside the hash matters as much**, because a fingerprint over
too much invalidates on changes that cannot alter the answer and then charges for it:

| not hashed | why |
|---|---|
| the answer body | editing a wrong roughness by hand is a supported repair — half the reason this stage answers in JSON — and hashing it would turn every fix into a stale entry demanding to be re-bought |
| `temperature` and any other sampling knob | two samples at different temperatures are both answers to the same question; a changed prompt means the cached answer answers a *different* one |
| `coverage`, `seam`, `uv_samples`, `tiling_fraction` | the model never sees them |
| `MIN_AREA_TEXELS`, `MAX_REGIONS`, the rasterizer, the tiling test | covered transitively and more precisely than a version stamp could: each either renumbers or drops a region (so the prompt moves) or repaints its texels (so the labels move), and if it does neither, the cached answer is still the right answer |
| the palette | the prompt names every region by its `colour rgb(...)`, so a palette edit already shows up in the prompt digest |

Hashing the label image's stored bytes rather than the rendered region overlay is the
same kind of choice: the overlay is a function of the labels, the patch order and the
palette, and the last two are already spelled out in the prompt — so the cheap input
covers the expensive one and a cache check stays file reads plus one prompt render,
rather than a full-resolution paint per texture.

**A mismatch reports, and does not spend.** Re-calling silently is $0.001 a texture and
about $0.40 for the set, which is cheap — and would spend the whole stage's budget on a
one-character edit to `SYSTEM` without anyone deciding to, *quietly*, which is the
property being removed. Refusing to run costs a person's attention on a run they may
not care about. So the middle: the entry is named, the input that moved is named, the
file on disk is left exactly as it was, the run exits non-zero, and a flag is what
buys a new answer.

| state | what it means | what happens |
|---|---|---|
| valid | every digest matches | reused, reconciled, silent |
| stale | at least one moved | named with what moved, left on disk, **not re-asked**; `--refresh-stale` re-classifies exactly these, `--force` everything |
| unknown | the entry has no fingerprint at all | reported, left alone, **not re-asked and not failed** — a file written before fingerprints existed is not evidence either way, and `--adopt-cached` is the operator asserting what the code cannot check |

Unknown has one piece of evidence available to it even so, and it is used: if such an
entry's region ids do not reconcile against today's patch list, it is not unknown any
more — that is exactly the failure a fingerprint would have caught, arriving through
the only check that works without one. Those are counted as stale, and `--adopt-cached`
refuses to stamp them.

**`maps` caches model output too, and it has the same problem.** The generated height
and de-lit colour PNGs are cached as their own artifacts so that editing `derive` costs
no API calls — and their prompt is built out of the *stage 1 answer*, so re-classifying
a texture, or hand-editing one material, invalidates them. Each now has a fingerprint
sidecar (albedo bytes, rendered prompt, model, and whether the seam pass ran, since a
tiling sheet is generated twice and cross-blended). A stale artifact is treated the way
a map that failed its gate is treated: **it is not used**, the derived map is, and the
note says why. `--regenerate` re-asks. `maps` also re-checks the materials fingerprint,
since that is the stage where a stale stage-1 answer does the actual damage; it reports
and exits non-zero but still writes the maps, because they are free to rewrite and
blocking a `derive` iteration loop would be the wrong trade.

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

Only **height** and **de-lighting**, under `maps --generate` and
`maps --generate --delight`. `--delight` on its own reaches no model — it uses
`derive.delight`, the arithmetic fallback, which is also what a failed gate falls
back to.

Height is the one map carrying information the albedo lacks — luminance is not depth,
a dark painted stripe is not a groove — and it is asked for as **grayscale relief**,
because a grayscale field has no validity constraint to violate.

Misregistration is handled twice. `derive.fuse_height` keeps the model's contribution
to the **low frequencies**, where drift is invisible, and takes high-frequency detail
from the albedo, which is aligned by definition. Then `metrics.accept` rejects or
un-rolls whatever drifted anyway.

De-lighting is the other one, and it is the stage whose output is hardest to check —
so what it is *for* was bounded by looking at the textures rather than by reasoning
about them. That measurement is its own section below ("Baked lighting"), and three
things came out of it: the stage stays, it stays opt-in, and its result is now applied
**only to the regions that asked for it** rather than to the whole sheet.

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

## Baked lighting: how much there is, and what `--delight` should do about it

When this was written, whether Gunlok's textures carry baked lighting was an open
question and `--delight` was scoped for the pessimistic answer. It is no longer
open, from two directions.

**The engine's side is settled.** Runtime lighting is `SHPVTINT` — baked
per-*vertex* intensities, one packed colour per vertex, 4,668 chunks across the
shipped files, and the only lighting in a `.rif` the game reads
(`rif_chunk_format.md`). The placed lights and the ambient floor in a `.rif` are
editor-time data the shipped engine ignores. The light sum itself is pinned
bit-identical against the real D3D8 runtime over all 104,693 lit pixels of a test
frame (commit 93fbdac). So the engine lights *per vertex* and multiplies its result
over whatever the artist painted — and anything painted into a sheet is therefore
the artist's shading, doubled.

**The textures' side is what this section measures**, and the answer is not the one
the stage was scoped for.

### What was looked at, and how it was chosen

The 365 albedos an `inventory` run writes. Two arithmetic proxies were computed over
all of them, **per region rather than per sheet** (an atlas fits a plane beautifully
across two unrelated materials), and used only to decide what to open:

- `plane_pp` — peak-to-peak of a least-squares plane fit to the region's luminance.
  A directional light gradient is exactly a plane. Median 0.15, p90 0.45, max 1.23.
- `shade_ratio` — std of a heavily blurred luminance over the std of what is left.
  Smooth broad tone with little detail under it is what an airbrushed form shade
  looks like; a photographic material scan has the opposite ratio.

Then **eight contact sheets of sixteen — 109 distinct sheets, 30% of the set — plus
four opened at full size**: the sixteen most-drawn of `ground`, of `units` and of
`structures` (by the observed draw counts, so the ones that are actually on screen);
the top and bottom sixteen of `plane_pp`; the top sixteen of `shade_ratio`; and —
because six of those eight are samples some metric or counter *nominated* — **two
blind systematic samples**, every 23rd name of the sorted manifest at offsets 0 and
11, which is 32 sheets drawn without reference to any of it. The four opened full
size were `gunlok_mk2_1024`, `maskelyn_mk2_512`, `dock line` and
`city ruins ground 1_a`, to check at texel scale what a 256-pixel thumbnail had
suggested.

### What the pictures say

**Ground and wall material is flat-lit, and it is photography.** Gravel, sand,
cracked mud, grass, rubble, concrete, asphalt, riveted plate. `ground/city ruins
ground 1_a` (134,396 draws over the profiled run) is a rubble scan with tyre tracks
and no light gradient at all; `gravel`, `wetsand`, `rock2`,
`ruins_messy concrete 1024`, `dock pavement` are the same. There is light and shade
at pebble scale, which is the *surface* and is exactly what the roughness and height
maps are read out of. There is nothing there for a de-lighter to remove that is not
the material.

**Unit atlases are saturated with painted light**, and this is where the stage earns
its place. `units/gunlok_mk2_1024` — the most-drawn sheet the manifest carries, at
1,179,804 draws — has torso pods airbrushed bright along a vertical axis and falling
to dark at both rims, a dome bright at the top and dark at the bottom, and cylinders
with a bright band down the centre. `units/maskelyn_mk2_512` goes further and paints
**specular highlights**: a mirror gradient across each lens, a hot line along every
ridge of the tan bodywork. `frend_mk2_512` and `elint_mk2_512` are the same
treatment. This is form shading on a curved object's unwrap, and per-vertex lighting
on the four-vertex quad it lands on could never have produced it — which is why the
artist painted it.

**Props photographed as objects carry the photograph's lighting.** `ground/dock line`
is an oil drum shot from the side: real cylindrical falloff, bright left of centre,
dark at the right edge. It will be wrapped onto a cylinder and lit again. The
distinction that matters is not photograph-vs-painting, it is *material* scan versus
*object* photograph.

**On the blind sample of 32, eight sheets carry a clearly shaded region and about
five more are arguable** — so roughly a quarter, with an upper bound near a third.
That is the number that matters for cost, because `_wants_delight` fires on a sheet
if *any* of its regions asks. Weighted by what is on screen it is much higher: units
are **43.3%** of the observed draws of manifest sheets, against ground's 39.5% and
structures' 16.6%.

### The proxies fire on the wrong thing, and that is the finding

Both metrics rank rust ahead of light. The top of `plane_pp` (1.23 down to 0.58) is
the ship-hull sheets — `hull 02`, `hull 12`, `hull 22` — whose gradient is **soot and
a waterline**, rust at the top fading to black at the bottom; and the
`s3 level 1k 17/18_chute` pair, whose gradient is **fire glow at the bottom of a
lava chute**, which is emissive and not incident. The top twenty of `shade_ratio` is
sixteen `ground` sheets of exactly that kind. And `units` — the group that plainly
carries the most painted light — has the *lowest* median `shade_ratio` of any group
(0.35 against ground's 0.43), because those atlases carry heavy detail alongside the
airbrush and the ratio cannot see past it.

So no arithmetic here answers the question, and that is an argument **for** stage 1
rather than against it: separating a light gradient from a dirt gradient is
recognition, which is what the classifier is for. What the arithmetic is good for is
choosing what to open, which is all it was used for.

### What changed

1. **`--delight` stays, and stays opt-in** — now for a stronger reason than caution.
   There are two consumers with opposite needs. A modern per-pixel PBR renderer
   wants the form shade removed, because it will compute one. **Gunlok itself does
   not**, and it is now a reachable consumer ("Putting a map on screen" below): its
   per-vertex lighting cannot put a cylinder's painted falloff back onto the quad it
   was painted for, so de-lighting a unit atlas and serving it to the game makes it
   *worse*. Opt-in is therefore a statement about the consumer, not a way to save
   money, and the flag's help says so.
2. **`SYSTEM`'s `delight` rule is rewritten.** It used to say "visible shadows, a
   gradient from a light direction, ambient occlusion in crevices", which is a
   description that fires on every one of the false positives above — this set's
   gradients are far more often dirt than light. It now names the true positive
   (smooth form shading or a specular highlight on a region that is one curved
   object's unwrap) and rules out the three things that look like it: rust, soot,
   waterline, stain and paint-fade gradients; heat glow, which is emissive; and the
   pebble-scale shading inside a photograph of a flat material. It also states the
   per-vertex fact, so the model knows *why* a form shade is worth removing.
3. **The result is applied per region** (`derive.delight_mask` / `apply_where`). It
   was whole-sheet: one shaded pod on a 24-region atlas sent the entire sheet through
   the de-lighter, including twenty-three flat-lit photographic plates that had
   nothing to lose but material contrast. Given that the shading is a minority of
   regions *on the sheets that have any*, this was the most consequential thing about
   the stage. The request is still whole-sheet — the model needs the whole picture to
   read it — and only the result is masked.
4. **The cost table's framing was wrong** and is corrected below.

Editing `SYSTEM` invalidates every cached stage-1 answer, by exactly the mechanism
"The cache key is the inputs, not the file name" describes. That is the intended
behaviour and not an accident of this edit: `classify` will name them stale, spend
nothing, and exit non-zero until `--refresh-stale` buys new ones.

## Putting a map on screen

Every map here is a PNG nothing has ever looked at in place. The gates say a height
map is *registered* to its albedo; they cannot say a normal map's relief points the
right way, and no arithmetic here can. Two pieces of GkPlus make looking cheap — the
mod filesystem (`src/Vfs`) will serve any file the engine opens, and
`utils/rendertest` drives the game over the REPL and photographs it — so there is a
short path from a generated PNG to pixels, and it is one command:

```
uv run python -m gkpbr.cli maps    "ground/gunlok rust.rim"          # derived only, no API
uv run python -m gkpbr.cli preview "ground/gunlok rust.rim" --map normal
#   ... launch, look ...
uv run python -m gkpbr.cli preview --remove
```

`preview` packs the map as a `.RIM` through `utils/rimutil` and drops it into
`<Gunlok>\gkplus\mods\gkpbr-preview\Graphics\...`, **replacing the sheet it was
derived from**. Then, with `GKPLUS_REPL_PORT` set (`utils/rendertest`):

```powershell
. .\utils\rendertest\shoot-settled.ps1
Shoot-Settled -Renderer d3d9 -Level level02 -Out shot.png
```

`--remove` is not optional and not a nicety. A leftover mod goes on replacing an
asset in every later session and the game looks *fine* — which is the failure commit
6655629 spent a session chasing, and why this install still has a leftover
`rimutil-body-test` mod for someone to trip over.

### Why a mod and not `render.material.override`

The override was the obvious candidate and it cannot do this. It re-points every draw
sampling one **loaded** image at another **loaded** image, keyed on a case-insensitive
substring of the `.rim` path; it has no way to introduce an image the engine never
loaded, and a PNG on disk is exactly that. A generated map has to enter through the
asset loader, and `src/Vfs` is the only seam into it (`mod_loading_notes.md`).

Two consequences are worth having anyway. The swap works under **every** renderer,
which matters because screenshots need `GKPLUS_RENDERER=d3d9` — under `d3d8`
`PrintWindow` comes back black. And it is what a mod shipping these maps would
actually do, so what is on screen is the real thing.

`material_override` is still useful *after* this and only after: with the mod
installed both sheets are loaded, so an override can A/B them inside one session
under Vulkan. The honest A/B is still mod-in against mod-out, which is what the
result below is.

### What was measured

level02, `d3d9`, camera settled (`Shoot-Settled`), the same rest position to the last
digit across all five runs. `mods.served` read 1 and `mods.recent` named
`Graphics/Ground/gunlok rust.RIM` exactly, which is what turns "the game looks
different" into "the game loaded my file".

`ground/gunlok rust.rim` is the tunnel mouth that fills the top-left quarter of
level02's opening shot — **25.1% of the frame changed** when it was swapped. (The
first attempt used `ground/city ruins ground 1_a.rim`, which the profile says is
drawn 134,396 times over the whole run and which the settled frame's own draw list
confirms is on screen — 7 draws, 148 primitives. It moved **234 pixels**. Draw
counts are not screen area, primitive counts are not either, and picking the target
off the profile alone wasted a run. `render.debug.frame_draws()` at the rest position is
what to read, and even that only ranks candidates.)

| what was served | what was on screen |
|---|---|
| `normal`, `body` | The tunnel reads as a tangent-space normal map: lavender where the surface is flat, navy on the tilted rim, and the diagonal streaks of the map line up with the rust streaks of the stock texture — the registration check that no metric can make. The interior is a **wall of full-amplitude colour speckle** |
| `height`, `dxt1` | A clean smooth grey-green relief field with the same layered structure as the albedo, and no speckle anywhere. The green is the engine's own per-vertex diffuse multiplying over it, which is `SHPVTINT` visible in one picture |
| `color`, `dxt1` (the control) | Indistinguishable from stock: 1.6% of the frame past a threshold of 8/255, mean absolute error **0.57** over the frame and **1.73** on the tunnel mouth itself, all of it DXT1 quantisation |

### Which traps bit

- **The empty `LIST:MIPM` did not bite the albedo, and it is not what made the normal
  map speckle.** That is what the `color` control run is for: the same pixels through
  the same `rimutil` path, mip-less, render as stock. The speckle is **content** — and
  it is a real finding about this pipeline rather than about the container.
  `derive.height_from_albedo` puts 35% of its weight on a σ=4 detail band, so the
  height field varies texel to texel; `normal_from_height` then takes its gradient
  with `strength=2.0`, which turns that into full-amplitude normals. Sample that
  minified with no mip level to fall back on and the result is noise. The height map
  from *the same field* renders clean, which separates the two causes: it is not that
  mips are missing, it is that mips are missing **and** the map has nothing but
  texel-scale content. `maps --generate` would soften it rather than fix it —
  `fuse_height` still mixes the albedo's σ=4 detail back in at a quarter weight, on
  purpose, because that is the term that keeps a generated map registered. Whether a
  mip chain, a smoothed height for the normal, or both is the right answer is not
  decided here; what is decided is that the derived normal map is not usable at
  minification and that until now nothing said so.
- **DXT1 versus DXT3 is not a decision for a map with no alpha.** They came back
  **byte-identical in RGB** on this normal map, because the two share a block
  encoding and differ only in alpha. So "use DXT3, it is higher quality" buys nothing
  at twice the size. The real choice is S3TC or not:

  | encoding | normal-vector error, mean / p99 / max |
  |---|---|
  | DXT1 and DXT3, identical | 2.53° / 8.27° / 17.25° |
  | `body` on a 16-bit surface (R5G6B5) | 1.15° / 1.89° / 2.33° |

  S3TC's error is not spread evenly — it concentrates in whichever 4×4 blocks
  straddle a gradient, which is where the relief is, hence the long tail. `body` on
  this map is 13 planes and 5,636 colours (a normal map from a smooth height field
  has very few distinct values), 1.6 MB against 0.5 MB. So `preview` defaults
  `normal` to `body` and everything else to `dxt1`, where 1.03/255 mean error on a
  one-channel map is nothing.
- **`body` is lossless on disk and not on screen.** An uncompressed image lands on
  whatever `ChooseSurfaceFormatForImage` picks and the 32-bit candidates are gated on
  `Use32BitTextures`, which is 0 in a retail build — so the table's second row assumes
  R5G6B5 and is the pessimistic reading.
- **The `ALPH` defect did not bite and would have.** These maps have no alpha, so
  nothing was lost; `--map color` on one of the four manifest sheets that *do* have
  alpha, packed as `body`, would load fully opaque, and `preview` says so rather than
  finding out on screen. `rimutil` refuses graded alpha under `--format body` on its
  own; the case it cannot see is this one, where the operator chose the encoding.
- **DXT5 never came up**, because `rimutil` refuses it by name. It is worth knowing it
  is the one that fails *silently*: the engine drops the fourcc and renders such a
  file with garbage alpha.

## Normal-map convention

`derive.normal_from_height` takes `green_down` and **refuses to guess**. RIF's V grows
downward and the addon flips it on import, so a map written in image space has
+G = "up in the image", which is −V in RIF and +V after the addon's flip. Getting it
backwards inverts lighting on one axis everywhere — subtle enough to survive review
and miserable to find later. Default is OpenGL/Blender (+G up); `--green-down` gives
the DirectX convention.

## Cost

Cost is not the constraint, which is worth knowing before optimising for it. At
current list prices, across all ~365 textures:

| Stage | Model | Per texture | Total |
|---|---|---|---|
| `classify` | `gemini-3.6-flash` | ~$0.001 | ~$0.40 |
| `maps --generate` (1K height) | `gemini-3.1-flash-image` | $0.067 | ~$24 |
| `maps --delight` (1K) | `gemini-3.1-flash-image` | $0.067 | **~$7**, not ~$24 |
| seam-repair second pass | as above | $0.067 | ~$1.90 (29 textures) |
| escalation on a failed gate | `gemini-3-pro-image` | $0.134 | per failure |

The whole run is tens of dollars either way, which argues for escalating to the pro
image model on anything that fails a gate rather than budgeting the cheap one. The
seam-repair line is nearly free only because the UV measurement above shrank it from
99 textures to 29 — and of those 29, only 6 are wrap-sampled; the other 23 are sheets
that segment into no region and are treated as one surface.

**The `--delight` row used to read ~$24 and to be described as half the run's cost,
and both were wrong.** It assumed every texture, and that stage is gated twice: on
the flag, and then per sheet on any of its regions answering `delight: true`. The
pictures put the ceiling on the second gate at about a third of the set and the
estimate at a quarter (see "Baked lighting" above), so the row is single-digit
dollars and the stage is the cheapest of the three model stages rather than the
joint-largest. Cost was never the reason to think about it; **what** it should
remove was, and that is what the measurement changed.

The render profile touches this table in one place, and it is ordering rather than
saving: `_select` puts the sheets the game demonstrably draws first, so an interrupted
`maps --generate` has spent its $24 on the ones that appear on screen. `--seen-only`
would cut the set from 365 to 282, but that is the operator's call and not the
default — 83 unseen is a property of one run, not of the game.

Stage 1 is what makes re-runs free: its JSON is cached per texture against a digest of
everything that produced it, so `derive` can be rewritten and every map regenerated
with no model calls — and an input that *did* move is named rather than skipped over.
See "The cache key is the inputs, not the file name" above for what is hashed, what is
deliberately not, and why a mismatch reports rather than quietly re-spending the first
row of this table.

## What the probe measured

`probe` asks for a height map and prints the gate numbers. It is the measurement the
design rests on, and it has now been run — ten textures against
`gemini-3.1-flash-image`.

**Registration holds.** Every accepted result came back at shift `(0, 0)`. The model
does not crop, pad or re-compose, so the fear that drove the whole low-frequency-only
design did not materialise in its strong form. The `fuse_height` mitigation stays,
because it costs nothing and the sample is ten textures rather than 365.

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
