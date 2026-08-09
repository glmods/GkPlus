"""A model's reply -> one channel; three channels -> the packed RGB.

Two things happen here that the model is not trusted to have done, and both are
cheap insurance rather than suspicion:

- **The reply is forced to greyscale.** It is asked for greyscale and usually
  obliges, but "usually" is not a property to build a channel on. Luma rather than
  a single channel or a plain mean, so a slight tint costs nothing and a strongly
  tinted answer still lands where a viewer would say it looks.
- **The reply is forced back to the source's size.** The endpoint's ``size`` is a
  preference that providers clamp, and a map at the wrong size is not registered to
  the texture it describes. Lanczos going down (these are 1024-class sheets, so
  down is the case that happens) and bilinear going up.
"""

import io

import numpy as np
from PIL import Image


def to_gray(data, width, height):
    """Reply bytes (PNG or JPEG) -> ``HxW`` uint8 at the source's size."""
    image = Image.open(io.BytesIO(data))
    if image.mode != "L":
        image = image.convert("L")
    if image.size != (width, height):
        down = image.width > width or image.height > height
        image = image.resize((width, height), Image.LANCZOS if down else Image.BILINEAR)
    return np.asarray(image, dtype=np.uint8)


def load_gray(path, width=None, height=None):
    """A greyscale PNG from disk, optionally resized -- the hand-edit path."""
    image = Image.open(path).convert("L")
    if width and height and image.size != (width, height):
        down = image.width > width or image.height > height
        image = image.resize((width, height), Image.LANCZOS if down else Image.BILINEAR)
    return np.asarray(image, dtype=np.uint8)


def save_gray(path, gray):
    Image.fromarray(np.asarray(gray, dtype=np.uint8), mode="L").save(path, optimize=True)


def median(samples):
    """Per-pixel median of several answers to the same question.

    **This is a variance fix, not a quality trick, and it is aimed at a measured
    problem.** Six draws of the identical prompt, model and texture for
    ``units/gunlok_mk2_1024``'s metallic channel came back at 0.226, 0.255, 0.256,
    0.262, 0.356 and 0.446 -- the spread between runs is far larger than the
    difference between any two prompts tried on it, so wording cannot settle what
    sampling has to. Median rather than mean because the failure is an *outlier*
    draw: one sheet answered "a robot is metal, so all of it is bright" and the
    mean would carry a third of that into the result.

    A single sample returns unchanged, so the default path is bit-identical and
    nothing already bought goes stale.
    """
    if len(samples) == 1:
        return samples[0]
    stack = np.stack([np.asarray(s, dtype=np.float32) for s in samples])
    return np.median(stack, axis=0).round().astype(np.uint8)


def combine(bump, metallic, roughness):
    """The three channels, in ``src/VkLighting.h``'s order: R bump, G metallic, B roughness.

    That order is the interface. Getting it wrong does not fail anywhere -- the
    renderer reads whatever is in each channel as that channel's quantity -- so a
    swapped pair shows up as a surface that is glossy where it should be matte,
    which reads as a bad answer from the model rather than as a bug here.
    """
    shapes = {a.shape for a in (bump, metallic, roughness)}
    if len(shapes) != 1:
        raise ValueError("channels disagree about size: %r" % (sorted(shapes),))
    return np.dstack([bump, metallic, roughness]).astype(np.uint8)


def to_png_bytes(rgb):
    """``HxWx3`` uint8 -> PNG bytes, which is how the source reaches the model."""
    buf = io.BytesIO()
    Image.fromarray(np.asarray(rgb, dtype=np.uint8), mode="RGB").save(buf, "PNG")
    return buf.getvalue()
