"""What a cached model answer was computed from, so staleness is detectable.

Stage 1's JSON is what makes the pipeline re-runnable: :mod:`derive` can be rewritten
and every map regenerated with no further model calls because the classification is
already on disk. That only holds while the cached answer is still an answer to
*today's* question, and until this module the cache key was the **file name** -- a
texture with a ``materials/<slug>.json`` was skipped whatever that JSON was an answer
to.

Four inputs move underneath such a file, and every one of them moves silently:

- **Region ids are patch labels from** :func:`atlas.segment`. Change
  ``MIN_AREA_TEXELS``, ``MAX_REGIONS``, the rasterizer or the tiling test and id 3
  denotes a different region. The cached answer is then re-pointed at the wrong
  texels and :mod:`derive` paints roughness and metalness accordingly, with nothing
  raised anywhere.
- **The albedo is not immutable either.** The decoders come from
  ``blender/io_scene_rif``, which moves on its own: ``rim.py`` gained the palettized
  ``BODY`` path in a533756 and ``structures/eog_cylinder`` entered the manifest as a
  result. See ``tests/test_addon_boundary.py`` for the other half of that seam.
- **The prompt and the response schema are inputs.** ``classify.SYSTEM`` has been
  edited once already -- to stop ``metallic`` hedging in a range that is physically
  meaningless -- which invalidated every answer taken before it, and nothing noticed.
- **The model.** ``--model`` is a flag; a set half-answered by one model and half by
  another is not a set anybody can reason about.

So a cached entry carries a fingerprint of its inputs. The fingerprint is **one
digest per input rather than one rolled-up hash**, because "your prompt changed" and
"the albedo changed" mean very different things to whoever is looking, and telling
them apart costs an extra sha256 over a few kilobytes rather than any lookup.

Nothing here knows what the pipeline's inputs *are*: :func:`classify.fingerprint` and
:func:`generate.fingerprint` decide that, each in the module that owns the inputs it
covers, and each documents what it leaves out and why. A fingerprint over too much is
as bad as one over too little -- it spends money re-asking on a change that could not
have altered the answer.
"""

import hashlib
import json

#: How much of the sha256 a fingerprint entry keeps. 64 bits is far past what a
#: change detector over a few hundred cache entries needs -- this guards against a
#: file moving underneath us, not against anyone constructing a collision -- and the
#: files it lands in are meant to be read by people. "Reviewable, diffable, editable
#: by hand" is the whole reason stage 1 answers in JSON, and eight full hashes at the
#: top of a small document is enough to stop being able to read it.
DIGEST_CHARS = 16

#: The value stored for an input that is legitimately not there, so that gaining one
#: later reads as a change rather than as an absence matching an absence. A
#: whole-sheet texture has no label image; if it is re-segmented into regions and
#: acquires one, that must invalidate.
ABSENT = "absent"


def digest(data):
    """Truncated sha256 of one input. ``str`` is hashed as UTF-8."""
    if isinstance(data, str):
        data = data.encode("utf-8")
    return hashlib.sha256(data).hexdigest()[:DIGEST_CHARS]


def digest_json(obj):
    """A structure's digest, canonicalised so key order alone cannot move it."""
    return digest(json.dumps(obj, sort_keys=True, separators=(",", ":")))


def changed(stored, current):
    """Which inputs moved: ``[]`` for none, ``None`` for *unknown*.

    Three states rather than two, deliberately. A file written before fingerprints
    existed carries no evidence either way: calling it valid re-adopts an answer to
    an unknown question, and calling it invalid throws away work that is probably
    fine and charges for it. It is reported as unknown and left to a decision -- see
    ``cli.cmd_classify``, which will not spend on one and offers ``--adopt-cached``
    for the operator to assert what the code cannot check.

    The comparison is over the **union** of the keys, so adding an input to a
    fingerprint invalidates every entry written before it -- which is correct, since
    an entry written before it has no evidence about the new input either. There is
    deliberately no format-version field: the key names carry that themselves, and
    name the thing that moved while a version number names nothing.
    """
    if not stored:
        return None
    return [k for k in sorted(set(stored) | set(current))
            if stored.get(k) != current.get(k)]


#: A fingerprint key in words. The report says which input moved because the fixes
#: are not the same: a changed albedo means the decoder or `inventory` moved under
#: the cache, a changed prompt means the segmentation did, and a changed system
#: prompt means every entry in the set is stale at once.
MEANS = {
    "albedo": "the albedo image",
    "labels": "the region label image",
    "prompt": "the prompt (regions, part names or sheet statistics)",
    "system": "the system prompt",
    "schema": "the response schema",
    "model": "the model",
    "tiling": "whether the sheet tiles (and so whether the seam pass ran)",
}


def explain(diff):
    """``['albedo', 'prompt']`` -> a phrase naming what moved."""
    return ", ".join(MEANS.get(k, k) for k in diff)
