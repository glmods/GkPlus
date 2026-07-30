"""The arithmetic the pipeline rests on, checked without Blender or the network.

    uv run python tests/test_pipeline.py

Everything here is synthetic on purpose: each test states a property the generator
would be silently wrong without, and the gates in :mod:`metrics` are only worth
having if they actually fire, so each one is checked against an input it must reject
as well as one it must accept.
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from gkpbr import atlas, derive, images, metrics  # noqa: E402

FAILURES = []


def check(name, ok, detail=""):
    print("  %-58s %s%s" % (name, "ok" if ok else "FAIL", "  " + detail if detail else ""))
    if not ok:
        FAILURES.append(name)


def checkerboard(size=128, period=16):
    y, x = np.mgrid[0:size, 0:size]
    return (((x // period) + (y // period)) % 2).astype(np.float32)


def wrapping_noise(size=128, seed=0):
    """A field that wraps exactly: low-frequency sinusoids at integer periods."""
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:size, 0:size] * (2 * np.pi / size)
    out = np.zeros((size, size))
    for kx, ky in rng.integers(1, 6, size=(6, 2)):
        out += np.sin(kx * x + ky * y + rng.random() * 6.28)
    return ((out - out.min()) / np.ptp(out)).astype(np.float32)


# ---------------------------------------------------------------------------

def test_normal_is_unit_length():
    """An invalid normal map lights as noise, which is why these are derived."""
    height = wrapping_noise(96, seed=1)
    for strength in (0.5, 2.0, 8.0):
        n = derive.normal_from_height(height, strength=strength) * 2.0 - 1.0
        mag = np.sqrt((n ** 2).sum(axis=-1))
        check("normal_from_height unit length (strength %.1f)" % strength,
              abs(float(mag.max()) - 1.0) < 1e-4 and abs(float(mag.min()) - 1.0) < 1e-4,
              "max dev %.2e" % abs(float(np.abs(mag - 1.0).max())))


def test_normal_green_convention():
    """The two conventions must differ in G and agree in R and B, or the flag lies."""
    # A ramp rising toward the top of the image: dy < 0 everywhere.
    height = np.tile(np.linspace(1.0, 0.0, 64)[:, None], (1, 64)).astype(np.float32)
    up = derive.normal_from_height(height, green_down=False)
    down = derive.normal_from_height(height, green_down=True)
    inner = (slice(2, -2), slice(2, -2))
    check("green_down flips G", float(np.abs((up[..., 1] - 0.5) + (down[..., 1] - 0.5))
                                     [inner].max()) < 1e-6)
    check("green_down leaves R and B alone",
          np.allclose(up[..., 0][inner], down[..., 0][inner])
          and np.allclose(up[..., 2][inner], down[..., 2][inner]))
    check("OpenGL convention: +G for a surface tilting toward the top of the image",
          float(up[..., 1][inner].mean()) > 0.5,
          "mean G %.3f" % float(up[..., 1][inner].mean()))


def test_align_accepts_identity_and_detects_shift():
    base = wrapping_noise(128, seed=2)
    r, shift, r0 = metrics.align(base, base)
    check("align(x, x) correlates ~1 with no shift",
          r > 0.99 and shift == (0, 0) and r0 > 0.99,
          "r=%.3f r0=%.3f shift=%s" % (r, r0, shift))

    rolled = np.roll(np.roll(base, 8, axis=0), -4, axis=1)
    r, shift, r0 = metrics.align(base, rolled)
    # 96/128 of a texel, so an 8-texel roll shows up as 6 at probe scale.
    check("align finds the shift of a rolled copy, and r0 is much worse",
          r > 0.9 and shift != (0, 0) and (r - r0) > metrics.SHIFT_MARGIN,
          "r=%.3f r0=%.3f shift=%s" % (r, r0, shift))

    rng = np.random.default_rng(3)
    r, _, _ = metrics.align(base, rng.random(base.shape).astype(np.float32))
    check("align rejects noise", r < metrics.MIN_ALIGN, "r=%.3f" % r)


def test_accept_gate_fires():
    """The gate has to reject as well as pass, or it is decoration."""
    base = wrapping_noise(128, seed=4)
    ok, _, why = metrics.accept(base, base)
    check("accept passes an identical map", ok, why)

    rng = np.random.default_rng(5)
    ok, _, why = metrics.accept(base, rng.random(base.shape).astype(np.float32))
    check("accept rejects noise", not ok, why)

    ok, _, why = metrics.accept(base, np.roll(base, 40, axis=1))
    check("accept rejects a large shift", not ok, why)

    nudged = np.roll(base, 2, axis=1)
    ok, corrected, why = metrics.accept(base, nudged)
    check("accept un-rolls a small shift",
          ok and not np.allclose(corrected, nudged), why)


def test_accept_passes_a_correctly_flat_height_map():
    """The case that forced the gate to be redesigned.

    ``structures/cratetextf 512`` is a crate whose albedo has enormous painted
    contrast (luminance std 0.335) and almost no geometric relief. The model
    correctly returned a nearly-flat height map (std 0.055), and the original gate --
    absolute gradient correlation against the albedo, floor 0.35 -- rejected it at
    0.34, then fell back to a derived map that manufactured relief out of the paint.
    Rejecting a right answer for doing the one thing the prompt asks for is worse
    than having no gate.
    """
    # The real profile: a high-contrast albedo, and relief that shares *some* of its
    # structure (a slat boundary is both a paint edge and a groove) but at a fraction
    # of the contrast, mixed with relief the paint does not show at all. Amplitude is
    # chosen to sit clear of FLAT_STD so this exercises the correlation path rather
    # than the flat escape -- which is what the previous version of this test
    # accidentally measured.
    shared = wrapping_noise(128, seed=11)
    albedo = shared.copy()
    unrelated = wrapping_noise(128, seed=12)
    relief = (0.5 + 0.55 * (0.45 * (shared - 0.5) + 0.55 * (unrelated - 0.5))
              ).astype(np.float32)

    r, shift, r0 = metrics.align(albedo, relief)
    check("the fixture reproduces cratetextf: weak r, low contrast, zero shift",
          relief.std() > metrics.FLAT_STD and r < 0.7 and shift == (0, 0),
          "std %.3f r %.2f r0 %.2f shift %s" % (relief.std(), r, r0, shift))

    ok, _, why = metrics.accept(albedo, relief)
    check("accept passes weakly-correlated relief that is in the right place", ok, why)

    # Displaced far, it must still be caught -- the redesign loosened the correlation
    # floor, and this is the check that it did not loosen registration with it.
    moved = np.roll(relief, 40, axis=1)
    ok, _, why = metrics.accept(albedo, moved)
    check("...while the same map displaced by 40 is still rejected", not ok, why)


def test_flat_escape():
    """A near-uniform map is accepted as flat rather than scored on correlation."""
    rng = np.random.default_rng(12)
    flat = (np.full((96, 96), 0.5) + rng.normal(0, 0.008, (96, 96))).astype(np.float32)
    ok, _, why = metrics.accept(wrapping_noise(96, seed=13), flat)
    check("accept passes a flat map with no correlation at all", ok, why)
    check("...and says so rather than reporting a correlation", "flat" in why, why)


def test_seam_energy_separates_wrapping_from_not():
    wrap = wrapping_noise(128, seed=6)
    check("a wrapping field reads as tiling", metrics.tiles(wrap),
          "seam %.2f/%.2f" % metrics.seam_energy(wrap))

    # A gradient does not wrap: its two edges are 0 and 1.
    ramp = np.tile(np.linspace(0, 1, 128)[None, :], (128, 1)).astype(np.float32)
    sx, _ = metrics.seam_energy(ramp)
    check("a non-wrapping gradient does not", not metrics.tiles(ramp),
          "seamX %.1f" % sx)


def test_blend_seamless_repairs_model_edge_artifacts():
    """What the half-offset trick is actually for.

    It cannot make a non-wrapping *source* wrap -- feeding it an identity model
    returns the source unchanged, which is correct and was the first version of this
    test. What it repairs is damage the **model** does near the edges of whatever it
    was handed: generating a second time from a half-offset input puts those edges in
    the interior, where the model behaves, and the blend takes each region from the
    pass that had it inside.
    """
    ideal = wrapping_noise(128, seed=8)
    band = 10

    def fake_model(src):
        """Faithful in the interior, garbage in a border band -- the failure seen."""
        out = src.copy()
        out[:band, :] = out[-band:, :] = 0.5
        out[:, :band] = out[:, -band:] = 0.5
        return out

    direct = fake_model(ideal)
    from_rolled = images.roll_half(fake_model(images.roll_half(ideal)))
    fixed = images.blend_seamless(direct, from_rolled)

    def err(a):
        return float(np.abs(a - ideal).mean())

    check("blend_seamless beats the single pass", err(fixed) < err(direct) * 0.6,
          "err %.4f -> %.4f" % (err(direct), err(fixed)))

    edge = np.zeros_like(ideal, dtype=bool)
    edge[:band, :] = edge[-band:, :] = edge[:, :band] = edge[:, -band:] = True
    before = float(np.abs(direct - ideal)[edge].mean())
    after = float(np.abs(fixed - ideal)[edge].mean())
    check("blend_seamless repairs the border band", after < before * 0.3,
          "border err %.4f -> %.4f" % (before, after))

    # And it must leave a clean pass essentially alone.
    clean = images.blend_seamless(ideal, ideal)
    check("blend_seamless is a no-op on two good passes",
          float(np.abs(clean - ideal).max()) < 1e-6)


def test_place_translates_and_never_wraps():
    """The bug that reported 99% coverage and no patches on the clearest atlas."""
    # A triangle straddling the right edge of a 1024 sheet.
    uv = [(1020.0, 10.0), (1030.0, 10.0), (1025.0, 20.0)]
    pts, full = atlas.place(uv, 1024, 1024)
    check("place() does not split an edge-straddling triangle", not full and pts is not None)
    spread = max(p[0] for p in pts) - min(p[0] for p in pts)
    check("place() keeps its width (a modulo would make it 1014)", abs(spread - 10.0) < 1e-6,
          "width %.1f" % spread)

    # A quad taking the whole sheet is a tiling sample, not a patch.
    _, full = atlas.place([(0.0, 0.0), (1024.0, 0.0), (1024.0, 1024.0), (0.0, 1024.0)],
                          1024, 1024)
    check("place() calls a full-sheet quad a tiling sample", full)

    _, full = atlas.place([(10.0, 10.0), (90.0, 10.0), (90.0, 90.0)], 1024, 1024)
    check("place() calls a small rect a patch", not full)


class _Ref:
    """Stands in for assets.Reference: a UV polygon plus who samples it."""

    def __init__(self, uv, part):
        self.uv = uv
        self.part = part
        self.shape = None
        self.rif = "test.rif"


def test_segmentation_boundaries_are_not_quantised():
    """The stepping bug: coarse discovery reused as the painting mask.

    Regions were rasterized at 1/8 scale and upsampled by pixel replication, so every
    boundary snapped to an 8-texel grid and `derive` painted roughness and metalness
    into visible blocks. A diagonal edge is the sharpest test available: its per-row
    width should take a different value on almost every row, and under grid
    quantisation it takes one value per block instead.
    """
    size = 64
    # A right triangle with a corner-to-corner hypotenuse. Kept under
    # atlas.FULL_SHEET_FRACTION of the sheet, or `place` correctly calls it a
    # whole-sheet tiling sample and there is no region to measure.
    span = 40.0
    tri = [(0.0, 0.0), (span, 0.0), (0.0, span)]
    _, labels, patches, _ = atlas.segment([_Ref(tri, "wedge")], size, size)

    check("a single part yields a single region", len(patches) == 1,
          "%d regions" % len(patches))
    if not patches:
        return

    widths = [int((labels[y] == patches[0].label).sum()) for y in range(int(span))]
    widths = [w for w in widths if w]
    distinct = len(set(widths))
    check("a diagonal edge has a near-unique width per row (not one per 8-row block)",
          distinct > len(widths) // 2,
          "%d distinct widths over %d rows" % (distinct, len(widths)))

    # And the width must actually decrease down the triangle rather than plateau.
    monotone = sum(1 for a, b in zip(widths, widths[1:]) if b <= a)
    check("...and it narrows monotonically", monotone >= len(widths) - 2,
          "%d of %d steps non-increasing" % (monotone, len(widths) - 1))

    check("labels fit in 8 bits, as the PNG on disk requires",
          labels.dtype == np.uint8 and int(labels.max()) <= 255, str(labels.dtype))


def test_segmentation_keeps_regions_separate_at_full_resolution():
    """Two parts meeting on a diagonal must not bleed into 8-texel blocks."""
    size, span = 64, 40.0
    left = [(0.0, 0.0), (span, 0.0), (0.0, span)]
    right = [(span, 0.0), (span, span), (0.0, span)]
    _, labels, patches, _ = atlas.segment(
        [_Ref(left, "a"), _Ref(right, "b")], size, size)
    check("two parts yield two regions", len(patches) == 2, "%d" % len(patches))
    if len(patches) != 2:
        return

    # Identify by part name, not by position in `patches`: both triangles have the
    # same area, so the area sort between them is arbitrary.
    by_name = {p.parts[0]: p.label for p in patches}
    check("both parts are named", set(by_name) == {"a", "b"}, str(sorted(by_name)))
    if set(by_name) != {"a", "b"}:
        return

    # `a` is the triangle containing the origin, `b` the one containing (span, span);
    # they share the hypotenuse x + y = span. Only the two far corners are
    # unambiguously inside one of them -- a block straddling the diagonal belongs to
    # neither, which is what the first version of this check sampled.
    corner_a = labels[2:10, 2:10]
    corner_b = labels[int(span) - 10:int(span) - 2, int(span) - 10:int(span) - 2]
    for name, block in (("a", corner_a), ("b", corner_b)):
        share = float((block == by_name[name]).mean())
        check("region %r owns its own far corner" % name, share > 0.95, "%.2f" % share)

    # And the diagonal must be a diagonal: the boundary row by row should advance.
    firsts = [int(np.argmax(labels[y] == by_name["b"])) for y in range(4, int(span) - 4)
              if (labels[y] == by_name["b"]).any()]
    check("the shared edge advances per row rather than in blocks",
          len(set(firsts)) > len(firsts) // 2,
          "%d distinct starts over %d rows" % (len(set(firsts)), len(firsts)))


def test_min_area_floor_is_absolute_not_fractional():
    """A small sheet must not admit smaller regions than a large one.

    With only ``MIN_AREA_FRACTION`` the threshold was 13 texels on a 256 sheet against
    210 on a 1024, so the sheets least able to afford tiny regions were the ones that
    admitted them -- ``ground/cracks.rim`` came out with a 30-texel region that the
    classifier answered with an invented material. The same absolute patch must now
    survive or be dropped on the same terms whatever sheet it sits in.
    """
    # A square patch a little under the floor, placed on two very different sheets.
    side = int(atlas.MIN_AREA_TEXELS ** 0.5) - 4
    small = [(4.0, 4.0), (4.0 + side, 4.0), (4.0 + side, 4.0 + side), (4.0, 4.0 + side)]
    for sheet in (256, 1024):
        _, _, patches, _ = atlas.segment([_Ref(small, "tiny")], sheet, sheet)
        check("a %d-texel patch is dropped on a %d sheet" % (side * side, sheet),
              len(patches) == 0, "%d regions" % len(patches))

    # And one comfortably over the floor survives on both.
    side = int(atlas.MIN_AREA_TEXELS ** 0.5) * 2
    big = [(4.0, 4.0), (4.0 + side, 4.0), (4.0 + side, 4.0 + side), (4.0, 4.0 + side)]
    for sheet in (256, 1024):
        _, _, patches, _ = atlas.segment([_Ref(big, "real")], sheet, sheet)
        check("a %d-texel patch is kept on a %d sheet" % (side * side, sheet),
              len(patches) == 1, "%d regions" % len(patches))
        if patches:
            # Pins the winding requirement. The dedup key is an order-insensitive
            # sorted tuple, and using it for the shoelace area and the triangle fan
            # measured this square as a bowtie -- 0 area, 1541 of 1936 texels
            # rasterized. Every shipped polygon is a triangle, where sorting three
            # points changes nothing, so only a quad catches it.
            got = patches[0].area
            check("...with its true area, not a bowtie's" ,
                  abs(got - side * side) <= 4 * side,
                  "%d texels vs %d expected" % (got, side * side))


def test_floor_applies_to_what_survives_painting():
    """A region shredded by the regions painted over it must be dropped.

    Painting goes largest-first so a small specific part wins the texels it shares
    with the hull it sits on. The consequence is that a large region can clear the
    area floor on its own analytic area and then be reduced to slivers. Filtering only
    on the analytic area left **one- and two-texel** regions in the real output --
    worse than the 30-texel ones the floor was added to remove.
    """
    outer = 44.0
    inner = 43.0
    big = [(2.0, 2.0), (2.0 + outer, 2.0), (2.0 + outer, 2.0 + outer), (2.0, 2.0 + outer)]
    small = [(2.0, 2.0), (2.0 + inner, 2.0), (2.0 + inner, 2.0 + inner), (2.0, 2.0 + inner)]

    _, labels, patches, _ = atlas.segment(
        [_Ref(big, "hull"), _Ref(small, "plate")], 256, 256)

    names = [p.parts[0] for p in patches]
    check("the shredded region is dropped, the one over it kept",
          names == ["plate"], "%s" % names)
    check("every surviving region clears the floor",
          all(p.area >= atlas.MIN_AREA_TEXELS for p in patches),
          "areas %s" % [p.area for p in patches])
    if patches:
        check("the dropped region's slivers revert to region 0",
              int((labels == 0).sum()) > 0 and set(np.unique(labels)) <= {0, patches[0].label},
              "labels present %s" % sorted(int(v) for v in np.unique(labels)))


def test_derive_respects_regions():
    """A per-region material must land only in its own region."""
    albedo = np.repeat(checkerboard(64, 8)[..., None], 3, axis=2)
    labels = np.zeros((64, 64), dtype=np.int32)
    labels[:32, :] = 1
    labels[32:, :] = 2
    materials = {
        "1": {"roughness": [0.1, 0.1], "roughness_drives": "constant", "metallic": 1.0,
              "height_scale": 0.0, "emissive": {"present": False}},
        "2": {"roughness": [0.9, 0.9], "roughness_drives": "constant", "metallic": 0.0,
              "height_scale": 0.0, "emissive": {"present": False}},
    }
    rough = derive.roughness(albedo, labels, materials)
    metal = derive.metallic(albedo, labels, materials)
    check("roughness follows the region", abs(float(rough[:32].mean()) - 0.1) < 1e-3
          and abs(float(rough[32:].mean()) - 0.9) < 1e-3,
          "%.2f / %.2f" % (rough[:32].mean(), rough[32:].mean()))
    check("metallic follows the region", float(metal[:32].min()) == 1.0
          and float(metal[32:].max()) == 0.0)


def test_emissive_stays_on_the_bright_pixels():
    albedo = np.zeros((64, 64, 3), dtype=np.float32)
    albedo[:, :, 0] = 0.05
    albedo[20:24, 20:24] = (1.0, 0.1, 0.1)  # a small red lamp
    materials = {"0": {"emissive": {"present": True, "hue": "r", "threshold": 0.25}}}
    em = derive.emissive(albedo, None, materials)
    lit = em.sum(axis=-1) > 0.01
    check("emissive lights only the lamp", lit[20:24, 20:24].all() and lit.sum() == 16,
          "%d texels lit" % int(lit.sum()))


def test_high_pass_wraps():
    """Detail from a tiling texture has to tile, or every derived map seams."""
    wrap = wrapping_noise(128, seed=7)
    detail, _ = derive.high_pass(wrap, sigma=6)
    check("high_pass output wraps", metrics.tiles(detail + 0.5),
          "seam %.2f/%.2f" % metrics.seam_energy(detail + 0.5))


def test_the_harness_can_fail():
    """A harness that cannot report a failure proves nothing."""
    before = len(FAILURES)
    check("deliberate failure (expected FAIL on the line above)", False, "by design")
    ok = len(FAILURES) == before + 1
    FAILURES.pop()
    print("  %-58s %s" % ("harness detects a failure", "ok" if ok else "FAIL"))
    if not ok:
        FAILURES.append("harness cannot fail")


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in tests:
        print("%s:" % fn.__name__)
        fn()
    print()
    if FAILURES:
        print("%d FAILED: %s" % (len(FAILURES), ", ".join(FAILURES)))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
