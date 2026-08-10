#!/usr/bin/env python3
"""Fit the lighting model that produced a level's baked per-vertex colours.

    python utils/riflights/fit_bake.py "<Gunlok dir>" [level ...]

Gunlok's static lighting was baked at authoring time into `SHPVTINT` -- one packed colour per
vertex -- and the rig that produced it ships in the same file as `STDLIGHT` and is read by
nothing (`rif_chunk_format.md`). So there is ground truth for a question the binary cannot
answer: given the lights, what function of position and normal reproduces the bake?

That matters because re-creating the lighting at runtime means choosing an attenuation curve, a
diffuse term, and whether `spread` and the orientation matrix mean anything -- and AvP's own
consumer of the same chunk reads neither of the latter two. Guessing would be a plausible-looking
picture with no way to tell it was wrong. Fitting gives the choice a residual.

Everything here is read through `blender/io_scene_rif`, which imports no `bpy`.
"""

import importlib.util
import math
import os
import sys

BS = chr(92)


def load_module(name, relative):
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    path = os.path.join(root, "blender", "io_scene_rif", relative)
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


rif = load_module("rif", "rif.py")
schema = load_module("schema", "schema.py")
shapes = load_module("shapes", "shapes.py")
heads = load_module("heads", "heads.py")

FIXED = 65536.0


# --------------------------------------------------------------------------
# extraction
# --------------------------------------------------------------------------

def walk(chunk, wanted, out):
    for child in (getattr(chunk, "children", None) or ()):
        if child.id in wanted:
            out.setdefault(child.id, []).append(child)
        walk(child, wanted, out)


def lights_of(tree):
    found = {}
    walk(tree, {b"STDLIGHT", b"AMBIENCE"}, found)
    lights = [schema.decode(b"STDLIGHT", c.body) for c in found.get(b"STDLIGHT", [])]
    ambience = 0
    if found.get(b"AMBIENCE"):
        ambience = int.from_bytes(found[b"AMBIENCE"][0].body[:4], "little", signed=True)
    return lights, ambience


def objects_with_baked_light(tree):
    """Every RBOBJECT that carries a SHPVTINT, paired with its shape.

    An object finds its shape by **id**, never by position: OBJHEAD1+0x38 matches
    SHPHEAD1+0x14. Pairing the two lists positionally is wrong in one shipped file in seven.
    """
    by_id = {}
    for shape in shapes.iter_shapes(tree):
        head = None
        for child in (getattr(shape.chunk, "children", None) or ()):
            if child.id == b"SHPHEAD1":
                head = child
        if head is not None:
            by_id[heads.shphead_file_id(head.body)] = shape

    out = []
    found = {}
    walk(tree, {b"RBOBJECT"}, found)
    for obj in found.get(b"RBOBJECT", []):
        vtint = objhead = None
        for child in (getattr(obj, "children", None) or ()):
            if child.id == b"SHPVTINT":
                vtint = child
            elif child.id == b"OBJHEAD1":
                objhead = child
        if vtint is None or objhead is None:
            continue
        shape = by_id.get(heads.objhead_shape_id(objhead.body))
        if shape is None:
            continue
        count = int.from_bytes(vtint.body[0x0c:0x10], "little", signed=True)
        values = [int.from_bytes(vtint.body[0x10 + 4 * i:0x14 + 4 * i], "little")
                  for i in range(count)]
        out.append((heads.objhead_name(objhead.body), objhead.body, shape, values))
    return out


def placement_of(objhead_body):
    """(location, quaternion) from OBJHEAD1 -- indices in dwords, per scene.py's slices."""
    import struct
    words = list(struct.unpack("<%di" % (len(objhead_body) // 4), objhead_body[:len(objhead_body) // 4 * 4]))
    location = tuple(words[5:8])
    quat = tuple(struct.unpack("<f", struct.pack("<i", w))[0] for w in words[8:12])
    return location, quat


def rotate(quat, v):
    x, y, z, w = quat
    # q * v * conj(q), written out.
    tx = 2.0 * (y * v[2] - z * v[1])
    ty = 2.0 * (z * v[0] - x * v[2])
    tz = 2.0 * (x * v[1] - y * v[0])
    return (v[0] + w * tx + (y * tz - z * ty),
            v[1] + w * ty + (z * tx - x * tz),
            v[2] + w * tz + (x * ty - y * tx))


def sample_vertices(name, objhead_body, shape, values):
    """(position in file space, normal, baked rgb 0..1) per vertex."""
    location, quat = placement_of(objhead_body)
    identity = abs(quat[3] - 1.0) < 1e-6 and max(abs(q) for q in quat[:3]) < 1e-6
    out = []
    normals = shape.vert_normals
    for index, vert in enumerate(shape.verts):
        if index >= len(values):
            break
        p = vert if identity else rotate(quat, vert)
        p = (p[0] + location[0], p[1] + location[1], p[2] + location[2])
        n = normals[index] if index < len(normals) else (0.0, 0.0, 0.0)
        packed = values[index]
        rgb = (((packed >> 16) & 0xFF) / 255.0, ((packed >> 8) & 0xFF) / 255.0,
               (packed & 0xFF) / 255.0)
        out.append((p, n, rgb))
    return out


# --------------------------------------------------------------------------
# candidate models
# --------------------------------------------------------------------------

def light_terms(light):
    pos = tuple(float(v) for v in light["position"])
    colour = (((light["colour"] >> 16) & 0xFF) / 255.0, ((light["colour"] >> 8) & 0xFF) / 255.0,
              (light["colour"] & 0xFF) / 255.0)
    orientation = [v / FIXED for v in light["orientation"]]
    return (pos, colour, light["brightness"] / FIXED, float(light["range"]), light["flags"],
            tuple(orientation[6:9]))


def predict(vertex, lights, model, ambience):
    """One vertex's predicted rgb under `model`.

    The fitted model, and every term in it was chosen by measurement rather than by taste:

        for each light within `range`:
            atten  = (1 - d/range)                      # LFlag_CosAtten
            atten *= max(0, N.L)                        # decisive: r 0.56 -> 0.78 on level01
            if not (flags & Omni):                      # LFlag_CosSpreadAtten
                atten *= max(0, dot(axis, light->vertex))
            sum += colour * brightness * atten
        result = max(ambience, sum * gain)

    `axis` is **row 2** of the orientation matrix. Rows 0 and 1 both make the fit worse and
    NEGATING row 2 collapses it (r 0.02 on level05), which is what identifies it rather than a
    coincidence. `spread` as a cone exponent makes the fit worse on every level, so it is unused.
    """
    (px, py, pz), normal, _ = vertex
    acc = [0.0, 0.0, 0.0]
    length = math.sqrt(normal[0] ** 2 + normal[1] ** 2 + normal[2] ** 2)
    for pos, colour, brightness, rng, flags, axis in lights:
        dx, dy, dz = pos[0] - px, pos[1] - py, pos[2] - pz
        d2 = dx * dx + dy * dy + dz * dz
        if d2 >= rng * rng:
            continue
        d = math.sqrt(d2)
        if model["falloff"] == "linear":
            atten = 1.0 - d / rng
        elif model["falloff"] == "windowed":
            # The fitted linear shape with its TAIL smoothed to zero (notes 4.64).
            #
            # `1 - d/range` reaches zero with a non-zero slope, so per pixel every light draws a
            # disc with a visible rim -- a first-derivative jump, which the eye reads as an edge
            # even though the value itself is continuous. Per VERTEX that is invisible, which is
            # why the fit never saw it: the tail is interpolated across whole terrain triangles.
            #
            # `(1 - t)(1 - t^4)` has zero derivative at t = 1 and is within 6% of linear at the
            # half-range, so it changes the model only where the vertex data had least to say.
            t = d / rng
            atten = (1.0 - t) * (1.0 - t ** 4)
        elif model["falloff"] == "inverse_square":
            # Normalised by range so every model's gain lives on one scale; raw 1/d^2 in rif
            # units is ~1e-8 and would need a gain no sweep here would find.
            atten = 1.0 / max(d2 / (rng * rng), 1e-4)
        elif model["falloff"] == "cosine":
            atten = 0.5 * (1.0 + math.cos(math.pi * d / rng))
        else:
            atten = 1.0
        if atten <= 0.0:
            continue
        if model["lambert"] and length > 1e-9:
            # RIF is Y-down but the normal and the delta share a frame, so no sign flip.
            atten *= max(0.0, (normal[0] * dx + normal[1] * dy + normal[2] * dz) / (d * length))
            if atten <= 0.0:
                continue
        if model["cone"] and not (flags & 0x4):
            atten *= max(0.0, -(axis[0] * dx + axis[1] * dy + axis[2] * dz) / d)
            if atten <= 0.0:
                continue
        for i in range(3):
            acc[i] += colour[i] * brightness * atten
    floor = ambience / FIXED
    return tuple(min(1.0, max(floor, a * model["gain"])) for a in acc)


def evaluate(samples, lights, model, ambience):
    """Mean absolute error in 0..255 units, and the mean predicted/actual luminance."""
    total = 0.0
    pred_sum = 0.0
    actual_sum = 0.0
    for vertex in samples:
        p = predict(vertex, lights, model, ambience)
        a = vertex[2]
        for i in range(3):
            total += abs(p[i] - a[i])
        pred_sum += sum(p) / 3.0
        actual_sum += sum(a) / 3.0
    n = max(1, len(samples))
    return total / (3 * n) * 255.0, pred_sum / n * 255.0, actual_sum / n * 255.0


def correlation(samples, lights, model, ambience):
    """Pearson r between predicted and baked luminance.

    **The metric that decides whether the model SHAPE is right**, where MAE only says whether it
    is scaled right. A wrong gain leaves r untouched; a model that has nothing to do with what
    baked the level drives r to zero however well its mean is tuned.
    """
    xs = []
    ys = []
    for vertex in samples:
        xs.append(sum(predict(vertex, lights, model, ambience)) / 3.0)
        ys.append(sum(vertex[2]) / 3.0)
    n = len(xs)
    mx = sum(xs) / n
    my = sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx <= 1e-12 or syy <= 1e-12:
        return 0.0
    return sxy / math.sqrt(sxx * syy)


def coverage(samples, lights):
    """How many lights reach each vertex at all. If most see none, no sum can explain the bake."""
    counts = []
    for (px, py, pz), _, _ in samples:
        n = 0
        for pos, _, _, rng, _, _ in lights:
            dx, dy, dz = pos[0] - px, pos[1] - py, pos[2] - pz
            if dx * dx + dy * dy + dz * dz < rng * rng:
                n += 1
        counts.append(n)
    lit = sum(1 for c in counts if c > 0)
    return lit / len(counts), sum(counts) / len(counts), max(counts)


def best_gain(samples, lights, model, ambience):
    """The scale that minimises error, by a coarse then fine sweep. One free parameter."""
    best = (None, 1e30)
    for coarse in [0.05, 0.1, 0.2, 0.35, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 5.0, 8.0]:
        model["gain"] = coarse
        mae, _, _ = evaluate(samples, lights, model, ambience)
        if mae < best[1]:
            best = (coarse, mae)
    centre = best[0]
    for step in [centre * f for f in (0.6, 0.7, 0.8, 0.9, 1.1, 1.2, 1.4, 1.7)]:
        model["gain"] = step
        mae, _, _ = evaluate(samples, lights, model, ambience)
        if mae < best[1]:
            best = (step, mae)
    model["gain"] = best[0]
    return best


def pearson(xs, ys):
    n = len(xs)
    mx = sum(xs) / n
    my = sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx <= 1e-12 or syy <= 1e-12:
        return 0.0
    return sxy / math.sqrt(sxx * syy)


def diagnose(samples, lights):
    """The tests that decide whether these lights baked this level AT ALL.

    Kept separate from the model table because they are prior to it: a falloff curve is worth
    arguing about only once the lights are known to carry any information about the result.
    """
    import random

    def normalise(c):
        s = sum(c)
        return (c[0] / s, c[1] / s, c[2] / s) if s > 1e-6 else (1 / 3.0, 1 / 3.0, 1 / 3.0)

    lum = []
    nearest = []
    in_range = []
    normal_y = []
    hue_dominant = []
    hue_random = []
    saturation = []
    random.seed(1)
    for (px, py, pz), normal, rgb in samples:
        lum.append(sum(rgb) / 3.0)
        normal_y.append(normal[1])
        mx, mn = max(rgb), min(rgb)
        saturation.append(0.0 if mx <= 1e-6 else (mx - mn) / mx)
        best_d2 = 1e30
        best_colour = None
        best_weight = 0.0
        count = 0
        for pos, colour, brightness, rng, _, _ in lights:
            d2 = (pos[0] - px) ** 2 + (pos[1] - py) ** 2 + (pos[2] - pz) ** 2
            best_d2 = min(best_d2, d2)
            if d2 < rng * rng:
                count += 1
                weight = brightness / max(d2, 1.0)
                if weight > best_weight:
                    best_weight = weight
                    best_colour = colour
        nearest.append(math.sqrt(best_d2))
        in_range.append(count)
        if best_colour is not None:
            nb = normalise(rgb)
            hue_dominant.append(sum(abs(a - b) for a, b in zip(nb, normalise(best_colour))))
            hue_random.append(
                sum(abs(a - b) for a, b in zip(nb, normalise(random.choice(lights)[1]))))

    grey = sum(1 for s in saturation if s < 0.01)
    print("  baked saturation mean %.3f, %.0f%% effectively grey"
          % (sum(saturation) / len(saturation), 100.0 * grey / len(saturation)))
    print("  correlation of baked luminance with")
    print("    distance to nearest light   r = %+.3f   <- STRONGLY NEGATIVE if these lights baked it"
          % pearson(nearest, lum))
    print("    lights within range         r = %+.3f" % pearson(in_range, lum))
    print("    normal.y (up is negative)   r = %+.3f" % pearson(normal_y, lum))
    if hue_dominant:
        # The control is the whole test: a hue error no better than a randomly chosen light's
        # means the baked colour carries no trace of which light is nearest.
        print("  hue error vs the dominant light %.4f, vs a RANDOM light %.4f  <- control"
              % (sum(hue_dominant) / len(hue_dominant), sum(hue_random) / len(hue_random)))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    gunlok = sys.argv[1]
    wanted = [a.lower() for a in sys.argv[2:]] or ["level02", "level01"]

    for stem in wanted:
        path = None
        for candidate in (stem + ".rif", stem + ".RIF"):
            p = os.path.join(gunlok, "RIF", "Levels", candidate)
            if os.path.exists(p):
                path = p
                break
        if path is None:
            print("%s: not found" % stem)
            continue

        tree = rif.load(path)
        lights_raw, ambience = lights_of(tree)
        if not lights_raw:
            print("%s: no lights" % stem)
            continue
        lights = [light_terms(l) for l in lights_raw]

        objects = objects_with_baked_light(tree)
        if not objects:
            print("%s: no object carries a SHPVTINT" % stem)
            continue
        # The map object is the one with by far the most baked vertices.
        objects.sort(key=lambda o: -len(o[3]))
        name, objhead, shape, values = objects[0]
        samples = sample_vertices(name, objhead, shape, values)
        # Subsample: the fit is a shape question, not a coverage one, and level01's map object
        # has ~100k vertices against 686 lights.
        step = max(1, len(samples) // 4000)
        samples = samples[::step]

        print("=" * 78)
        print("%s: %d lights, ambience %d (%.4f), map object %r, %d of %d vertices sampled"
              % (stem, len(lights), ambience, ambience / FIXED, name, len(samples),
                 len(values)))
        lo = [min(v[0][i] for v in samples) for i in range(3)]
        hi = [max(v[0][i] for v in samples) for i in range(3)]
        llo = [min(l[0][i] for l in lights) for i in range(3)]
        lhi = [max(l[0][i] for l in lights) for i in range(3)]
        print("  vertex extent %s .. %s" % (tuple(int(v) for v in lo), tuple(int(v) for v in hi)))
        print("  light  extent %s .. %s" % (tuple(int(v) for v in llo), tuple(int(v) for v in lhi)))
        actual = sum(sum(v[2]) / 3.0 for v in samples) / len(samples) * 255.0
        print("  baked mean luminance %.1f / 255" % actual)

        diagnose(samples, lights)
        lit_frac, mean_lights, max_lights = coverage(samples, lights)
        print("  reached by >=1 light: %.1f%%   mean %.2f, max %d lights in range"
              % (100.0 * lit_frac, mean_lights, max_lights))

        print("  %-34s %7s %7s %7s" % ("model", "gain", "MAE", "r"))
        # The ablations, in the order they were established. Each row removes one term from the
        # row below it, so the column of r values IS the evidence for every term in the model.
        rows = [
            ("distance only (no N.L, no cone)", {"falloff": "linear", "lambert": False, "cone": False}),
            ("  + N.L", {"falloff": "linear", "lambert": True, "cone": False}),
            ("  + cone on every light", {"falloff": "linear", "lambert": True, "cone": "all"}),
            ("  + cone, omni exempt = FITTED", {"falloff": "linear", "lambert": True, "cone": True}),
            ("fitted, windowed tail", {"falloff": "windowed", "lambert": True, "cone": True}),
            ("fitted, cosine falloff", {"falloff": "cosine", "lambert": True, "cone": True}),
            ("fitted, inverse-square falloff", {"falloff": "inverse_square", "lambert": True, "cone": True}),
        ]
        for label, model in rows:
            if model["cone"] == "all":
                model = dict(model, cone=True)
                lights_used = [(p, c, b, r, f & ~0x4, a) for p, c, b, r, f, a in lights]
            else:
                lights_used = lights
            model["gain"] = 1.0
            gain, mae = best_gain(samples, lights_used, model, ambience)
            r = correlation(samples, lights_used, model, ambience)
            print("  %-34s %7.3f %7.2f %7.3f" % (label, gain, mae, r))


if __name__ == "__main__":
    main()
