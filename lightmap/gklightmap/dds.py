"""Writing the one DDS shape ``src/Dds.cpp`` accepts for a lighting map.

**Uncompressed 24-bit B,G,R with a full mip chain**, and each half of that is a
decision:

- **Uncompressed rather than DXT1.** ``src/VkLighting.h`` recommends DXT1 to a
  modder and it is the right default for artwork; it is the wrong one here. Two of
  the three channels are *masks* -- highlight intensity and sharpness -- and DXT1
  quantises a block's three channels against a single pair of endpoints, so a
  metallic edge and a roughness edge inside one 4x4 block drag each other around.
  There is no S3TC compressor in this tool's dependency set either (``pbr``'s
  ``preview`` shells out to ``utils/rimutil`` for that, which writes ``.RIM`` and
  not ``.dds``), so the choice is between correct and absent.
- **A full chain down to 1x1.** The 4x4 mip floor that ``Dds.h`` documents is an
  S3TC rule -- the engine's row loop decrements by 4 and can only terminate on a
  multiple of it -- and uncompressed levels are explicitly exempt. Nothing about
  the floor applies to the Vulkan path at all, which decodes these itself, but a
  file that is legal to both is worth more than one that is legal to one.
  A chain is not optional: ``pbr``'s own preview run measured what a mip-less map
  with texel-scale content does at minification, and the answer was full-amplitude
  speckle.

The cost is size -- a 1024 map is 4 MB against DXT1's 0.7 -- and it buys exactness
on a channel set where a wrong value is a wrong material rather than a slightly
wrong colour.
"""

import struct

import numpy as np
from PIL import Image

MAGIC = b"DDS "

DDSD_CAPS = 0x00000001
DDSD_HEIGHT = 0x00000002
DDSD_WIDTH = 0x00000004
DDSD_PITCH = 0x00000008
DDSD_PIXELFORMAT = 0x00001000
DDSD_MIPMAPCOUNT = 0x00020000

DDPF_RGB = 0x00000040

DDSCAPS_COMPLEX = 0x00000008
DDSCAPS_TEXTURE = 0x00001000
DDSCAPS_MIPMAP = 0x00400000


def mip_chain(rgb):
    """``[HxWx3 uint8]`` from the base level down to 1x1, halving by box filter.

    Box rather than Lanczos: these are data channels, and a filter with negative
    lobes overshoots past 0 and 255 at every hard material boundary -- which on the
    metallic channel is a rim of spurious highlight around every patch.
    """
    levels = [rgb]
    height, width = rgb.shape[:2]
    while width > 1 or height > 1:
        width = max(1, width // 2)
        height = max(1, height // 2)
        image = Image.fromarray(levels[-1], mode="RGB").resize((width, height), Image.BOX)
        levels.append(np.asarray(image, dtype=np.uint8))
    return levels


def header(width, height, levels):
    """The 128 bytes ``gk::dds::ParseHeader`` reads, for an R8G8B8 source.

    The masks are the only pair that file accepts for a 24-bit image
    (``0x00ff0000``/``0x0000ff00``/``0x000000ff``, i.e. B,G,R in memory); anything
    else would need a swizzle in the row converter and is refused by name.
    """
    flags = (DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT
             | DDSD_MIPMAPCOUNT)
    caps = DDSCAPS_TEXTURE | DDSCAPS_COMPLEX | DDSCAPS_MIPMAP
    return b"".join([
        MAGIC,
        struct.pack("<7I", 124, flags, height, width, width * 3, 0, levels),
        b"\0" * 44,                                   # dwReserved1[11]
        struct.pack("<2I", 32, DDPF_RGB),             # DDS_PIXELFORMAT.dwSize, flags
        struct.pack("<2I", 0, 24),                    # fourCC (none), bit count
        struct.pack("<4I", 0x00ff0000, 0x0000ff00, 0x000000ff, 0),
        struct.pack("<5I", caps, 0, 0, 0, 0),         # caps1..4, dwReserved2
    ])


def encode(rgb, mips=True):
    """``HxWx3`` uint8 (R,G,B) -> the bytes of a complete ``.dds`` file.

    The array is in the *channel* order the renderer reads -- R bump, G metallic,
    B roughness -- and is reversed on the way out, because a 24-bit DDS stores
    B,G,R.
    """
    if rgb.ndim != 3 or rgb.shape[2] != 3:
        raise ValueError("expected an HxWx3 array, got %r" % (rgb.shape,))
    rgb = np.ascontiguousarray(rgb.astype(np.uint8))
    levels = mip_chain(rgb) if mips else [rgb]
    out = [header(rgb.shape[1], rgb.shape[0], len(levels))]
    for level in levels:
        out.append(np.ascontiguousarray(level[..., ::-1]).tobytes())
    return b"".join(out)


def write(path, rgb, mips=True):
    with open(path, "wb") as fh:
        fh.write(encode(rgb, mips=mips))
