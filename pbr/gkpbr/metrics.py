"""The gates. Nothing a model returns is accepted without passing these.

A generative image model re-renders rather than edits, so the two ways its output
can be quietly useless are **misregistration** against the albedo and **seams** on
a texture that has to tile. Both are measurable in a few lines, and measuring them
is what makes it safe to put a model in the pipeline at all -- a failed gate falls
back to the deterministic derivation in :mod:`derive`, which cannot misalign
because it never leaves the albedo's own pixels.

``align`` reports the correlation at the best offset, that offset, **and** the
correlation at zero offset, and the three together are what make the gate
trustworthy: a displaced map lines up much better somewhere else and is recoverable
by rolling it back, a decorrelated one lines up nowhere and must be thrown away, and
a *weakly* correlated map whose best match is already at zero is simply a flat
surface described correctly -- see :data:`MIN_ALIGN` for the measurement that forced
that third case to exist.
"""

import numpy as np

#: Below this best-case gradient correlation the map bears no relation to the albedo
#: at *any* offset, which is the signature of a result that is simply wrong.
#:
#: Deliberately low, and the reason is a measurement. This started at 0.35 as a
#: straightforward "does the height map describe the albedo's surface" test, and it
#: rejected a **correct** answer: on ``structures/cratetextf 512`` the albedo's
#: luminance standard deviation is 0.335 while the returned height map's is 0.055,
#: because the model declined to read a crate's strong painted light/dark pattern as
#: geometry -- which is precisely what :data:`generate.HEIGHT_PROMPT` asks it to do.
#: Absolute gradient correlation therefore punishes the single most valuable thing a
#: height model does, namely decoupling tone from depth, and it cannot be the accept
#: criterion. Registration is tested by :data:`SHIFT_MARGIN` instead.
MIN_ALIGN = 0.15

#: How much better a shifted match must be than the un-shifted one before the map is
#: called displaced rather than merely weakly correlated. This is the real
#: registration test: a map that lines up *much* better somewhere else has moved,
#: whereas a map whose best match is at zero offset is in the right place however
#: weakly it correlates.
SHIFT_MARGIN = 0.15

#: A shift larger than this fraction of the image is a re-composition, not a nudge.
MAX_SHIFT_FRACTION = 0.05

#: Below this standard deviation a candidate carries almost no relief, and the
#: correlation test stops meaning anything: there are no gradients to correlate.
#:
#: This distinction is load-bearing rather than a nicety. A smooth painted panel
#: *should* produce a near-flat height map, and such a map is harmless -- it yields a
#: flat normal map, which leaves lighting alone. Rejecting it and falling back to
#: :func:`derive.height_from_albedo` would **invent** relief out of albedo luminance
#: on a surface that has none, which is actively worse than the model's answer. So a
#: flat result is accepted *as* flat, and only a map with real structure in the wrong
#: place is rejected. Measured against the probe set, where a crate panel scored 0.22
#: on correlation while being a perfectly reasonable "this is flat".
FLAT_STD = 0.05

#: Seam energy, as a fraction of full range, above which a texture does not tile.
TILING_SEAM = 0.10


def luminance(rgb):
    """Rec. 709 luma of a float ``HxWx3`` or ``HxWx4`` array in 0..1."""
    return (0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2])


def _box_reduce(a, size):
    """Area-average down to ``size x size``, which is where structure lives."""
    h, w = a.shape
    ys = np.linspace(0, h, size + 1).astype(int)
    xs = np.linspace(0, w, size + 1).astype(int)
    out = np.empty((size, size), dtype=np.float64)
    for i in range(size):
        for j in range(size):
            block = a[ys[i]:max(ys[i] + 1, ys[i + 1]), xs[j]:max(xs[j] + 1, xs[j + 1])]
            out[i, j] = block.mean()
    return out


def gradient_magnitude(gray):
    """Sobel magnitude, the thing two maps of the same surface must share."""
    gy, gx = np.gradient(gray.astype(np.float64))
    return np.hypot(gx, gy)


def align(reference, candidate, size=96):
    """How well ``candidate`` describes ``reference``'s surface, and where.

    Returns ``(r_best, (dy, dx), r_zero)`` -- Pearson's r between the two
    gradient-magnitude fields at the best whole-pixel offset, that offset, and r at
    zero offset. Callers want all three: the pair ``(r_best, r_zero)`` is what
    separates "displaced" from "weakly correlated but in the right place", and only
    the first of those is a defect. See :data:`MIN_ALIGN`.
    """
    a = _box_reduce(gradient_magnitude(reference), size)
    b = _box_reduce(gradient_magnitude(candidate), size)
    a = a - a.mean()
    b = b - b.mean()
    if a.std() < 1e-9 or b.std() < 1e-9:
        return 0.0, (0, 0), 0.0

    def corr_at(dy, dx):
        rolled = np.roll(np.roll(b, dy, axis=0), dx, axis=1)
        return float((a * rolled).mean() / (a.std() * rolled.std()))

    # Phase correlation: the peak of the circular cross-correlation is the shift.
    spec = np.fft.rfft2(a) * np.conj(np.fft.rfft2(b))
    corr = np.fft.irfft2(spec, s=a.shape)
    peak = np.unravel_index(int(np.argmax(corr)), corr.shape)
    dy = peak[0] - (size if peak[0] > size // 2 else 0)
    dx = peak[1] - (size if peak[1] > size // 2 else 0)

    return corr_at(dy, dx), (int(dy), int(dx)), corr_at(0, 0)


def seam_energy(img):
    """``(x, y)`` wrap discontinuity as a fraction of full range.

    Compares the wrapped edge pair against the *interior* gradient at the same
    place, so a naturally busy texture is not penalised for being busy: a texture
    that tiles has an edge step no larger than its own internal variation.
    """
    a = img if img.ndim == 2 else luminance(img)
    inner_x = np.abs(a[:, 1:] - a[:, :-1]).mean() + 1e-6
    inner_y = np.abs(a[1:, :] - a[:-1, :]).mean() + 1e-6
    step_x = np.abs(a[:, 0] - a[:, -1]).mean()
    step_y = np.abs(a[0, :] - a[-1, :]).mean()
    return float(step_x / max(inner_x, 1e-3)), float(step_y / max(inner_y, 1e-3))


#: An edge step this small is invisible whatever the interior gradient is.
#:
#: Precautionary rather than measured, and flagged as such. :func:`seam_energy`
#: divides by the interior gradient, so a very smooth map -- a flat painted panel --
#: can show a large *ratio* from an absolutely tiny step, and rejecting it would
#: repeat the mistake :data:`MIN_ALIGN` documents: throwing away a correct answer for
#: being featureless. No probe result has hit this yet; the one seam rejection so far
#: (``ground/rock2a 00``) was a genuine 0.51-of-full-range step, top row mean 0.527
#: against bottom row 0.015, and this floor does not excuse it.
SEAM_ABSOLUTE_FLOOR = 0.02


def tiles(img, threshold=3.0):
    """Does this texture wrap? Edge step no worse than ``threshold`` x interior.

    An absolutely negligible step passes regardless of that ratio -- see
    :data:`SEAM_ABSOLUTE_FLOOR`.
    """
    a = img if img.ndim == 2 else luminance(img)
    step_x = float(np.abs(a[:, 0] - a[:, -1]).mean())
    step_y = float(np.abs(a[0, :] - a[-1, :]).mean())
    sx, sy = seam_energy(a)
    ok_x = sx <= threshold or step_x < SEAM_ABSOLUTE_FLOOR
    ok_y = sy <= threshold or step_y < SEAM_ABSOLUTE_FLOOR
    return ok_x and ok_y


def accept(reference, candidate, require_tiling=False):
    """Gate a model result. Returns ``(ok, corrected, why)``.

    ``corrected`` is ``candidate`` rolled back into registration when the only
    problem was a small uniform shift.
    """
    spread = float(candidate.std())
    if spread < FLAT_STD:
        # No structure to be misplaced. See FLAT_STD: accepting this is safer than
        # falling back, which would invent relief the model correctly says is absent.
        return True, candidate, "flat (std %.3f), correlation not applicable" % spread

    r, (dy, dx), r0 = align(reference, candidate)
    h = candidate.shape[0]
    limit = max(1, int(MAX_SHIFT_FRACTION * 96))

    if r < MIN_ALIGN:
        return False, candidate, (
            "no relation to the albedo at any offset (best r %.2f < %.2f, std %.3f)"
            % (r, MIN_ALIGN, spread))

    # Displaced only if some other offset is *materially* better than zero. A map
    # whose best match is at zero is in the right place, however weakly it
    # correlates -- which is the case for a correctly flat-ish height map over a
    # high-contrast albedo. See MIN_ALIGN.
    displaced = (dy or dx) and (r - r0) > SHIFT_MARGIN
    if displaced and (abs(dy) > limit or abs(dx) > limit):
        return False, candidate, (
            "displaced by (%d,%d) at 96px, beyond +-%d (r %.2f vs %.2f at zero)"
            % (dy, dx, limit, r, r0))

    out = candidate
    if displaced:
        scale = h / 96.0
        out = np.roll(np.roll(candidate, int(round(dy * scale)), axis=0),
                      int(round(dx * scale)), axis=1)

    if require_tiling and not tiles(out):
        sx, sy = seam_energy(out)
        return False, out, "seam energy %.1f/%.1f on a tiling texture" % (sx, sy)
    return True, out, "r %.2f (%.2f at zero), shift (%d,%d)" % (r, r0, dy, dx)
