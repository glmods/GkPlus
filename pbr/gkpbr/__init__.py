"""Generate PBR map sets from Gunlok's ``.RIM`` textures.

The pipeline and the reasoning behind its shape are in ``pbr/README.md``. The short
version: the model decides *what a material is*, deterministic arithmetic decides
*what every pixel is*, and two measurable gates stand between the two.
"""

__all__ = ["assets", "atlas", "classify", "derive", "generate", "images", "metrics"]
