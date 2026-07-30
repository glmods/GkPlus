"""Segmenting a texture sheet into the regions the geometry actually uses.

Of the 364 textures the shipped geometry names, only **6** are sampled as wrapping
tiling surfaces; the rest are **atlases**, and the heavily-used ones especially --
``baddies3.rim`` is named by 133 ``.rif`` files and packs unrelated materials side by
side. A single roughness or metalness answer for such a sheet is wrong by
construction, so the classifier has to be asked per *region*, and the regions have to
come from somewhere.

(An earlier pixel-based estimate put the tiling count at 99. That measured whether a
sheet's edges happen to match, which is a question about the artist; :func:`place` and
:func:`tiling_fraction` measure what the game does with the sheet.)

They come from the UV triangles, **grouped by the part that samples them** -- the
``OBJHEAD1`` name of the object whose shape the polygon belongs to. So a region is
"the texels the gun barrel of this walker uses", which is both a mask to paint into
and a phrase to hand a classifier.

Four implementation notes, each of which was a wrong version first:

- **Group by part, not by connected components of the occupancy mask.** These
  atlases have no gutters: ``baddies3.rim`` is 99% covered by the polygons that
  sample it, so every patch touches its neighbours and one flood fill swallows the
  entire sheet. Connectivity cannot separate what the artist packed edge to edge;
  the geometry's own grouping can.
- **Segment by UV, never by mesh adjacency.** RIF UVs are an *indexed* list, not
  parallel to the vertices, so two polygons sharing a vertex may sit anywhere in
  the sheet. A union-find over vertex indices merges distant patches into one
  region spanning the whole texture, which is what the first attempt at this did.
- **Rasterize at full texel resolution.** Discovery was done coarsely, on a 1/8-scale
  mask, and then that mask was reused for *painting* -- so every boundary snapped to an
  8-texel grid and :mod:`derive` produced visibly stepped roughness and metalness. The
  cheap part is picking *which* regions exist, which :func:`_polygon_area` now answers
  in closed form with no rasterization at all; the part that must be exact is which
  texels belong to one. See :func:`segment`.
"""

import collections

import numpy as np

#: Patches below this share of the sheet are dropped as rasterization crumbs.
MIN_AREA_FRACTION = 0.0002

#: ...and below this many texels regardless of sheet size. The floor is the binding
#: one on small sheets, and having only the fraction was **backwards**: it gave 13
#: texels on a 256 sheet against 210 on a 1024, so exactly the sheets least able to
#: afford tiny regions were the ones that admitted them.
#:
#: The consequence was measurable rather than theoretical. ``ground/cracks.rim`` (256)
#: came out with 24 regions whose tail ran down to **30 texels** -- a 5x6 patch -- and
#: the classifier duly returned a distinct material for each: "metal plate edge",
#: "metal plate corner", "metal wall joint", "grimy metal panel", all paraphrases of
#: one material, with metalness scattered from 0.0 to 0.7 because nothing legible was
#: there to judge. Those hedged values then painted hard-edged specks into the
#: metalness map. The free-text ``material`` field hid it: 24 regions produced 24
#: "distinct" strings, and only the numbers showed they were the same surface.
#:
#: 512 texels is ~22x22 pixels in the image the model is shown, which is about the
#: smallest region it can say anything about. Chosen by measuring the alternatives:
#: it takes ``cracks.rim`` from 24 regions to 13 while leaving ``elint_mk2_512``'s
#: genuinely-distinct 24 at 23, where a 1024-texel floor would cut ``cracks`` to 7
#: and start losing real parts. Anything dropped joins region 0, "the rest of this
#: sheet", which is one honest answer instead of eleven invented ones.
MIN_AREA_TEXELS = 512


class Patch:
    """One connected region of a sheet, in full texel coordinates.

    ``label`` is this patch's id in the label image :func:`segment` returns,
    which is what lets :mod:`derive` paint a per-patch material into the patch's
    *actual* shape rather than into its bounding box -- the boxes of two
    interlocking patches overlap, so a box-painted map would fight itself.
    """

    __slots__ = ("index", "label", "x0", "y0", "x1", "y1", "area", "polys", "parts", "rifs")

    def __init__(self, index, label, x0, y0, x1, y1, area, polys, parts, rifs):
        self.index = index
        self.label = label
        self.x0, self.y0, self.x1, self.y1 = x0, y0, x1, y1
        self.area = area
        self.polys = polys
        self.parts = parts
        self.rifs = rifs

    @property
    def box(self):
        return (self.x0, self.y0, self.x1, self.y1)

    def as_dict(self):
        return {
            "index": self.index,
            "label": self.label,
            "box": list(self.box),
            "area_texels": self.area,
            "polys": self.polys,
            "parts": self.parts[:12],
            "rif_count": self.rifs,
        }

    def __repr__(self):
        return "<Patch %d (%d,%d)-(%d,%d) %d polys>" % (
            self.index, self.x0, self.y0, self.x1, self.y1, self.polys)


def _fill_triangle(mask, ax, ay, bx, by, cx, cy):
    """Scanline-fill one triangle into a boolean mask, clipped to it.

    Spans are taken from ``floor(lo)`` to ``ceil(hi)`` inclusive, so a triangle
    over-covers by up to a texel rather than under-covering. Adjacent triangles then
    overlap slightly instead of leaving a gap between them, which is the right way
    round: a gap would leave unlabelled seams through the middle of a region.
    """
    h, w = mask.shape
    pts = sorted(((ay, ax), (by, bx), (cy, cx)))
    (y0, x0), (y1, x1), (y2, x2) = pts
    if y2 == y0:  # degenerate after rounding: mark the row it sits on
        y = int(y0)
        if 0 <= y < h:
            lo, hi = int(min(x0, x1, x2)), int(max(x0, x1, x2))
            mask[y, max(0, lo):min(w, hi + 1)] = True
        return

    def edge(xa, ya, xb, yb, y):
        if yb == ya:
            return xa
        return xa + (xb - xa) * (y - ya) / (yb - ya)

    for y in range(max(0, int(np.floor(y0))), min(h, int(np.ceil(y2)) + 1)):
        long_x = edge(x0, y0, x2, y2, y)
        short_x = edge(x0, y0, x1, y1, y) if y < y1 else edge(x1, y1, x2, y2, y)
        lo, hi = sorted((long_x, short_x))
        a, b = int(np.floor(lo)), int(np.ceil(hi))
        if b >= a:
            mask[y, max(0, a):min(w, b + 1)] = True


#: Most regions a sheet is split into. A unit atlas is sampled by hundreds of named
#: parts, most of them one small plate; asking a classifier about 400 regions costs
#: more than the whole rest of the pipeline and answers the same thing 400 times.
#: The largest by polygon count are kept and the tail falls into label 0, which is
#: classified as "the rest of this sheet".
MAX_REGIONS = 24


#: A polygon whose UV extent covers at least this share of the sheet on either axis
#: is sampling the whole texture, not a patch in an atlas.
FULL_SHEET_FRACTION = 0.7


def place(uv, width, height):
    """Bring one UV polygon into the sheet, or report that it samples the whole thing.

    Returns ``(points, full_sheet)``. A polygon is **translated as a whole** by whole
    multiples of the sheet -- never wrapped per vertex, which is the trap: a
    triangle spanning u = 1020..1030 on a 1024 sheet has its second vertex land at
    6 under a modulo, and the triangle then fills the entire width. Doing that to
    every edge-straddling polygon of ``baddies3.rim`` reported 99% coverage and no
    patches at all, on the most obviously atlased sheet in the game.

    The ``full_sheet`` test is on **relative extent, not on exceeding the sheet**,
    and that distinction is measured rather than assumed: Gunlok tiles a ground
    texture by giving each terrain quad the full ``0..size`` range and repeating the
    quad, not by handing one polygon ``0..8*size``. Testing for UVs past the edge
    therefore called every ground texture an atlas, and the full-sheet quads then
    filled the occupancy mask and buried the real patches underneath.
    """
    us = [p[0] for p in uv]
    vs = [p[1] for p in uv]
    if (max(us) - min(us)) >= FULL_SHEET_FRACTION * width:
        return None, True
    if (max(vs) - min(vs)) >= FULL_SHEET_FRACTION * height:
        return None, True
    ox = np.floor(min(us) / width) * width
    oy = np.floor(min(vs) / height) * height
    return [(u - ox, v - oy) for u, v in uv], False


def _polygon_area(pts):
    """Shoelace area of a placed UV polygon, in texels.

    Analytic rather than counted: this decides which regions clear
    :data:`MIN_AREA_FRACTION`, and doing it in closed form means the ranking costs
    nothing and does not need a rasterization pass to precede it.
    """
    total = 0.0
    for i in range(len(pts)):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % len(pts)]
        total += x0 * y1 - x1 * y0
    return abs(total) * 0.5


def segment(refs, width, height):
    """``[Reference]`` for one texture -> ``(mask, labels, [Patch], stats)``.

    ``mask`` is a full-resolution occupancy map, so callers can report what share of
    the sheet is used at all -- an unused corner needs no material. ``labels`` is the
    same size, holding each patch's :attr:`Patch.label` and zero where nothing is
    used. ``stats`` carries the tiling fraction described in :func:`tiling_fraction`.

    **Rasterization is at full texel resolution, and that is not an optimisation
    choice.** An earlier version discovered regions on a 1/8-scale mask and then
    reused that same mask for painting, upsampled by pixel replication -- so every
    region boundary was quantised to 8 texels and :mod:`derive` painted roughness and
    metalness into visibly stepped blocks. Coarse is fine for deciding *which*
    regions exist; it is not fine for deciding *which texels* belong to one.

    Doing it at full resolution is affordable because the reference list collapses:
    ``baddies3.rim``'s 94,301 polygon references are 4,107 distinct UV triangles, and
    ``elint_mk2_512``'s 7,843 are 4,079. Every triangle is rasterized at most twice
    regardless of sheet size.
    """
    from . import assets

    mask = np.zeros((height, width), dtype=bool)

    # Deduplicate before rasterizing: the same UV triangle repeats across every file
    # that reuses the model. Rounded to a tenth of a texel -- full precision, unlike
    # the old key, which quantised to a 1/8 grid and threw away the sub-texel
    # detail this function now depends on.
    unique = {}
    counts = {"total": 0, "full_sheet": 0, "junk": 0, "placed": 0}
    for ref in refs:
        counts["total"] += 1
        if not assets.usable(ref.uv, width, height):
            counts["junk"] += 1
            continue
        pts, is_full_sheet = place(ref.uv, width, height)
        if is_full_sheet:
            counts["full_sheet"] += 1
            continue
        counts["placed"] += 1
        # The key is order-insensitive so the same patch reached by differently-wound
        # polygons dedupes to one entry -- but the *points* are stored in their
        # original order, because both the shoelace area and the triangle fan depend
        # on winding. Using the sorted key for those computed a bowtie: a 44x44 quad
        # measured 0 area and rasterized 1541 of its 1936 texels. Gunlok's SHPPOLYS
        # are all triangles, where sorting three points changes nothing, which is why
        # this stayed invisible on real data.
        key = tuple(sorted((round(u, 1), round(v, 1)) for u, v in pts))
        entry = unique.setdefault(key, {"pts": pts, "refs": []})
        entry["refs"].append(ref)

    # Group by the part that samples the texels. An unnamed shape falls under a
    # synthetic key so it is still a region rather than being lumped in with
    # everything else unnamed.
    by_part = collections.defaultdict(
        lambda: {"tris": [], "polys": 0, "rifs": set(), "area": 0.0})
    for entry in unique.values():
        group = entry["refs"]
        for ref in group:
            name = (ref.part or "").strip()
            rec = by_part[name.lower() if name else "unnamed"]
            rec["polys"] += 1
            rec["rifs"].add(ref.rif)
        rec = by_part[(group[0].part or "unnamed").strip().lower()]
        rec["tris"].append(entry["pts"])
        rec["area"] += _polygon_area(entry["pts"])

    ranked = sorted(by_part.items(), key=lambda kv: -kv[1]["polys"])
    min_area = max(MIN_AREA_FRACTION * width * height, MIN_AREA_TEXELS)
    kept = [(n, r) for n, r in ranked if r["area"] >= min_area][:MAX_REGIONS]

    def rasterize(tris, target):
        for poly in tris:
            pts = [(min(max(u, 0.0), width - 1e-3), min(max(v, 0.0), height - 1e-3))
                   for u, v in poly]
            for i in range(1, len(pts) - 1):
                _fill_triangle(target, pts[0][0], pts[0][1], pts[i][0], pts[i][1],
                               pts[i + 1][0], pts[i + 1][1])

    # Occupancy covers every part, including the ones too small to earn a region, so
    # `coverage` answers "how much of this sheet does the game touch at all".
    for _, rec in ranked:
        rasterize(rec["tris"], mask)

    # Paint largest first so a small, specific region wins the texels it shares with
    # a big one: a bolt plate sampled by its own part and by the hull it sits on
    # should be classified as the bolt plate.
    if len(kept) > 255:  # labels ship as an 8-bit image; MAX_REGIONS keeps this true
        kept = kept[:255]
    labels = np.zeros((height, width), dtype=np.uint8)
    assigned = []
    for label, (name, rec) in enumerate(
            sorted(kept, key=lambda kv: -kv[1]["area"]), start=1):
        region = np.zeros((height, width), dtype=bool)
        rasterize(rec["tris"], region)
        labels[region] = label
        assigned.append((label, name, rec))

    # The floor has to be applied to what SURVIVES painting, not to the analytic area
    # that got a region shortlisted. Because painting goes largest-first, a big region
    # can pass the pre-filter comfortably and then be shredded into slivers by the
    # smaller, more specific regions drawn over it -- filtering only on analytic area
    # left regions of **one and two texels** in the output, which is worse than the
    # 30-texel ones the floor was introduced to remove. Slivers revert to region 0,
    # "the rest of this sheet"; the texels that were taken from them already belong to
    # whichever region took them.
    surviving = np.bincount(labels.ravel(), minlength=len(assigned) + 1)
    for label, _, _ in assigned:
        if surviving[label] < min_area:
            labels[labels == label] = 0

    patches = []
    for label, name, rec in assigned:
        ys, xs = np.nonzero(labels == label)
        if len(xs) == 0:  # dropped above, or entirely overpainted
            continue
        patches.append(Patch(
            index=len(patches),
            label=label,
            x0=int(xs.min()),
            y0=int(ys.min()),
            x1=int(xs.max()) + 1,
            y1=int(ys.max()) + 1,
            area=int(len(xs)),
            polys=rec["polys"],
            parts=[name],
            rifs=len(rec["rifs"]),
        ))
    patches.sort(key=lambda p: -p.area)
    counts["regions"] = len(patches)
    counts["parts_seen"] = len(by_part)
    return mask, labels, patches, counts


def tiling_fraction(stats):
    """Share of usable samples that take the whole sheet at once.

    This is the honest tiling test, and it is better than any measurement of the
    image: a wrap-seam metric on pixels asks "would this tile *well*", which is a
    question about the artist's skill, whereas the UVs say what the game actually
    does with the sheet. A ground quad that samples 0..size and repeats is tiling
    whether or not its edges match; an atlas is sampled inside sub-rectangles.
    """
    usable_count = stats["full_sheet"] + stats["placed"]
    if usable_count == 0:
        return 0.0
    return stats["full_sheet"] / float(usable_count)


def save_labels(path, labels):
    """Write the label image as an 8-bit PNG.

    A PNG rather than ``.npy``: full-resolution labels are 1 MB raw per 1024 sheet and
    ~350 MB across the set, while a label image is almost entirely flat runs and
    compresses to a few kilobytes. It is also directly viewable, which a ``.npy`` is
    not, and that matters for a mask nobody can otherwise check.
    """
    from PIL import Image
    Image.fromarray(labels.astype(np.uint8), mode="L").save(path, optimize=True)


def load_labels(path):
    from PIL import Image
    return np.asarray(Image.open(path).convert("L"))


def coverage(mask):
    """Share of the sheet any polygon touches."""
    return float(mask.mean())
