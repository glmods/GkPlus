"""Stage 1: what *is* this material? A vision model answering in JSON.

This is deliberately not the image model. Deciding that a region is painted steel
with a roughness around 0.4 is a recognition task worth about $0.001 on a text
model, and its answer is a small JSON document -- reviewable, diffable, editable by
hand when it is wrong, and cached against a fingerprint of everything that produced
it. That last property is what makes the pipeline re-runnable: :mod:`derive` can be
rewritten and every map regenerated with no further model calls, and an input that
did move is named rather than skipped over. The inputs are :func:`fingerprint`; what
a mismatch does is ``cli.cmd_classify``; why either exists is :mod:`gkpbr.cache`.

Each atlas patch is described to the model with the thing the image alone cannot
supply: **the names of the parts that use it**, out of ``OBJHEAD1``. "This rect is
used by COMPOUND C and silo_large" is the context that turns a guess into a
reading.
"""

import json
import os

import numpy as np

from . import cache, renderstate

#: Visually distinct fills for the region map, one per region up to
#: :data:`atlas.MAX_REGIONS`. Described to the model as RGB triples rather than by
#: name, because "teal" and "cyan" are the same region to a reader and two
#: different ones to a prompt.
PALETTE = [
    (255, 0, 0), (0, 255, 0), (0, 80, 255), (255, 255, 0),
    (255, 0, 255), (0, 255, 255), (255, 128, 0), (128, 0, 255),
    (0, 128, 0), (128, 0, 0), (0, 0, 128), (128, 128, 0),
    (255, 128, 128), (128, 255, 128), (128, 160, 255), (255, 255, 160),
    (255, 160, 255), (160, 255, 255), (200, 100, 0), (100, 0, 200),
    (0, 200, 100), (200, 0, 100), (100, 200, 0), (160, 160, 160),
]


def colours_for(patches):
    """``{label: rgb}``, without painting anything.

    Split out of :func:`region_map` because the prompt names each region by its
    colour, and a cached entry has to be *checked* against today's prompt without
    rendering a 1024x1024 overlay to do it -- the overlay is only built when the
    model is actually going to be called.
    """
    return {p.label: PALETTE[i % len(PALETTE)] for i, p in enumerate(patches)}


def region_map(labels, patches, width, height):
    """A flat-colour image of the regions, plus ``{label: rgb}``.

    This replaces bounding boxes as the way a region is pointed at. A part's UV
    footprint is usually *scattered* -- ``siloa`` on ``baddies3.rim`` touches texels
    across a box of (0,192)-(1024,832) while covering 5% of it -- so a box tells the
    model almost nothing about where to look, whereas a colour tells it exactly.
    """
    out = np.zeros((height, width, 3), dtype=np.float32)
    colours = colours_for(patches)
    for patch in patches:
        rgb = colours[patch.label]
        out[labels == patch.label] = np.array(rgb, dtype=np.float32) / 255.0
    return out, colours


#: The response schema. Given to the model as a schema *and* described in the
#: prompt, because a field the model understands the purpose of is answered better
#: than one it merely fills in.
#:
#: Regions are a **list carrying an ``id``**, not an object keyed by id. The latter
#: is the natural shape and it does not work: keying by a dynamic id needs
#: ``additionalProperties``, and the Developer API rejects that outright with
#: "additionalProperties is only supported in Gemini Enterprise Agent Platform mode".
PATCH_SCHEMA = {
    "type": "object",
    "properties": {
        "id": {"type": "string"},
        "material": {"type": "string"},
        "roughness": {
            "type": "array",
            "items": {"type": "number"},
            "minItems": 2,
            "maxItems": 2,
        },
        "roughness_drives": {
            "type": "string",
            "enum": ["luminance", "inverse_luminance", "detail", "saturation", "constant"],
        },
        "metallic": {"type": "number"},
        "height_scale": {"type": "number"},
        "relief": {"type": "string"},
        "delight": {"type": "boolean"},
        "emissive": {
            "type": "object",
            "properties": {
                "present": {"type": "boolean"},
                "hue": {"type": "string", "enum": ["r", "g", "b", "any"]},
                "threshold": {"type": "number"},
                "strength": {"type": "number"},
            },
            "required": ["present"],
        },
        "confidence": {"type": "number"},
    },
    "required": ["id", "material", "roughness", "roughness_drives", "metallic",
                 "height_scale", "relief", "delight", "emissive", "confidence"],
}

RESPONSE_SCHEMA = {
    "type": "object",
    "properties": {
        "regions": {"type": "array", "items": PATCH_SCHEMA},
    },
    "required": ["regions"],
}

SYSTEM = """\
You are labelling textures from Gunlok, a 2000 real-time strategy game, so that
physically-based material maps can be derived from them. The textures are 256-1024
pixel sheets, often atlases packing several unrelated materials.

For each region you are given, answer with the material's PBR parameters. Rules:

- `roughness` is a [min, max] range in 0..1 that the region's roughness should span.
  Polished metal is around [0.1, 0.3]; painted or anodised metal [0.3, 0.55];
  concrete, rock and dirt [0.6, 0.95]; rubber and cloth [0.7, 0.9]; glass [0.02, 0.15].
- `roughness_drives` names which albedo signal maps onto that range. Use
  `inverse_luminance` when bright areas are worn/polished, `luminance` when bright
  areas are dusty or chalky, `detail` when roughness tracks surface texture rather
  than tone, `saturation` for painted-over-bare-metal, `constant` for a uniform
  surface.
- `metallic` must be exactly 1.0 or exactly 0.0 in almost every case. It is not a
  confidence or a "how metal does this look" score: it selects between two different
  reflectance models, and a value in between is physically meaningless for a single
  material. Answer 1.0 for bare or scratched metal, 0.0 for paint, plastic, rubber,
  glass, concrete, rock and dirt -- **painted metal is 0.0**, because the paint is
  what light hits. Use an intermediate value only when one region genuinely mixes
  bare and covered metal across its pixels, such as paint flaking off steel, and
  then set it to the approximate *area fraction* that is bare.
- `height_scale` in 0..1 is how much real relief the surface has: 0.05 for smooth
  painted panels, 0.4 for riveted plate or brick, 0.9 for rubble and rock.
- `relief` is one short phrase describing the geometric detail a height map should
  show for this material -- "horizontal panel seams and rivet heads", "irregular
  cracked mud", "none, smooth". It is the prompt an image model will be given.
- `delight` is true only when a LIGHT SOURCE is painted into this region: smooth
  airbrushed shading that follows a curved form, a specular highlight running
  along a cylinder or across a dome, a cast shadow, or ambient occlusion tucked
  into a crevice. In this game that is almost always one region that is a single
  curved object's unwrap -- a pod, a barrel, a pipe, a limb -- and the surfaces
  around it on the same sheet are flat-lit and must be answered false.
  These are NOT baked lighting and must be answered false, because they are the
  material and removing them destroys it:
  * rust, soot, grime, a waterline, a stain, a scorch or a paint fade, however
    strong the top-to-bottom or side-to-side gradient they make. A gradient is
    much more often dirt than light on these sheets;
  * a glow from something hot or burning, which is emissive, not incident;
  * the shading inside a photograph of a flat material -- gravel, sand, cracked
    mud, concrete, grass. The pebble-scale light and shade in a material scan is
    the surface, and it is what the roughness and height maps are read out of.
  The engine lights this geometry PER VERTEX and multiplies that over the
  texture, so a form shade painted into the texture really is doubled -- but it
  is only worth removing where it is a form shade.
- `emissive.present` is true only for surfaces that actually emit light: lamps,
  screens, glowing vents, hot metal. Set `hue` to the emitting colour's dominant
  channel and `threshold` to the 0..1 luminance above which a pixel is emitting.
- Some sheets come with a "Measured in the running game" block. Those numbers are
  the engine's own draw calls, so they are facts and not opinions -- but they are
  facts about the WHOLE SHEET, and you are answering per region. Use them as
  evidence, in this direction only: a sheet none of whose draws add to the
  framebuffer is very unlikely to contain a self-lit surface, so prefer
  `emissive.present` false there unless the image plainly shows a lamp or a screen;
  a sheet with a large additive fraction is drawn as glow or effect sprites, so a
  bright region on it probably *is* emitting and its `height_scale` and `relief`
  should be near-nothing, because an effect sprite has no geometry. Never conclude
  that a specific region emits merely because the sheet was blended.
- `confidence` in 0..1. Be honest; a low-confidence region falls back to a safe
  neutral material rather than to your guess.

Return one entry per region id you are given, each carrying that id in its `id`
field. Answer for every region; do not omit any.\
"""

#: A text/vision model, not an image one -- stage 1 answers in JSON. Verified
#: against ``client.models.list()``: there is no plain ``gemini-3-flash``, only
#: ``gemini-3-flash-preview``, and this is the newest non-preview flash.
DEFAULT_MODEL = "gemini-3.6-flash"


#: How the ``.RIM`` stored the image, said in words. ``format`` is a fourcc for an
#: S3TC texture but the literal ``BODY`` for a palettized one, which is not a
#: format name and means nothing to a model -- what is worth telling it is what the
#: storage did to the pixels, since block artifacts are not surface detail.
FORMAT_PHRASES = {
    "DXT1": "DXT1 block-compressed",
    "DXT3": "DXT3 block-compressed",
    "BODY": "palettized, not block-compressed",
}


def _format_phrase(record):
    """The stored format, tolerating a manifest written before the addon's rename.

    ``rim.Texture.fourcc`` became ``format`` when the palettized ``BODY`` form was
    added, and the manifest key followed it. An older ``manifest.json`` is otherwise
    identical, so it is read rather than rejected -- there is nothing about it to
    reject, and re-running `inventory` to regenerate one walks all 563 ``.rif``s.
    """
    fmt = record.get("format") or record.get("fourcc") or "unknown format"
    return FORMAT_PHRASES.get(fmt, fmt)


def build_prompt(record, patches, colours=None, observed=None):
    """The text half of a request: the sheet's statistics and its regions.

    ``observed`` is this sheet's row of :mod:`gkpbr.renderstate` -- what the engine's
    own draw calls did with it -- and it goes in as **measured context beside the
    images, not as an answer**. It is per-sheet and the regions are per-region, so it
    can support "no part of this sheet is drawn as a glow" and can never support "this
    region is a lamp". :func:`renderstate.describe` is where that is phrased, and the
    block lands after the sheet statistics and before the region list, so a texture
    with no observation produces exactly the prompt it produced before.
    """
    lines = [
        "Texture: %s" % record["name"],
        "Size: %dx%d, %s%s" % (record["width"], record["height"], _format_phrase(record),
                               ", has alpha" if record.get("alpha") else ""),
        "Referenced by %d .rif files, %d polygons." % (
            record.get("rif_count", 0), record.get("polys", 0)),
        "Mean luminance %.2f, mean saturation %.2f of 1.0." % (
            record.get("lum_mean", 0) / 255.0, record.get("sat_mean", 0)),
        "",
    ]
    measured = renderstate.describe(observed)
    if measured:
        lines += measured + [""]
    if not patches:
        lines += [
            "The game samples this sheet as one surface, not as an atlas. There is "
            'one image. Answer for a single region with id "0" covering all of it.',
        ]
        return "\n".join(lines)

    lines += [
        "The game samples this sheet as an ATLAS. You are given two images: first "
        "the texture, then a region map of the same size.",
        "",
        "The region map was built by rasterizing every UV triangle in the game's "
        "geometry that samples this sheet, grouped by the model part that samples "
        "it. Each flat colour marks the pixels one part actually uses; black is "
        "unused. The part names are the artist's own, from the .rif object headers.",
        "",
    ]
    for patch in patches:
        rgb = (colours or {}).get(patch.label)
        where = "colour rgb%s" % (tuple(rgb),) if rgb else "box %s" % (tuple(patch.box),)
        parts = ", ".join(patch.parts[:6]) if patch.parts else "unnamed"
        lines.append('id "%d": %s, part "%s", %d polygons across %d files, '
                     "bounding box %s"
                     % (patch.label, where, parts, patch.polys, patch.rifs,
                        tuple(patch.box)))
    lines += [
        "",
        'Also answer for id "0", which covers every pixel not marked above: the '
        "unused margins and any part too small to list.",
    ]
    return "\n".join(lines)


def fingerprint(albedo_png, label_png, prompt, model):
    """Everything that determines this texture's classification, one digest each.

    What is in it, and why each:

    - ``albedo`` -- the PNG bytes as handed to the model, not the ``.RIM`` on disk.
      That is what covers a decoder change in the addon, which has happened once
      already, without this module knowing anything about decoders.
    - ``labels`` -- the label PNG's **stored bytes**, rather than the rendered
      region overlay. The overlay is a function of the labels, the patch order and
      the palette; the first is this digest and the last two are already spelled out
      in the prompt, which names every region by its ``colour rgb(...)``. Hashing the
      cheap thing that covers the expensive one keeps a cache check to file reads.
    - ``prompt`` -- the **rendered** prompt, which is exactly what the model saw, and
      a far better input than the fields it is built from: it carries the region ids,
      the part names, the polygon counts, the boxes, the sheet's size and format and
      its luminance/saturation means, in the arrangement they were actually asked in.
    - ``system``, ``schema``, ``model`` -- the rules, the shape of the answer, and
      who answered.

    And what is deliberately left out, because a fingerprint over too much spends
    money re-asking on changes that cannot alter the answer:

    - **The answer itself.** Editing a wrong roughness by hand is a supported repair
      -- it is half the reason this stage answers in JSON -- and hashing the body
      would turn every such fix into a stale entry demanding to be re-bought.
    - **``temperature`` and any other sampling knob.** Two samples at different
      temperatures are both answers to the same question; a changed prompt means the
      cached answer answers a *different* question. Only the second is worth $0.001
      per texture to correct.
    - **The manifest fields the prompt does not carry** -- ``coverage``, ``seam``,
      ``uv_samples``, ``tiling_fraction``. The model never sees them.
    - **``atlas.MIN_AREA_TEXELS``, ``MAX_REGIONS``, the rasterizer and the tiling
      test.** These are the ones that re-point a region id at different texels, and
      they are covered *transitively and more precisely* than a version stamp could:
      changing one either drops or renumbers a region (so ``prompt`` moves) or repaints
      the texels (so ``labels`` moves), and if it does neither then the cached answer
      is still the right answer and re-asking would be waste.
    """
    return {
        "albedo": cache.digest(albedo_png),
        "labels": cache.digest(label_png) if label_png is not None else cache.ABSENT,
        "prompt": cache.digest(prompt),
        "system": cache.digest(SYSTEM),
        "schema": cache.digest_json(RESPONSE_SCHEMA),
        # Verbatim rather than hashed: it is short, and a report that says
        # "gemini-3.6-flash -> gemini-3.7-flash" says more than one that says two
        # hashes differ.
        "model": model,
    }


def classify(record, patches, albedo_png, region_png=None, colours=None,
             model=DEFAULT_MODEL, client=None, prompt=None, observed=None):
    """One texture -> ``{region id: material spec}``.

    Raises rather than returning a partial answer: a missing region is better handled
    by :data:`derive.DEFAULT_MATERIAL` downstream than by a silently truncated dict.

    ``prompt`` is accepted already rendered so that the caller can fingerprint the
    exact string it is about to send. Building it twice would work today and would
    stop being true the moment anything in :func:`build_prompt` is not a pure
    function of its arguments.
    """
    from google import genai
    from google.genai import types

    client = client or genai.Client(api_key=os.environ["GEMINI_API_KEY"])
    parts = [types.Part.from_bytes(data=albedo_png, mime_type="image/png")]
    if region_png is not None:
        parts.append(types.Part.from_bytes(data=region_png, mime_type="image/png"))
    parts.append(build_prompt(record, patches, colours, observed)
                 if prompt is None else prompt)

    response = client.models.generate_content(
        model=model,
        contents=parts,
        config=types.GenerateContentConfig(
            system_instruction=SYSTEM,
            response_mime_type="application/json",
            response_schema=RESPONSE_SCHEMA,
            temperature=0.2,
        ),
    )
    data = json.loads(response.text)
    # Back to a dict keyed by region id, which is what `derive` indexes by. The wire
    # shape is a list only because the API will not take a dynamically-keyed object.
    out = {}
    for entry in data.get("regions", []) or []:
        rid = str(entry.pop("id", "")).strip()
        if rid:
            out[rid] = entry
    return out
