"""Maps built from the albedo and a per-patch material description.

Everything here is deterministic arithmetic on the albedo's own pixels, which buys
two properties no generated map has: it is **registered by construction** (the
detail comes from the very pixels it describes) and it **tiles exactly as well as
the albedo does** (a per-pixel function of a tiling image tiles). That is why this
is the fallback whenever a gate in :mod:`metrics` rejects a model result, and why
roughness, metalness and emissive are derived here rather than generated at all.

The model's job is the part arithmetic cannot do: deciding *what the material is*
(:mod:`classify`) and supplying relief the albedo does not imply
(:mod:`generate`). Luminance is not depth -- a dark painted stripe is not a groove
-- so a height map derived here is a weak stand-in, and is marked as such.

**The normal map's green channel is a convention, not a detail.** RIF's V grows
downward and the addon flips it on import, so a map written in image space has
+G = "up in the image" which is -V in RIF and +V after the addon's flip. Get it
backwards and every surface is lit inverted on one axis, which is subtle enough to
survive review and miserable to find later. :func:`normal_from_height` therefore
takes ``green_down`` explicitly and refuses to guess.
"""

import numpy as np

from . import metrics

#: What an unclassified region gets: a neutral dielectric, no relief, no glow.
DEFAULT_MATERIAL = {
    "material": "unknown",
    "roughness": [0.45, 0.75],
    "roughness_drives": "inverse_luminance",
    "metallic": 0.0,
    "height_scale": 0.3,
    "emissive": {"present": False},
}


def saturation(rgb):
    mx = rgb[..., :3].max(axis=-1)
    mn = rgb[..., :3].min(axis=-1)
    return np.where(mx > 1e-6, (mx - mn) / np.maximum(mx, 1e-6), 0.0)


def _normalise(a):
    lo, hi = float(np.percentile(a, 2)), float(np.percentile(a, 98))
    if hi - lo < 1e-6:
        return np.full_like(a, 0.5)
    return np.clip((a - lo) / (hi - lo), 0.0, 1.0)


def high_pass(gray, sigma=6):
    """Detail: the image minus a cheap separable box blur of it.

    A box blur applied three times approximates a Gaussian closely enough for a
    detail split, and stays a handful of array ops with no SciPy dependency. The
    blur **wraps**, so detail taken from a tiling texture still tiles.
    """
    blur = gray.astype(np.float64)
    for _ in range(3):
        k = max(1, int(sigma))
        pad = np.concatenate([blur[:, -k:], blur, blur[:, :k]], axis=1)
        csum = np.cumsum(pad, axis=1)
        blur = (csum[:, 2 * k:] - csum[:, :-2 * k]) / (2 * k)
        pad = np.concatenate([blur[-k:, :], blur, blur[:k, :]], axis=0)
        csum = np.cumsum(pad, axis=0)
        blur = (csum[2 * k:, :] - csum[:-2 * k, :]) / (2 * k)
    return gray - blur, blur


def roughness(albedo, labels, materials):
    """Per-patch roughness range, modulated by an albedo-derived driver."""
    lum = metrics.luminance(albedo)
    sat = saturation(albedo)
    detail = _normalise(np.abs(high_pass(lum)[0]))
    out = np.zeros(lum.shape, dtype=np.float32)

    for label, spec in _iter_regions(labels, materials):
        region = labels == label if label else np.ones(lum.shape, dtype=bool)
        lo, hi = spec.get("roughness", DEFAULT_MATERIAL["roughness"])
        drive = spec.get("roughness_drives", "inverse_luminance")
        if drive == "luminance":
            t = _normalise(lum)
        elif drive == "inverse_luminance":
            t = 1.0 - _normalise(lum)
        elif drive == "detail":
            t = detail
        elif drive == "saturation":
            t = _normalise(sat)
        else:  # constant
            t = np.full(lum.shape, 0.5)
        out[region] = np.clip(lo + (hi - lo) * t, 0.0, 1.0)[region]
    return out


def metallic(albedo, labels, materials):
    """Per-patch metalness.

    Kept a per-patch constant on purpose: metalness is physically near-binary, and
    a constant over a correctly-segmented region is *more* right than a texture
    that hedges between 0 and 1 across a surface that is entirely one or the other.
    The only modulation is that painted or corroded coverage, which the classifier
    reports as a low value, scales down rather than dithering.
    """
    out = np.zeros(albedo.shape[:2], dtype=np.float32)
    for label, spec in _iter_regions(labels, materials):
        region = labels == label if label else np.ones(out.shape, dtype=bool)
        out[region] = float(np.clip(spec.get("metallic", 0.0), 0.0, 1.0))
    return out


#: Most of a region that may be treated as emitting light.
#:
#: The classifier's *readings* are sound -- "rock cliff with glowing red circuitry",
#: "fiery clouds" really are emissive surfaces -- but its **thresholds are not
#: calibrated to the texture's own histogram**. On the level01 run it returned a
#: threshold of 0.20 for a sheet whose mean luminance is 0.23, and 0.35 for one at
#: 0.48, so a "glow" covered 39% and 19% of those sheets. A surface that emits over
#: most of its area is a lightsource, not a texture with lamps on it.
#:
#: So the threshold is raised to whatever percentile actually selects this much,
#: which needs no second opinion from the model and cannot be wrong in the same way.
MAX_EMISSIVE_FRACTION = 0.25


def emissive(albedo, labels, materials):
    """Glow, thresholded out of the albedo so it stays on the pixels that glow.

    A generated emissive mask is the worst case for misregistration: the map is
    mostly black, so a gradient correlation says little, and a lamp offset by ten
    texels lights the wall beside it. Thresholding the albedo cannot do that.

    The threshold the classifier supplies is a floor, not the final word -- see
    :data:`MAX_EMISSIVE_FRACTION`.
    """
    lum = metrics.luminance(albedo)
    sat = saturation(albedo)
    out = np.zeros(albedo.shape[:2] + (3,), dtype=np.float32)

    for label, spec in _iter_regions(labels, materials):
        em = spec.get("emissive") or {}
        if not em.get("present"):
            continue
        region = labels == label if label else np.ones(lum.shape, dtype=bool)
        thresh = float(em.get("threshold", 0.75))
        strength = float(em.get("strength", 1.0))
        hue = em.get("hue", "any")

        hue_ok = np.ones(lum.shape, dtype=bool)
        if hue in ("r", "g", "b"):
            channel = {"r": 0, "g": 1, "b": 2}[hue]
            hue_ok = (albedo[..., channel] == albedo[..., :3].max(axis=-1)) & (sat > 0.25)

        # Raise the threshold until at most MAX_EMISSIVE_FRACTION of the region's
        # hue-eligible pixels qualify. Measured on the region, not the sheet, so a
        # small bright part is not throttled by a large dark one beside it.
        eligible = lum[region & hue_ok]
        if eligible.size:
            cap = float(np.quantile(eligible, 1.0 - MAX_EMISSIVE_FRACTION))
            thresh = max(thresh, cap)
        hot = (lum >= thresh) & hue_ok
        # Ramp from the threshold to white so a lamp core is brighter than its halo.
        ramp = np.clip((lum - thresh) / max(1e-3, 1.0 - thresh), 0.0, 1.0)
        sel = hot & region
        out[sel] = (albedo[..., :3] * ramp[..., None] * strength)[sel]
    return out


def height_from_albedo(albedo, labels, materials):
    """A stand-in height map: band-passed luminance, scaled per material.

    This is the honest fallback, not the goal. Luminance correlates with depth
    only where lighting happens to have been baked in that way, so the result is
    plausible relief rather than the real thing -- which is exactly the gap
    :mod:`generate` exists to fill.
    """
    lum = metrics.luminance(albedo)
    detail, _ = high_pass(lum, sigma=4)
    coarse, _ = high_pass(lum, sigma=16)
    field = 0.65 * _normalise(coarse) + 0.35 * _normalise(detail + 0.5)

    out = np.full(lum.shape, 0.5, dtype=np.float32)
    for label, spec in _iter_regions(labels, materials):
        region = labels == label if label else np.ones(lum.shape, dtype=bool)
        scale = float(np.clip(spec.get("height_scale", 0.3), 0.0, 1.0))
        out[region] = (0.5 + (field - 0.5) * scale)[region]
    return out


def fuse_height(generated, albedo, labels, materials, weight=0.75):
    """Model relief for the low frequencies, albedo detail for the high ones.

    This is the mitigation that makes a generated height map usable despite a model
    that cannot promise pixel registration: the frequencies where misalignment is
    *visible* come from the albedo, which is aligned by definition, and the model
    only supplies the broad form it is actually good at. A few texels of drift in
    the low-frequency term is invisible; the same drift in the detail term is not.
    """
    _, gen_low = high_pass(generated.astype(np.float64), sigma=12)
    alb_detail, _ = high_pass(metrics.luminance(albedo), sigma=4)

    out = np.zeros(gen_low.shape, dtype=np.float32)
    for label, spec in _iter_regions(labels, materials):
        region = labels == label if label else np.ones(out.shape, dtype=bool)
        scale = float(np.clip(spec.get("height_scale", 0.3), 0.0, 1.0))
        fused = (0.5 + (_normalise(gen_low) - 0.5) * weight
                 + alb_detail * (1.0 - weight) * 2.0)
        out[region] = np.clip(0.5 + (fused - 0.5) * (0.4 + scale), 0.0, 1.0)[region]
    return out


def normal_from_height(height, strength=2.0, green_down=False):
    """Tangent-space normal map from a height field, as float ``HxWx3`` in 0..1.

    Deriving rather than generating is what guarantees the result is a *valid*
    normal map -- every texel is a unit vector, which a model asked for an RGB
    normal map has no way to promise, and an invalid one lights as noise.

    The gradient wraps, so a tiling height map yields a tiling normal map.

    ``green_down`` selects the convention: ``False`` writes +G for a surface tilting
    toward the top of the image (OpenGL / Blender), ``True`` inverts it (DirectX).
    See this module's own docstring -- this interacts with the addon's V flip and is
    not a cosmetic choice.
    """
    h = height.astype(np.float64)
    dx = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) * 0.5
    dy = (np.roll(h, -1, axis=0) - np.roll(h, 1, axis=0)) * 0.5

    nx = -dx * strength
    ny = -dy * strength
    if green_down:
        ny = -ny
    nz = np.ones_like(h)
    norm = np.sqrt(nx * nx + ny * ny + nz * nz)
    out = np.stack([nx / norm, ny / norm, nz / norm], axis=-1)
    return ((out + 1.0) * 0.5).astype(np.float32)


def delight_mask(labels, materials):
    """Which texels belong to a region stage 1 said carries baked lighting.

    ``None`` means "every texel", which is what an unsegmented sheet gets.

    This exists because painted lighting in Gunlok is a **per-region** property
    of a minority of regions, measured rather than assumed -- see the README's
    "Baked lighting". Almost all of
    it is airbrushed form shading on one curved object's unwrap, so a 24-region
    unit atlas typically wants de-lighting on the two or three patches that are a
    pod or a cylinder and on none of the flat-lit photographic plate beside them.
    Handing the whole sheet to a de-lighter would flatten those twenty-one as
    well, and they have nothing to flatten -- what would come off them is
    material contrast.
    """
    if labels is None or not materials:
        return None
    keep = [label for label, spec in _iter_regions(labels, materials)
            if (spec or {}).get("delight")]
    if not keep:
        return np.zeros(labels.shape, dtype=bool)
    if len(keep) == len(np.unique(labels)):
        return None
    mask = np.zeros(labels.shape, dtype=bool)
    for label in keep:
        mask |= labels == label
    return mask


def apply_where(base, replacement, mask):
    """``replacement`` inside ``mask``, ``base`` outside. ``None`` mask = all of it.

    A hard edge and not a feather: the mask is a region boundary the whole
    pipeline already paints roughness and metalness across with a hard edge, and
    a feathered one here would put a band of half-de-lit pixels *outside* the
    region that asked for it.
    """
    if mask is None:
        return replacement
    out = base.copy()
    out[mask] = replacement[mask]
    return out


def delight(albedo, labels, materials, amount=0.6):
    """Flatten baked lighting by dividing out the low-frequency luminance.

    A crude but registered de-lighter, useful as the fallback when the model's
    de-lit albedo fails its gate. It cannot tell a painted gradient from a shadow,
    which is precisely what a model can, so ``amount`` stays conservative.

    Applied only to the regions that asked for it (:func:`delight_mask`). The
    low-frequency term is still computed over the whole sheet, because a blur
    restricted to a scattered region has no defined value at its edges; it is the
    *result* that is masked.
    """
    lum = metrics.luminance(albedo)
    _, low = high_pass(lum, sigma=24)
    gain = np.where(low > 1e-3, np.mean(low) / np.maximum(low, 1e-3), 1.0)
    gain = 1.0 + (np.clip(gain, 0.4, 2.5) - 1.0) * amount
    out = albedo.copy()
    out[..., :3] = np.clip(albedo[..., :3] * gain[..., None], 0.0, 1.0)
    return apply_where(albedo, out, delight_mask(labels, materials))


def _iter_regions(labels, materials):
    """Yield ``(label, spec)``, falling back to one whole-image region.

    ``label 0`` means "the sheet is not segmented" (a tiling texture) or "this
    texel is not used by any polygon". Either way one spec covers it, which is why
    a tiling texture needs no atlas pass at all.
    """
    if labels is None or not materials:
        yield 0, (materials or {}).get("0") or (materials or {}).get(0) or DEFAULT_MATERIAL
        return
    present = [int(v) for v in np.unique(labels)]
    for label in present:
        spec = materials.get(str(label)) or materials.get(label)
        if spec is None and label == 0:
            spec = DEFAULT_MATERIAL
        if spec is None:
            spec = DEFAULT_MATERIAL
        yield label, spec
