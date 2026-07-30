"""Numpy <-> PNG, and the one place a decoded ``.RIM`` becomes an array.

Everything downstream works in float ``HxWx{3,4}`` in 0..1 with row 0 at the top,
which is the orientation :class:`rim.Texture` already uses, so nothing flips.
"""

import io

import numpy as np
from PIL import Image


def from_texture(tex):
    """A decoded :class:`rim.Texture` -> float ``HxWx4`` in 0..1."""
    arr = np.frombuffer(tex.rgba, dtype=np.uint8).reshape(tex.height, tex.width, 4)
    return arr.astype(np.float32) / 255.0


def load(path):
    img = Image.open(path)
    img = img.convert("RGBA" if img.mode in ("RGBA", "LA", "P") else "RGB")
    return np.asarray(img).astype(np.float32) / 255.0


def load_bytes(data):
    """PNG/JPEG bytes -> float array. What a model response goes through."""
    img = Image.open(io.BytesIO(data))
    img = img.convert("RGBA" if img.mode in ("RGBA", "LA") else "RGB")
    return np.asarray(img).astype(np.float32) / 255.0


def save(path, arr, gray=False):
    """Write a float array as an 8-bit PNG.

    ``gray`` collapses to a single channel, which is what roughness, metalness and
    height want: a one-channel PNG is a third the size and leaves no doubt about
    which channel a consumer should read.
    """
    a = np.clip(arr, 0.0, 1.0)
    if gray:
        if a.ndim == 3:
            a = a[..., 0]
        Image.fromarray((a * 255.0 + 0.5).astype(np.uint8), mode="L").save(path, optimize=True)
        return
    mode = "RGBA" if a.ndim == 3 and a.shape[2] == 4 else "RGB"
    if a.ndim == 2:
        a = np.repeat(a[..., None], 3, axis=2)
        mode = "RGB"
    Image.fromarray((a[..., :len(mode)] * 255.0 + 0.5).astype(np.uint8), mode=mode).save(
        path, optimize=True)


def to_png_bytes(arr):
    """Float array -> PNG bytes, for handing an image to a model."""
    a = np.clip(arr, 0.0, 1.0)
    if a.ndim == 2:
        a = np.repeat(a[..., None], 3, axis=2)
    buf = io.BytesIO()
    Image.fromarray((a[..., :3] * 255.0 + 0.5).astype(np.uint8), mode="RGB").save(buf, "PNG")
    return buf.getvalue()


def resize(arr, width, height):
    """Nearest-free resize through Pillow, used to bring a model result back to size.

    Lanczos on the way *down* and bilinear on the way up: a model asked for 1K when
    the source is 512 returns something that has to be halved, and halving with a
    box filter would alias the detail the gate then measures.
    """
    a = np.clip(arr, 0.0, 1.0)
    mode = "RGB" if a.ndim == 3 else "L"
    if a.ndim == 2:
        img = Image.fromarray((a * 255.0 + 0.5).astype(np.uint8), mode="L")
    else:
        img = Image.fromarray((a[..., :3] * 255.0 + 0.5).astype(np.uint8), mode="RGB")
    down = width < img.width or height < img.height
    out = img.resize((width, height), Image.LANCZOS if down else Image.BILINEAR)
    res = np.asarray(out).astype(np.float32) / 255.0
    return res if mode == "RGB" else res


def roll_half(arr):
    """Shift by half the image on both axes, wrapping.

    The seam-free trick: a texture generated from this has its seams in the middle,
    so rolling the result back and cross-blending puts an interior region over every
    edge. Only worth the second call on a texture that has to tile.
    """
    h, w = arr.shape[:2]
    return np.roll(np.roll(arr, h // 2, axis=0), w // 2, axis=1)


def blend_seamless(direct, from_rolled):
    """Combine a map with its half-offset twin, favouring each away from its seams.

    ``from_rolled`` must already be rolled *back* into the original frame. The
    weight is a raised cosine that is 1 at the centre of the direct image and 0 at
    its edges, so the offset twin -- whose own centre sits on those edges -- covers
    exactly where the direct one is unreliable.
    """
    h, w = direct.shape[:2]
    wy = 0.5 - 0.5 * np.cos(2.0 * np.pi * (np.arange(h) + 0.5) / h)
    wx = 0.5 - 0.5 * np.cos(2.0 * np.pi * (np.arange(w) + 0.5) / w)
    weight = np.sqrt(np.outer(wy, wx))
    if direct.ndim == 3:
        weight = weight[..., None]
    return direct * weight + from_rolled * (1.0 - weight)
