"""Stage 3: the image model, used for the two things arithmetic cannot do.

Only **height** and **de-lighting** are generated. Everything else is derived from
the albedo in :mod:`derive`, because a derived map is registered and tiling by
construction and a generated one is neither.

Height is asked for as **grayscale relief, never as an RGB normal map**. Two reasons,
and both are structural rather than a matter of prompt quality: a normal map has to
hold unit vectors in a specific tangent basis, which nothing constrains a model's RGB
output to do, and an invalid normal map lights as noise; whereas a grayscale height
field has no validity constraint at all, and :func:`derive.normal_from_height` turns
it into a guaranteed-valid normal map with the gradient wrapping so the result tiles.

Misregistration is handled twice over. :func:`derive.fuse_height` keeps the model's
contribution to the low frequencies where drift is invisible, and
:func:`metrics.accept` rejects or un-rolls anything that drifted anyway. Seams on a
tiling texture are handled by generating a second time from a half-offset albedo and
cross-blending, which puts an interior region over every edge.
"""

import base64
import os

from . import images, metrics

#: `gemini-3.1-flash-image` is the balance point: $0.067 per 1K image against
#: `gemini-3-pro-image` at $0.134 and `gemini-3.1-flash-lite-image` at $0.034.
#: At 364 textures the whole run is tens of dollars either way, so the model is
#: chosen on result quality and escalated per-texture, not budgeted for.
DEFAULT_MODEL = "gemini-3.1-flash-image"
ESCALATION_MODEL = "gemini-3-pro-image"

HEIGHT_PROMPT = """\
Convert this game texture into a grayscale HEIGHT MAP (a displacement map).

White is the highest surface, black is the deepest. Output a grayscale image only.

Critical requirements:
- The output must be pixel-aligned with the input. Every feature must stay at
  exactly the same position and the same size. Do not crop, pad, rotate, rescale,
  re-centre or re-compose anything.
- Encode GEOMETRY, not brightness. A dark painted marking on a flat panel is flat,
  so it stays mid-grey. A groove, seam, crack or recess is dark because it is
  deeper. A rivet, weld or raised boss is light because it protrudes. Ignore
  shadows and lighting already painted into the image.
- Keep it smooth and low-contrast overall. Only real geometric relief should
  deviate far from mid-grey.

The surface is: %s
The relief to show is: %s\
"""

DELIGHT_PROMPT = """\
Remove baked-in lighting from this game texture to recover its flat ALBEDO
(base colour).

Remove directional shading, cast shadows, ambient occlusion in crevices, and any
overall brightness gradient. Keep the material's own colours, markings, dirt,
rust and wear exactly as they are -- those belong to the albedo. The result should
look uniformly lit, as if photographed flat.

Critical requirement: the output must be pixel-aligned with the input. Every
feature stays at exactly the same position and size. Do not crop, pad, rotate,
rescale, re-centre or re-compose anything.

The surface is: %s\
"""


#: **The API returns JPEG only.** Asking for ``image/png`` is a 400 with
#: "Supported values: 'image/jpeg'". It costs a little: a height map comes back with
#: JPEG ringing, and DCT artifacts are high-frequency, which is exactly the band
#: :func:`derive.fuse_height` discards in favour of the albedo's own detail -- so the
#: loss lands where it is thrown away rather than where it is used. The *input* is
#: still PNG; only the response format is constrained.
OUTPUT_MIME = "image/jpeg"


def _call(client, model, prompt, png, size="1K"):
    """One image-out request. Returns the first image in the response, as bytes."""
    interaction = client.interactions.create(
        model=model,
        input=[
            {"type": "text", "text": prompt},
            {"type": "image",
             "data": base64.b64encode(png).decode("ascii"),
             "mime_type": "image/png"},
        ],
        response_format={"type": "image", "mime_type": OUTPUT_MIME,
                         "aspect_ratio": "1:1", "image_size": size},
    )
    out = getattr(interaction, "output_image", None)
    if out is not None and getattr(out, "data", None):
        return base64.b64decode(out.data)
    said = []
    for step in getattr(interaction, "steps", []) or []:
        for block in getattr(step, "content", []) or []:
            kind = getattr(block, "type", None)
            if kind == "image" and getattr(block, "data", None):
                return base64.b64decode(block.data)
            if kind == "text" and getattr(block, "text", None):
                said.append(block.text.strip())
    # An image request that comes back with prose is a refusal or a clarifying
    # question, and the text is the only thing that says which. Swallowing it turns a
    # one-line explanation into an unexplained retry, which happened once here.
    raise RuntimeError("no image from %s%s" % (
        model, ": " + " / ".join(said)[:300] if said else " (and no text either)"))


def _client(client=None):
    from google import genai
    return client or genai.Client(api_key=os.environ["GEMINI_API_KEY"])


def _request_size(width, height):
    """The smallest offered size that does not throw away detail.

    The accepted values are ``512``/``1K``/``2K``/``4K`` -- **the smallest is the
    literal string ``512``, not ``0.5K``**, which is what the published docs call it
    and what the API rejects with a 400. The aspect ratio must also come from a fixed
    list, so a non-square sheet would be re-composed rather than mapped; that is
    moot here because all 364 referenced sheets are square.
    """
    n = max(width, height)
    if n <= 512:
        return "512"
    if n <= 1024:
        return "1K"
    if n <= 2048:
        return "2K"
    return "4K"


def height(albedo, material_summary, relief_summary, *, require_tiling=False,
           model=DEFAULT_MODEL, client=None, escalate=True, want_raw=False):
    """A grayscale height field for a whole sheet.

    Returns ``(array, note)``, or ``(array, note, raw)`` when ``want_raw``, where
    ``raw`` is the last model output whether or not it passed -- so a caller can save
    a rejected map for inspection instead of only recording that it scored badly.

    ``array`` is ``None`` when every attempt failed its gate; the caller then falls
    back to :func:`derive.height_from_albedo`, which is why a failure here is a
    quality regression and never a broken run.
    """
    client = _client(client)
    h, w = albedo.shape[:2]
    size = _request_size(w, h)
    prompt = HEIGHT_PROMPT % (material_summary, relief_summary)

    attempts = [(model, require_tiling)]
    if escalate:
        attempts.append((ESCALATION_MODEL, require_tiling))

    last = "not attempted"
    raw = None
    for use_model, seamless in attempts:
        try:
            direct = _fetch(client, use_model, prompt, albedo, w, h, size)
        except Exception as exc:  # noqa: BLE001
            last = "%s: %s" % (use_model, exc)
            continue

        if seamless:
            try:
                rolled = _fetch(client, use_model, prompt, images.roll_half(albedo),
                                w, h, size)
                direct = images.blend_seamless(direct, images.roll_half(rolled))
            except Exception as exc:  # noqa: BLE001
                last = "%s: offset pass failed: %s" % (use_model, exc)
        raw = direct

        ok, corrected, why = metrics.accept(
            metrics.luminance(albedo), direct, require_tiling=require_tiling)
        if ok:
            note = "%s (%s)" % (use_model, why)
            return (corrected, note, raw) if want_raw else (corrected, note)
        last = "%s rejected: %s" % (use_model, why)

    return (None, last, raw) if want_raw else (None, last)


def delit_albedo(albedo, material_summary, *, model=DEFAULT_MODEL, client=None):
    """A de-lit albedo, or ``None`` with a note. Gated the same way."""
    client = _client(client)
    h, w = albedo.shape[:2]
    try:
        got = _fetch(client, model, DELIGHT_PROMPT % material_summary, albedo, w, h,
                     _request_size(w, h), gray=False)
    except Exception as exc:  # noqa: BLE001
        return None, "%s: %s" % (model, exc)

    ok, corrected, why = metrics.accept(metrics.luminance(albedo),
                                        metrics.luminance(got))
    if not ok:
        return None, "rejected: %s" % why
    # The gate ran on luminance; apply the same correction to the colour image.
    if corrected.shape != got.shape[:2]:
        return got, why
    return got, why


def _fetch(client, model, prompt, albedo, width, height_px, size, gray=True):
    """One call, brought back to the source resolution as a float array."""
    raw = _call(client, model, prompt, images.to_png_bytes(albedo), size=size)
    arr = images.load_bytes(raw)
    if arr.shape[0] != height_px or arr.shape[1] != width:
        arr = images.resize(arr, width, height_px)
    return metrics.luminance(arr) if gray else arr
