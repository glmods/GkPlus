"""The three prompts, and nothing else.

One per channel of the lighting map, and the channel semantics are
``src/VkLighting.h``'s rather than PBR's -- which is the whole reason these are
written out longhand instead of saying "roughness map" and hoping:

  * **R -- bump.** A *height field*. The renderer takes its gradient against a
    tangent frame from the fragment's own derivatives; it never reads it as a
    normal map, so an RGB normal map here would light as noise.
  * **G -- metallic.** The **intensity** of the highlight, straight through. Not a
    metal/dielectric switch, and nothing about the base colour changes with it --
    so the question to ask a model is "how strong a highlight", not "is this metal".
  * **B -- roughness.** The **sharpness** of the highlight, as a specular exponent:
    0 is the sharpest, 1 the broadest. That is the ordinary sense of roughness, so
    this is the one channel a stock answer suits.

:data:`RULES` is appended to each. It is the half that does not vary and the half
that decides whether the result is usable at all: a map that is cropped, padded,
rotated or given a border is not registered to the texture it describes, and
nothing downstream can put it back.
"""

#: Named when the caller knows it, which is always in practice.
#:
#: **The name is evidence and is not authority**, and saying so is most of why this
#: is a separate block. Gunlok's artists named these usefully -- ``city wall conc
#: floor 01``, ``gravel``, ``wetsand``, ``beige rock 1024`` -- and a model shown the
#: picture alone was reading bright specks in a concrete photograph as gloss. But
#: the same directory holds ``s3 level 1k 38 rygb`` and ``couldron00``, so a prompt
#: that treated the name as a specification would invent a material out of an
#: artist's shorthand. Hence "if it disagrees with what you see, believe the image".
CONTEXT = """
This texture's file name in the game is: "%s"

That is the artist's own name for the asset. It is often descriptive of the
material, and it is sometimes an abbreviation, a level code or meaningless. Use it
as a hint about what you are looking at; where it disagrees with what you can see,
believe the image.
"""

#: What every request needs regardless of which map it asks for.
RULES = """
Hard requirements, all of them:
- Output ONE image and no text.
- It must be greyscale: every pixel's red, green and blue equal.
- It must be pixel-for-pixel aligned with the input, at the same size and framing.
  Do not crop, pad, zoom, rotate, mirror, re-compose or re-draw it. Every feature
  must stay exactly where it is in the input, to the pixel.
- Do not add a border, a frame, a mat, a caption, a watermark, a label, a colour
  swatch or an example strip. The output is the map and nothing else.
- Do not stylise, sharpen, denoise or "improve" anything. This is a data channel,
  not a picture.
- The input is a texture sheet from a 2000-era 3D game and is often an ATLAS: it
  may hold several unrelated materials side by side with no gutter between them.
  Judge each patch on its own terms and keep the boundaries between them crisp.
"""

#: R -- the height field the renderer differentiates.
BUMP = """
From this texture, produce a greyscale HEIGHT MAP (a bump map).

Brightness is height above the surface, nothing else:
- White: the parts that stick out furthest -- rivets, bolt heads, ridges, raised
  panel edges, the faces of bricks, pebbles standing proud of the sand.
- Mid grey: the base level of the surface.
- Black: the deepest recesses -- grooves, panel gaps, mortar lines, holes, cracks,
  the space between plates.

Read GEOMETRY, not paint and not lighting. A dark painted stripe on flat metal is
flat, so it stays mid grey. A groove that happens to catch the light is still a
groove, so it goes dark. A photographed surface already carries its own shading;
use it as evidence about shape, but the map you output describes the shape.

Keep the field smooth where the surface is smooth. Per-pixel noise across a flat
area becomes visible sparkle in the renderer, so a flat material should come out
nearly flat.
"""

#: G -- highlight intensity, which is not a metal mask.
METALLIC = """
From this texture, produce a greyscale HIGHLIGHT INTENSITY mask.

This value scales how strong a specular highlight the surface shows. It is not a
metal-versus-dielectric switch and it does not change the surface's colour, so
answer the question "how shiny is this", not "is this metal":
- White: bare, clean metal; polished plastic; glass; wet or glazed surfaces.
- Mid grey: painted or lightly worn metal, smooth stone, sealed floors.
- Black: matte surfaces that catch no highlight at all -- concrete, dirt, sand,
  grass, rust, rubber, cloth, foam, unfinished stone, soot.

**Black is the default answer, and most of this game's surfaces are black.** Unless
you can name the specific thing that is shiny -- this bolt head, that pane of
glass, this puddle, this strip of bare metal -- the answer for that area is black.
Concrete, stone, rock, sand, gravel, dirt, mud, rubble, brick, plaster, tarmac,
rust and weathered paint are all black even when they are bright, and a photograph
of a rough material is full of small bright specks that are its texture catching
the photographer's light, not gloss. Do not turn those into highlights.

Be decisive. A surface is usually one thing or the other, and a whole sheet hedged
around 0.5 paints a dull sheen over everything. Where one material meets another,
the boundary should be a hard edge, not a gradient.
"""

#: B -- highlight sharpness, in the ordinary sense of roughness.
ROUGHNESS = """
From this texture, produce a greyscale ROUGHNESS map.

This controls how tight or how broad the specular highlight is:
- Black: perfectly smooth -- a small, sharp, mirror-like highlight. Polished metal,
  glass, glaze, wet surfaces.
- Mid grey: lightly worn or brushed surfaces, satin paint.
- White: very rough -- a broad, soft sheen or none at all. Concrete, sand, rust,
  bare rock, corroded or pitted metal, cloth.

Vary it with the surface: scratches, wear along an edge, a scuffed patch or a
weld seam are rougher than the plate around them, and that variation is most of
what makes the map worth having. Follow the material, not the lighting -- a bright
patch is not automatically smooth.
"""

#: Which prompts carry :data:`CONTEXT`, and this is a **measurement rather than a
#: preference**. The asset name was added to answer a real defect in one channel
#: (see the README), and a three-way A/B on eight ground textures says the name on
#: its own makes that channel *worse* -- matte mean 0.330 without it, 0.404 with it,
#: 0.036 with it plus the "black is the default" paragraph. Nothing was measured
#: about what it does to a height field or to a roughness map.
#:
#: So it goes where it has been shown to earn its place and nowhere else. Adding it
#: to the other two would also invalidate every answer already bought for them --
#: 448 maps, about $11 -- on the strength of an untested intuition, which is the
#: trade this constant exists to refuse. Widen it after an A/B, not before.
WANTS_CONTEXT = frozenset({"metallic"})

#: Channel name -> (prompt, index into the packed RGB).
MAPS = {
    "bump": (BUMP, 0),
    "metallic": (METALLIC, 1),
    "roughness": (ROUGHNESS, 2),
}

#: The order they are generated and reported in, which is also the channel order.
ORDER = ("bump", "metallic", "roughness")


def prompt_for(name, texture=None):
    """The full prompt sent for one map: its own text, the asset name, :data:`RULES`.

    ``texture`` is the asset's own name as the game spells it (``ground/gravel``).
    It reaches the prompt only for the maps in :data:`WANTS_CONTEXT`, so passing it
    unconditionally is correct and is what the CLI does. The measurement behind
    that is in ``lightmap/README.md`` under "The metallic channel came back far too
    high".

    The assembly is ``body \\n [context] \\n rules \\n``, and the **single** newline is
    load-bearing: a prompt is what a cached answer is fingerprinted against, so
    joining the unchanged blocks with a blank line instead would invalidate every
    bump and roughness answer ever bought -- 448 maps here, about $11 -- over
    whitespace. A prompt that did not change must render byte-identically.
    """
    body, _ = MAPS[name]
    parts = [body.strip()]
    if texture and name in WANTS_CONTEXT:
        parts.append("\n" + CONTEXT.strip() % texture + "\n")
    parts.append(RULES.strip())
    return "\n".join(parts) + "\n"
