// The world pass: vertex pulling out of the canonical arena, bindless textures, one pipeline.
//
// Both entry points live in one file because they share the push constant block, and a push
// constant block must match across every stage of a pipeline - keeping them together makes that
// structural rather than a convention to remember.

// Must match vulkan::CanonicalVertex byte for byte (src/VertexFormat.h): pos 0, normal 16,
// color 28, uv0 32, uv1 40, size 48.
//
// **Three float4s and not the natural {float4, float3, uint, float2, float2}**, which is the
// obvious spelling and is a trap. A float3 followed by a scalar lays out differently under
// std140, std430 and scalar rules, so the natural struct only matches the C one if Slang picks
// the rule this code assumes - and it does not say which it picked. Three float4s are 48 bytes
// with their fields at 0/16/32 under every rule there is, so the agreement is structural
// instead of a bet. The unpacking below is free.
struct Vertex {
    float4 pos;               // xyz + w: rhw for a pre-transformed vertex, 1.0 otherwise
    float4 normal_and_color;  // xyz = normal, w = the D3DCOLOR's bits
    float4 uv;                // xy = uv0, zw = uv1
};

// One D3D8 light, and everything about a draw that is not a texture stage. Both must match
// GpuLight and GpuDrawRecord in src/VkDraw.h field for field.
//
// Everything is a float4 or an array of them for the layout reason above: a float4 has a
// 16-byte stride under every rule, so `float4 mvp[4]` cannot be laid out two ways, where
// `float m[16]` could. The four trailing uints are the exception, and they are last precisely
// so no rule can disagree about where they start.
struct GpuLight {
    float4 position;     // xyz world space
    float4 direction;    // xyz world space, normalised
    float4 diffuse;
    float4 specular;
    float4 ambient;
    float4 attenuation;  // att0, att1, att2, range
    float4 spot;         // cos(theta/2), cos(phi/2), falloff, type (1 point, 2 spot, 3 dir)
};

struct GpuDrawRecord {
    float4 mvp[4];
    float4 world[4];
    float4 normal_transform[3];
    float4 material_ambient;
    float4 material_diffuse;
    float4 material_specular; // w = D3DMATERIAL8::Power
    float4 material_emissive;
    float4 global_ambient;    // D3DRS_AMBIENT
    float4 eye;               // xyz: the camera in world space
    // world->view rotation, 3 rows of 4. Only the chrome pass reads it; see src/VkDraw.h for why
    // it is the rotation alone and why a transpose is enough.
    float4 view_rotation[3];
    uint light_offset;
    uint light_count;
    uint lighting;
    uint lit_colour;
};

// One material: the two texture stages and the alpha test. Must match GpuMaterial in
// src/VkDraw.h field for field, and 64 bytes is a multiple of 16, so the array stride is the
// same under std140, std430 and scalar rules alike.
//
// The stages are flat rather than an array for the same layout reason the vertex is three
// float4s: an array of scalars is padded to a 16-byte stride under std140 and is not under
// scalar rules, so `uint stage_texture[2]` would be a bet on which one Slang picked. Named
// scalars cannot be laid out two ways.
//
// `color` and `alpha` each pack one D3D texture-stage operation:
//     bits  0..7   D3DTEXTUREOP
//     bits  8..15  the first argument  (D3DTA_*, complement and alpha-replicate included)
//     bits 16..23  the second argument
//     bits 24..31  D3DTSS_TEXCOORDINDEX, meaningful on the colour word only
struct GpuMaterial {
    uint stage0_texture; // bindless slot, 0xffffffff for untextured
    uint stage0_sampler;
    uint stage0_color;
    uint stage0_alpha;
    uint stage1_texture;
    uint stage1_sampler;
    uint stage1_color;
    uint stage1_alpha;
    uint stage_count;    // 0, 1 or 2 - stages up to the first D3DTOP_DISABLE
    // The alpha test: D3DCMPFUNC in bits 0..3, 0 meaning the test is off, and D3DRS_ALPHAREF
    // in bits 8..15. A `discard` and not pipeline state, because it varies per material with the
    // same blend and depth settings - making it part of the pipeline key would multiply the
    // pipeline count by the number of distinct references for nothing.
    uint flags;
    uint shading;        // D3DRS_SHADEMODE: 1 FLAT, anything else GOURAUD
    // The material override's tint, RGBA8 with R in bits 0..7. 0xffffffff for every material the
    // game itself produces, which is the identity, so nothing here branches on whether an
    // override exists.
    uint tint;
    // The lighting map for this surface's stage-0 texture, or kNoTexture - which is what every
    // material the game itself produces carries. R is a height field, G the highlight's intensity
    // and B its sharpness; see src/VkLighting.h for where the file comes from and what the three
    // channels are allowed to mean.
    uint lighting_texture;
    // 1 when this material is Gunlok's chrome pass: stage 1 samples `units\reflect.rim`, ADDSIGNED
    // over the unit's own texture, on a second draw of the same mesh. The capture layer decides
    // this by name - a bindless slot has none here. See src/VkLighting.h.
    uint chrome;
    uint pad1;
    uint pad2;
};

// One of the LEVEL's own lights - the rig that baked its per-vertex colours. Must match
// GpuMapLight in src/VkDraw.h. Colour is premultiplied by brightness on the CPU.
struct GpuMapLight {
    float4 position;  // xyz world space, w = range
    float4 colour;    // rgb premultiplied, w = engine_light_flags as a float
    float4 axis;      // xyz = row 2 of the orientation, the light's axis
};

// Must match kMaxShadowCascades in src/VkDraw.h: the atlas is a 2x2 grid of tiles, and the tile
// index below is derived from a cascade's number exactly as RecordShadowPass derives its viewport.
static const uint kMaxShadowCascades = 4u;

static const uint kShadeFlat = 1u;
// LFlag_Omni, from AvP's prototyp.h. The one flag bit this shader reads, and it decides whether
// the cone applies - measured, not assumed: applying the cone to an omni light makes level04's
// fit against the real bake go 0.831 -> 0.640 (notes §4.54).
static const uint kMapLightOmni = 0x4u;

// Everything that is the same for every draw in a frame. Must match GpuFrameData in
// src/VkDraw.h field for field - the 8-byte members first, where no layout rule can disagree
// about their alignment, then the 4-byte scalars.
struct GpuFrameData {
    ConstBufferPointer<GpuMapLight> map_lights;
    ConstBufferPointer<uint> light_grid;
    ConstBufferPointer<uint> light_indices;
    float force_lod;
    float bump_scale;
    float bump_diffuse;
    float specular_scale;
    float gloss_min;
    float gloss_max;
    float specular_from_diffuse;
    float chrome_scale;
    float chrome_blur;
    float chrome_texgen;
    float per_pixel_lighting;
    float map_light_gain;
    float map_ambience;
    uint map_light_count;
    uint map_flags;
    // The sun's shadow map. `shadow_texture == kNoTexture` is the one test - "no sun", "the knob
    // is off" and "this device has no shadow pipeline" are all that state, and none is a branch
    // of its own.
    uint shadow_texture;
    uint shadow_sampler;
    float shadow_strength;
    float shadow_texel;      // 1 / the TILE, so a PCF tap is a step in a cascade's own uv
    float shadow_z_near;
    float shadow_z_span;
    uint shadow_cascades;    // how many of `cascades` are live, 0 for none
    // The map lights' static shadow atlas. Same one test as above: kNoTexture is "off", "not
    // created" and "this level has no map lights" all at once.
    uint map_shadow_texture;
    uint map_shadow_sampler;
    float map_shadow_offset; // along the normal, in atlas texels at the fragment's own distance
    // The per-frame atlas (§4.66). Its own bias because its face is 128 texels where the static
    // one's is 64, so one texel is half the world distance.
    uint dyn_shadow_texture;
    uint dyn_shadow_sampler;
    float dyn_shadow_offset;
    // The PCF radius for D3D's point and spot lights, in texels: 0 a single tap, 1 a 3x3, 2 a 5x5
    // (§4.69). It applies to whichever atlas serves them, which is why it is not named for either.
    // **This was `pad1`**, so it costs no size change and `cascades` stays on its boundary.
    uint local_shadow_taps;
    // The PN-triangle amplification pass (§4.71). Eight scalars, the last two padding, because
    // `cascades` has to stay on a 16-byte boundary - see the header for why six became eight.
    float tess_edge_pixels;
    float tess_max;
    float tess_min;
    float pn_strength;
    float pn_flat_threshold;
    float target_width;
    float target_height;
    // The ceiling on how far a control point may leave its chord, in world units (§4.74).
    // **This was `pad_tess`**, so it costs no size change and `cascades` stays on its boundary.
    float pn_max_offset;
    // **Last, and it has to stay last** - the header's `offsetof` asserts pin `cascades` to 176
    // and nothing pins a field before it. This block sat above `dyn_shadow_texture` here while
    // the header had it below `pad1`, and the two disagreed for four fields with no diagnostic
    // (§4.67): a permutation preserves the size, so `sizeof(GpuFrameData) == 272` still held.
    uint light_flags;        // kLocalLightsOn / kMapShadowsOn / kLocalShadowsOn
    // Per cascade: xy the box's snapped centre in light space, z the reciprocal of its
    // half-extent, w its depth bias already converted out of texels.
    float4 cascades[kMaxShadowCascades];
    // World -> **light space**: the sun's basis, no scale and no translation, so the result is in
    // world units and a cascade is a centre and a half-extent in it. One transform, four boxes.
    float4 sun_matrix[4];
    // Screen-space ambient occlusion (§4.86), appended AFTER `sun_matrix` on purpose: everything
    // above it has a pinned offset in src/VkDraw.h and the end is the one place a field can be
    // added without moving any of them. `kNoTexture` is the single test again - "off", "the pass
    // could not be created" and "this frame drew no occluder" are one state here as they are for
    // the three shadow maps.
    uint ao_texture;
    uint ao_flags;   // kAoMapOnly / kAoDebug
    // How much the occlusion also scales the DIRECT sum. 0 is ambient only, which is what
    // occlusion means physically; 1 darkens everything and is the knob for a stylised look.
    float ao_direct;
    float pad_ao;
    // --- the PN split-corner table (§4.71) --------------------------------------------------
    //
    // One bit per canonical vertex: set when the mesh has split this corner into vertices
    // carrying different normals, so the tangent term there has to be zeroed on *both* sides or
    // the two patches build different boundary curves through the same two points and tear apart.
    // Appended after the AO block for the reason that block was appended after `sun_matrix`.
    //
    // `split_count` of 0 is the single test - "the fix is off", "no level is loaded" and "the
    // table has not been built yet" are all that state, and none is a branch of its own.
    ConstBufferPointer<uint> split_corners;
    uint split_base;   // the canonical-vertex index bit 0 stands for
    uint split_count;  // how many bits are valid
    // --- HDR (the plan's "In progress: HDR") -------------------------------------------------
    //
    // kLinearInput / kUnclamped, appended for the reason the two blocks above were: every field
    // with a pinned `offsetof` in src/VkDraw.h is above here, and the end is the only place a
    // field can go without moving one of them.
    uint colour_flags;
    // How far the lighting map's diffuse ratio may carry a pixel from the colour the game lit it
    // (§4.98): the ratio is clamped to `[1 / limit, limit]`, so 4 lets a bump brighten or darken a
    // surface fourfold and 1 turns the diffuse half off without touching the highlight.
    // **This was one of the three `pad_colour` words**, so it costs no size change and every
    // pinned offset in src/VkDraw.h stays where it is.
    float bump_diffuse_limit;
    // **Two words of padding and not one**, and it is the same constraint the tessellation block
    // above records: `cascades` is a `float4[]`, so std430 aligns the whole struct to 16 while the
    // scalar rule aligns it to 8, and the two only agree on a size that is a multiple of 16.
    // `src/gen-shader-abi.py` refuses a struct whose two layouts disagree, which is what caught
    // this rather than a wrong picture later.
    uint pad_colour[2];
    // --- soft sun shadows, PCSS (notes 4.100) --------------------------------------------------
    //
    // Appended for the reason every block since `sun_matrix` has been: every field with a pinned
    // offset in src/VkDraw.h is above this, and the end is the only place a field can be added
    // without moving one of them. Four words, so the struct stays a multiple of 16.
    //
    // `shadow_softness` is the **tangent of the sun's angular radius** - the penumbra's world
    // radius per world unit between a fragment and whatever shadows it. **0 is the single test
    // for "hard"**, and it takes the 3x3 path this file had before PCSS existed, unchanged, so
    // the default is bit-identical to that build rather than approximately it.
    float shadow_softness;
    // The filter's floor and ceiling, in texels of whichever cascade the fragment landed in. The
    // floor keeps a contact shadow at least as filtered as the old 3x3 was; the ceiling bounds
    // both how sparse the kernel may get and how far a tap may stray from its own tile.
    float shadow_soft_min;
    float shadow_soft_max;
    // How many of `kShadowDisc` each of the two loops walks. Clamped to the table on the CPU.
    uint shadow_soft_taps;
    // The screen-space mask (shadowmask.slang). `kNoTexture` is the single test: "the knob is
    // off", "the pass could not be built" and "it did not run this frame" are one state here.
    uint shadow_mask_texture;
    // The prepass's world position, which is what makes reading the mask *safe*: a fragment only
    // takes it when the pixel it would read is the surface being shaded.
    uint shadow_mask_position;
    uint pad_mask[2];
};

// **56 bytes of a guaranteed 128.** It reached exactly 128 with the light grid (§4.56), and
// everything frame-uniform then moved into GpuFrameData above - which is what a value that is the
// same for every draw should always have been. The four hot addresses stay here because the
// vertex shader reaches `vertices` before it has read anything at all, and putting it behind
// `frame` would make that a dependent load on every vertex.
struct Push {
    ConstBufferPointer<Vertex> vertices;
    ConstBufferPointer<GpuDrawRecord> draws;
    ConstBufferPointer<GpuLight> lights;
    ConstBufferPointer<GpuMaterial> materials;
    ConstBufferPointer<GpuFrameData> frame;
    uint record;         // this draw's entry in `draws`
    uint material;       // ... and in `materials`, shared with every draw of the same surface
    uint base_vertex;    // where this draw's buffer starts, in canonical vertices
    uint pad0;
};

// GpuFrameData::map_flags - must match the bits VkDraw.cpp sets.
static const uint kMapLightingOn = 0x1u;
static const uint kMapLightingAll = 0x2u;
static const uint kMapLightingGrid = 0x4u;
// GpuFrameData::light_flags - likewise. `render.local_lights`, off, drops D3D's point and spot
// lights from the sum and leaves the directionals, so the difference image is exactly the pixels
// those lights reach. That is the CEILING on what a shadow from them could ever change, which is
// the measurement phase 5 has to take before it designs anything (§4.65).
static const uint kLocalLightsOn = 0x1u;
// Which of the static atlas's two tenants may sample it. `map_shadow_texture` says only that the
// image exists - the map lights (§4.61) and D3D's own point lights (§4.65) are separate features
// with separate knobs, so each asks about itself.
static const uint kMapShadowsOn = 0x2u;
static const uint kLocalShadowsOn = 0x4u;
// `render.local_light_window`, on by default. Off restores D3D8's hard cutoff at Range, so the
// windowed tail can be A/B'd inside ONE PAUSED FRAME rather than across two launches - which is
// the only way to read this change, because §4.30 measured that two settles of level02 drift by
// the objectives text's fade and the units' animation phase and nothing separates that from a
// renderer difference in a whole-frame number.
static const uint kLocalLightWindowOn = 0x8u;
// GpuFrameData::ao_flags (§4.86). `kAoMapOnly` restricts the term to the map's own geometry, for
// the reason the map-lighting substitution is restricted the same way: a prop or a unit is a
// separate RBOBJECT whose vertex colours were BAKED from its own file's lights, and that bake
// already contains occlusion - so applying this on top of it darkens the same crease twice.
static const uint kAoMapOnly = 0x1u;
// Replaces the shaded colour with the occlusion term itself, as grey. The only way to look at
// what the pass produced rather than at what it did to a frame, which is what the radius and tap
// count have to be tuned against.
static const uint kAoDebug = 0x2u;

[[vk::push_constant]]
ConstantBuffer<Push> pc;

// The bindless set, exactly as VkResources.cpp builds it.
[[vk::binding(0, 0)]] SamplerState samplers[];
[[vk::binding(1, 0)]] Texture2D textures[];

static const uint kNoTexture = 0xffffffffu;

// GpuDrawRecord::lighting - must match VkDraw.h.
static const uint kLightSum = 0x01u;
static const uint kLitCollapse = 0x02u;
static const uint kNormaliseNormals = 0x04u;
static const uint kSpecularEnable = 0x08u;
static const uint kVertexColour = 0x10u;
static const uint kColourVertex = 0x20u;

// GpuFrameData::colour_flags - must match src/VkDraw.h.
//
// `kLinearInput` is `render.linear_input`, and after §4.97 it means exactly one thing: **the
// fragment's final colour is sRGB-decoded on the way out**, at the bottom of `fragment_main`.
// Everything above that point - the light sum, the material terms, the whole texture cascade -
// runs on the values Gunlok authored, in the domain it authored them in, untouched.
//
// It got there by three play reports. Decoding the albedos on the way IN crushed the chrome pass,
// because `ADDSIGNED` is a biased sum and a sum does not survive a decode (§4.96). Decoding the
// light colours flattened every light's falloff, because the attenuation constants shape a
// display-referred gradient and applying them in linear then encoding raises their ratio to 1/2.2
// (§4.97). The pattern is that this game's shading was balanced against a display-referred
// renderer and **no partial decode of it preserves what was balanced**.
//
// What the decode is FOR is downstream of the shading: the framebuffer blend runs on light, so
// additive fires and flares accumulate past 1 instead of clipping, and the tonemap gets a real
// over-range. An opaque draw is bit-exact against stock, since the tonemap re-encodes with the
// inverse curve.
//
// Alpha is never decoded - it is coverage, not light - which also leaves the alpha test's 0..255
// comparison alone. The **2D layers never take this path at all**: they are drawn after the
// tonemap and their frame block has `colour_flags` clear (§4.92, §4.95).
static const uint kLinearInput = 0x1u;
// `render.hdr` itself. The clamps that reproduce the fixed function's D3DCOLOR output stage come
// off, so an over-range light survives to the tonemap instead of dying at the last multiply. It is
// a separate bit rather than the same one because it is inert without a float target, where
// kLinearInput is not.
static const uint kUnclamped = 0x2u;

// The exact piecewise sRGB curve, and the inverse of `srgb_encode` in tonemap.slang - the two have
// to agree exactly or the 2D half of the game does not survive the round trip. See the note there
// for why the round trip is exact at 8 bits through an fp16 buffer.
float3 srgb_decode(float3 encoded) {
    const float3 c = max(encoded, float3(0, 0, 0));
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

// The fixed function's output stage, which clamps to a D3DCOLOR. Under `render.hdr` there is a
// float target to clamp into instead of an 8-bit one, so it does not - and that is where the
// over-range the tonemap compresses comes from. Still clamps at zero: a negative colour is not a
// quantity the fixed function can produce and would survive into the blend as one.
float3 clamp_output(float3 c) {
    return (pc.frame[0].colour_flags & kUnclamped) != 0u ? max(c, float3(0, 0, 0)) : saturate(c);
}

// A D3DCOLOR - 0xAARRGGBB in a uint - as a float4.
float4 unpack_colour(uint packed) {
    return float4(float((packed >> 16) & 0xffu), float((packed >> 8) & 0xffu),
                  float(packed & 0xffu), float((packed >> 24) & 0xffu)) / 255.0;
}

// D3DRS_SHADEMODE is a *rasteriser* state in D3D and a varying qualifier in Vulkan, so the two
// colours are carried twice - once interpolated, once flat - and the fragment shader picks. That
// keeps SHADEMODE off the pipeline key: making it pipeline state would double the pipeline count
// across every blend/depth/cull combination to serve 2% of the draws (§4.31).
//
// **The provoking vertex already agrees.** D3D flat-shades a primitive with the colour of its
// FIRST vertex, and Vulkan's default convention is first-vertex too - so `nointerpolation` needs
// no `VK_EXT_provoking_vertex` and no state to go with it. Had the two disagreed, a strip would
// have taken its colour from the wrong end and looked like an off-by-one in the geometry.
//
// Only the two colours: D3D flat shading does not affect texture coordinates, and `uv` stays
// interpolated.
struct VertexOut {
    float4 position : SV_Position;
    float4 color : COLOR;
    float4 specular : COLOR1; // D3D's second output colour, added after the texture stages
    nointerpolation float4 color_flat : COLOR2;
    nointerpolation float4 specular_flat : COLOR3;
    float4 uv : TEXCOORD0;    // xy = uv0, zw = uv1 - uv1 addresses the fog grid (notes §4.51)
    // The two the fragment shader needs to run D3D's light sum itself: the lighting map's bumped
    // normal (§4.48) and, since Phase 1, the whole per-pixel path. Zero on a draw that was not
    // lit, where the fragment shader does not read them - a pre-transformed vertex has no world
    // position to carry.
    //
    // `world_normal` is deliberately NOT normalised here: D3D normalises only under
    // D3DRS_NORMALIZENORMALS and the resulting length is a real difference in brightness, so the
    // vertex path reproduces that. The per-pixel path normalises after interpolation instead,
    // because interpolation changes the length whatever D3D would have done.
    float3 world_position : TEXCOORD1;
    float3 world_normal : TEXCOORD2;
};

// Four float4 rows back into the matrix they are. Rows, because everything on this side is
// D3D's row-vector convention: `mul(v, M)` is v * M.
float4x4 rows_to_matrix(float4 r0, float4 r1, float4 r2, float4 r3) {
    return float4x4(r0, r1, r2, r3);
}

// --- fixed-function lighting ------------------------------------------------------------------
//
// D3D8's per-vertex equation, reproduced rather than approximated (§4.26):
//
//     colour.rgb = Ce + Ca * (global_ambient + SUM k*L.ambient)
//                     + Cd * SUM k*L.diffuse*max(0, N.L)
//     colour.a   = Cd.a
//     specular   = Cs * SUM over lights with N.L > 0 of k*L.specular*pow(max(0, N.H), power)
//
// with k = attenuation * spot per light. **The N.L > 0 condition on the specular sum is not a
// restatement of max(0, N.H)** - the two differ over a wide band of orientations, and dropping
// it wraps a highlight right around onto the faces turned away from a light (§4.46). The C* are the material colours, each of which the
// game can redirect to a vertex colour through D3DRS_*MATERIALSOURCE - which is exactly the
// state that made this look unnecessary for so long: with DIFFUSEMATERIALSOURCE = D3DMCS_COLOR1
// a vertex diffuse is the *material* diffuse, not the final colour, and using it as the final
// colour is D3D's lighting-DISABLED result. That is what the renderer did, and it reproduced
// lighting-off d3d9 to 0.5/255 while being 6.54 off the lit game (§4.25).
//
// Lighting is computed in WORLD space, which is where D3D8 puts a light: SetLight's position
// and direction are world-space, so the vertex comes to them rather than the other way round.

// Which colour a D3DRS_*MATERIALSOURCE selects. D3DMCS_COLOR2 falls back to the material
// because the canonical vertex drops the specular colour (§4.10); the CPU counts every draw
// that asks, so this is an approximation with a number on it rather than a silent one.
float4 material_source(uint lighting, uint shift, float4 material, float4 vertex_colour) {
    const uint source = (lighting >> shift) & 3u;
    const bool have_vertex = (lighting & kColourVertex) != 0u && (lighting & kVertexColour) != 0u;
    return (source == 1u && have_vertex) ? vertex_colour : material;
}

// The six cube faces of the map lights' static shadow atlas. **This table is duplicated in
// src/VkDraw.cpp and the two must agree exactly** - the bake rasterises with it, this projects
// with it, and a disagreement puts a light's shadow on the wrong face rather than producing
// anything that looks like a bug.
static const float3 kFaceForward[6] = {float3(1, 0, 0),  float3(-1, 0, 0), float3(0, 1, 0),
                                       float3(0, -1, 0), float3(0, 0, 1),  float3(0, 0, -1)};
static const float3 kFaceRight[6] = {float3(0, 0, -1), float3(0, 0, 1), float3(1, 0, 0),
                                     float3(1, 0, 0),  float3(1, 0, 0), float3(-1, 0, 0)};
static const float3 kFaceUp[6] = {float3(0, 1, 0),  float3(0, 1, 0), float3(0, 0, -1),
                                  float3(0, 0, 1),  float3(0, 1, 0), float3(0, 1, 0)};
// kMapShadowFace / kMapShadowAtlas / kMapShadowNear in src/VkDraw.h and .cpp. Constants here
// rather than frame data because they are compile-time on that side too, and a value that cannot
// change has no business costing a load per fragment per light.
static const float kMapShadowFace = 64.0;
static const float kMapShadowTilesPerRow = 64.0;
static const float kMapShadowNear = 1.0 / 64.0;
// The per-frame atlas (§4.66). kDynShadowFace / kDynShadowAtlas in src/VkDraw.h - **256-texel
// faces where the static atlas has 64**, because there are 42 slots here against 682 and the
// blockiness is what a bigger face buys. The 4096² atlas is what keeps all 42 at that size
// (§4.69); at 2048² a 256-texel face would leave ten.
static const float kDynShadowFace = 256.0;
static const float kDynShadowTilesPerRow = 16.0;

// One cube in one atlas, as the lookup needs to see it. Passing these rather than reading them
// from `frame` is what lets the static and the per-frame atlas share the body below: they differ
// only in the image, the face size and the bias, and a second copy of that projection is a second
// place for it to disagree with the bake.
struct ShadowAtlas {
    uint texture;
    uint sampler_index;
    float face;          // texels across one cube face
    float tiles_per_row;
    float offset;        // the normal offset, in texels at the fragment's own distance
    // The PCF kernel's radius in texels: 0 is a single tap, 1 is 3x3, 2 is 5x5. **Not a property
    // of the image** - the same atlas is read once per fragment by a D3D point light, which needs
    // filtering, and eleven times by the map lights, which do not (§4.69). So the builders below
    // leave it at the caller's default and each call site sets what it wants.
    float radius;
};

// The two atlases, each built from the frame block once at the point of use. `kNoTexture` in
// `texture` is what every caller tests, and it means "off", "could not be created" and "nothing in
// it this frame" alike.
// `radius` 0 - a single tap - is the default both builders return, because it is what the map
// lights want and they are by far the heaviest reader. A D3D light's call site widens it.
ShadowAtlas static_atlas() {
    ShadowAtlas out;
    out.texture = pc.frame[0].map_shadow_texture;
    out.sampler_index = pc.frame[0].map_shadow_sampler;
    out.face = kMapShadowFace;
    out.tiles_per_row = kMapShadowTilesPerRow;
    out.offset = pc.frame[0].map_shadow_offset;
    out.radius = 0.0;
    return out;
}

ShadowAtlas dynamic_atlas() {
    ShadowAtlas out;
    out.texture = pc.frame[0].dyn_shadow_texture;
    out.sampler_index = pc.frame[0].dyn_shadow_sampler;
    out.face = kDynShadowFace;
    out.tiles_per_row = kDynShadowTilesPerRow;
    out.offset = pc.frame[0].dyn_shadow_offset;
    out.radius = 0.0;
    return out;
}

// How lit `world_position` is by one light with a cube in the static atlas, 0 to 1. `slot` is
// which cube.
//
// **Shared by the level's own `STDLIGHT` rig and by D3D's point and spot lights** (§4.61, §4.65).
// The two are completely different light systems - one is authoring data the shipped engine never
// read, the other is what the game sets on the device this frame - but a cube is a cube, and
// having one lookup is what keeps them from projecting against subtly different frusta. The CPU
// side has the same property: one `BuildCubeFaceMatrix` bakes both.
//
// **The bias is a normal offset, not a depth one** (notes §4.61). A 64-texel cube face is coarse -
// a texel is `distance / 32` world units - so the depth error a flat surface accumulates across
// one is dominated by its slope against the light, and moving the lookup along the normal by a
// texel or two cancels exactly that. A depth offset large enough to do the same job here would
// detach every shadow from its caster by metres.
float cube_shadow_visibility(float3 world_position, float3 normal, float3 light_position,
                             float light_range, uint slot, ShadowAtlas atlas) {
    const float range = max(light_range, 1e-3);
    float3 delta = world_position - light_position;
    // The offset is in texels *at this distance*, and a face is 90 degrees across `atlas.face` of
    // them, so one texel spans `2 * distance / atlas.face` world units.
    const float texel = 2.0 * length(delta) / atlas.face;
    delta += normal * (atlas.offset * texel);

    const float3 a = abs(delta);
    uint face;
    if (a.x >= a.y && a.x >= a.z) {
        face = delta.x >= 0.0 ? 0u : 1u;
    } else if (a.y >= a.z) {
        face = delta.y >= 0.0 ? 2u : 3u;
    } else {
        face = delta.z >= 0.0 ? 4u : 5u;
    }
    const float z = dot(delta, kFaceForward[face]);
    const float near_plane = range * kMapShadowNear;
    if (z <= near_plane || z >= range) {
        // Nearer than the near plane is geometry the bake clipped, and past the far plane is out
        // of this light's reach entirely - the caller has already rejected the latter by range,
        // so this is the near case and it must read LIT. Shadowing it would put a dark halo on
        // everything close enough to a light to matter most.
        return 1.0;
    }
    // Exactly what the bake's matrix produces: a 90-degree left-handed perspective, so x/z and
    // y/z land in -1..1 across one face, and the depth is D3D's z/w.
    const float2 local = float2(dot(delta, kFaceRight[face]), dot(delta, kFaceUp[face])) / z;
    const float scale = range / (range - near_plane);
    const float reference = scale * (1.0 - near_plane / z);

    const float2 tile = float2(float(slot * 6u + face) % atlas.tiles_per_row,
                               floor(float(slot * 6u + face) / atlas.tiles_per_row));
    const float2 centre = local * 0.5 + 0.5;
    // **`radius` taps a side, and it is the CALLER's** (§4.69). One tap is right for the map
    // lights and wrong for D3D's: this body is shared, and the "one tap, not nine" it used to hard-
    // code is a measurement about the STDLIGHT rig - a fragment on level02 is in range of a mean of
    // 11.5 of them (§4.54), so the averaging PCF would buy is already in the sum, and nine taps a
    // light there would be a hundred a fragment. A point light from an explosion has NOTHING
    // averaging it: one or two reach a fragment, so the hard 0/1 compare is the visible edge, and
    // it is what play reported as jagged.
    //
    // Half a texel in from the tile's own edge, clamped per tap rather than once, for §4.59's
    // reason one atlas up: a tap at the boundary reads the neighbouring tile, which here is a
    // different light's cube face rather than merely a different scale.
    const float2 lo = float2(0.5, 0.5) / atlas.face;
    const float2 hi = float2(1.0, 1.0) - lo;
    const int r = int(atlas.radius);
    float lit = 0.0;
    float taps = 0.0;
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            const float2 uv = clamp(centre + float2(float(x), float(y)) / atlas.face, lo, hi);
            // SampleLevel, not Sample: this runs under a branch on the material, where an
            // implicit-derivative fetch is undefined - the same rule sun_visibility follows.
            const float depth = textures[atlas.texture].SampleLevel(
                samplers[atlas.sampler_index], (tile + uv) / atlas.tiles_per_row, 0.0).r;
            lit += reference <= depth ? 1.0 : 0.0;
            taps += 1.0;
        }
    }
    return lit / taps;
}

// One light's geometry: the direction to it and the attenuation-times-spot factor D3D calls k,
// or `valid = false` for a light this vertex is out of range of or outside the cone of.
//
// Factored out because the FRAGMENT shader runs the same loop for a lighting map, and the two
// diverging silently is exactly the class of defect §4.46 was: a highlight computed under a
// slightly different condition from the diffuse it sits on looks like an authoring problem in the
// map rather than like a shader that disagrees with itself.
struct LightGeometry {
    float3 to_light;
    float k;
    // How much of this light reaches the surface, from the static atlas (§4.65). **Separate from
    // `k` rather than folded into it**, because the ambient term must never be shadowed - ambient
    // is by definition light that did not arrive along the ray, so attenuating it makes a
    // shadowed surface darker than an unlit one, which is how a first shadow implementation ends
    // up with holes in it (§4.58). So: ambient uses `k`, the direct terms use `k * shadow`.
    float shadow;
    bool valid;
};

// `normal` is here **only** for the shadow lookup, whose bias is an offset along it. Nothing else
// in this function uses it, and passing it is what lets the fixed-function sum and the lighting
// map's response share one visibility rather than compute two that can disagree.
LightGeometry light_geometry(GpuLight light, float3 world_position, float3 normal) {
    LightGeometry out;
    out.to_light = float3(0, 0, 1);
    out.k = 1.0;
    out.shadow = 1.0;
    out.valid = true;

    const float type = light.spot.w;
    // `render.local_lights = false` - a diagnostic, and the one that prices phase 5 (§4.65). It
    // drops every point and spot light and keeps the directionals, so a paused A/B paints exactly
    // the pixels D3D's local lights reach. A shadow can only ever REMOVE light, so that set
    // strictly contains anything shadowing them could change: it is the ceiling, measured before
    // anything is built.
    //
    // Here rather than in either loop for the reason this function exists at all: the fixed
    // function's sum and the lighting map's response both run it, and a diagnostic that reached
    // one and not the other would report a ceiling that is not one.
    if (type < 2.5 && (pc.frame[0].light_flags & kLocalLightsOn) == 0u) {
        out.valid = false;
        return out;
    }
    if (type >= 2.5) {
        // Directional: no position, no attenuation. The CPU has normalised the direction.
        out.to_light = -light.direction.xyz;
        return out;
    }
    const float3 delta = light.position.xyz - world_position;
    const float distance = length(delta);
    if (distance > light.attenuation.w) {
        out.valid = false; // outside Range, which switches the light off entirely
        return out;
    }
    out.to_light = distance > 0.0 ? delta / distance : float3(0, 0, 1);
    const float denominator = light.attenuation.x + light.attenuation.y * distance +
                              light.attenuation.z * distance * distance;
    out.k = denominator > 0.0 ? 1.0 / denominator : 1.0;
    // **The range cutoff, windowed** - §4.64's defect in the path §4.64 did not cover.
    //
    // D3D8 switches a light off HARD at Range while `1/(a0 + a1 d + a2 d^2)` is still well above
    // zero there. Measured off `render.frame_lights` on level02, the two fires are range 6,
    // attenuation 0.959/0.0333/0.16666 and diffuse 4.0, so k at the boundary is **0.140** and the
    // term it scales is 0.56 before N.L - cut to nothing across one pixel. That is a step in the
    // VALUE, where the map lights' rim was only a kink in its slope.
    //
    // It is faithful to the original and was invisible there for §4.64's reason: D3D8 evaluated
    // this per VERTEX, so the step landed inside a triangle and interpolation destroyed it. Moving
    // the sum per pixel (§4.55) is what exposed it, exactly as it exposed the other one.
    //
    // **A window, not a replacement.** The map lights multiply by `(1 - t)(1 - t^4)` because there
    // the linear `(1 - t)` *is* the fitted attenuation; here the attenuation is D3D's own curve and
    // has to survive, so this only pulls its tail down. `(1 - t^4)^2` is zero in both value and
    // first derivative at t = 1 - §4.64's criterion - and is within 1% of 1.0 out to a quarter of
    // the range, so the near field, where the quadratic dominates anyway, is untouched.
    //
    // There is no bake to refit against: these are D3D lights, not `SHPVTINT`, so the only
    // instrument is an A/B against `GKPLUS_RENDERER=d3d8` and the residual is expected to RISE.
    if ((pc.frame[0].light_flags & kLocalLightWindowOn) != 0u) {
        const float t = distance / max(light.attenuation.w, 1e-6);
        const float window = saturate(1.0 - t * t * t * t);
        out.k *= window * window;
    }
    if (type >= 1.5) {
        // Spot. rho is measured along the cone axis, so it needs the direction from the light to
        // the vertex - the opposite of the vector the diffuse term wants.
        const float rho = dot(light.direction.xyz, -out.to_light);
        const float cos_theta = light.spot.x;
        const float cos_phi = light.spot.y;
        if (rho <= cos_phi) {
            out.valid = false;
            return out;
        }
        if (rho < cos_theta) {
            out.k *= pow(saturate((rho - cos_phi) / max(cos_theta - cos_phi, 1e-6)),
                         light.spot.z);
        }
    }
    // **The shadow, last** - after the range test and the cone test have each already returned,
    // exactly as the map lights' lookup sits behind theirs (§4.61). A light this fragment cannot
    // see costs no texture fetch at all.
    //
    // **Two atlases, and the per-frame one wins.** `position.w` is this light's cube in the
    // per-frame atlas (§4.66) and `direction.w` its cube in the static one (§4.65), each -1 for a
    // light that has none. The per-frame cube is strictly the better answer where it exists - it
    // was rasterised this frame from this frame's casters, so it holds the units and props the
    // static one cannot and it holds a light that moved - so the static slot is a fallback for a
    // light the 42 per-frame slots had no room for, and only that.
    // **Both D3D paths filter** (§4.69). One or two of these reach a fragment, so nothing averages
    // the 0/1 compare the way the map lights' sum does, and a single tap is a stair-stepped edge.
    // The radius is a knob (`render.local_shadow_taps`) because it is the one thing here that
    // trades frame time for the artefact play reported.
    const float radius = float(pc.frame[0].local_shadow_taps);
    const int dynamic_slot = int(light.position.w);
    ShadowAtlas dyn = dynamic_atlas();
    dyn.radius = radius;
    if (dynamic_slot >= 0 && dyn.texture != kNoTexture) {
        out.shadow = cube_shadow_visibility(world_position, normal, light.position.xyz,
                                            light.attenuation.w, uint(dynamic_slot), dyn);
        return out;
    }
    const int slot = int(light.direction.w);
    ShadowAtlas fixed = static_atlas();
    fixed.radius = radius;
    if (slot >= 0 && (pc.frame[0].light_flags & kLocalShadowsOn) != 0u &&
        fixed.texture != kNoTexture) {
        out.shadow = cube_shadow_visibility(world_position, normal, light.position.xyz,
                                            light.attenuation.w, uint(slot), fixed);
    }
    return out;
}

// The three sums D3D8's equation accumulates over the enabled lights, before any material colour
// is applied. Splitting the loop from the material resolution is what lets the SAME code run per
// vertex and per pixel: the loop needs a position and a normal, the resolution needs a vertex
// colour, and only the first of those changes between the two.
struct LightSums {
    float3 ambient;
    float3 diffuse;
    float3 specular;
};

// The loop, verbatim from what `shade_vertex` used to do inline. `to_eye` is the caller's because
// the two call sites disagree about where it comes from: per vertex it is computed once from the
// vertex's world position, per pixel from the fragment's.
//
// `visibility` is the sun's shadow term, and it is applied HERE rather than to the finished sums
// because it belongs to **one** light. It multiplies a DIRECTIONAL light's contribution and
// nothing else: the map describes what occludes the sun, and a point light three metres away in
// the same room is not occluded by the roof between the room and the sky.
//
// It is worth only 0.12 MAD on level02's covered start, whose visible lighting turns out to be the
// directional rather than its two fires (§4.59) - and that smallness is the measurement, because
// it says the level going dark there is the sun being correctly occluded and not a point light
// being wrongly shadowed.
//
// A level with two directional lights would have both shadowed by the one map. None is known;
// every draw measured so far carries a single white directional plus point lights.
LightSums light_sum(GpuDrawRecord draw, float3 world_position, float3 normal, bool want_specular,
                    float3 to_eye, float power, float visibility) {
    LightSums out;
    out.ambient = draw.global_ambient.rgb;
    out.diffuse = float3(0, 0, 0);
    out.specular = float3(0, 0, 0);

    for (uint i = 0u; i < draw.light_count; ++i) {
        const GpuLight light = pc.lights[draw.light_offset + i];
        const LightGeometry geometry = light_geometry(light, world_position, normal);
        if (!geometry.valid) {
            continue;
        }
        const float3 to_light = geometry.to_light;
        // **The ambient term is never shadowed**, whatever the light. Ambient is by definition
        // light that did not arrive along the sun's ray, so attenuating it makes a shadowed
        // surface darker than an unlit one - the classic way a first shadow implementation ends
        // up with black holes rather than shade.
        //
        // Two visibilities, one per light system, and each reaches only the light it describes:
        // `visibility` is the sun's cascades and applies to a DIRECTIONAL light alone, and
        // `geometry.shadow` is the static atlas and applies to whichever point or spot light has
        // a cube in it (§4.65). A light with neither carries 1.0 in both.
        const float k =
            geometry.k * (light.spot.w >= 2.5 ? visibility : 1.0) * geometry.shadow;

        const float lambert = max(0.0, dot(normal, to_light));
        out.ambient += geometry.k * light.ambient.rgb;
        out.diffuse += k * light.diffuse.rgb * lambert;
        // **Gated on the diffuse term, not just on N.H.** A light behind the surface contributes
        // no highlight in D3D, and `max(0, N.H)` alone does not say so: N.H stays positive well
        // past the point where N.L goes negative, so an ungated term paints a specular wash on
        // every face turned AWAY from a light (notes §4.46). On level02 that was the near face
        // of the concrete ledge and the ground in front of it, lit orange by two fires that
        // are behind them - the whole of "the ledge is much redder in Vulkan".
        if (want_specular && lambert > 0.0) {
            const float3 halfway = normalize(to_light + to_eye);
            out.specular +=
                k * light.specular.rgb * pow(max(0.0, dot(normal, halfway)), max(power, 1e-4));
        }
    }
    return out;
}

// The sums, plus this surface's material colours, into the two colours the texture stages consume.
// Shared for the same reason the loop is: D3DRS_*MATERIALSOURCE has to be resolved identically
// wherever the sum ran, and it is the half that needs the vertex colour.
struct LitColour {
    float4 color;
    float3 specular;
};

LitColour resolve_lit_colour(GpuDrawRecord draw, LightSums sums, float4 vertex_colour,
                             bool want_specular) {
    const float4 ambient =
        material_source(draw.lighting, 10u /* kAmbientSourceShift */, draw.material_ambient,
                        vertex_colour);
    const float4 diffuse =
        material_source(draw.lighting, 8u /* kDiffuseSourceShift */, draw.material_diffuse,
                        vertex_colour);
    const float4 emissive =
        material_source(draw.lighting, 14u /* kEmissiveSourceShift */, draw.material_emissive,
                        vertex_colour);
    const float4 specular =
        material_source(draw.lighting, 12u /* kSpecularSourceShift */, draw.material_specular,
                        vertex_colour);

    LitColour out;
    // Saturated because the fixed function's output is a D3DCOLOR: the clamp is where the
    // pipeline is, not an approximation of it.
    //
    // ... which is exactly why `render.hdr` lifts it. This is the line that discards three
    // quarters of level02's key light - notes 4.48 measured it at `diffuse 4.0` - and with a float
    // target underneath there is somewhere for the rest to go. `clamp_output` is the identity
    // under kUnclamped and `saturate` otherwise, so the stock path is unchanged by construction
    // rather than by inspection.
    out.color = float4(clamp_output(emissive.rgb + ambient.rgb * sums.ambient +
                                    diffuse.rgb * sums.diffuse),
                       diffuse.a);
    out.specular = want_specular ? clamp_output(specular.rgb * sums.specular) : float3(0, 0, 0);
    return out;
}

// The sampling pattern both of PCSS's loops walk, unit radius.
//
// **The same construction ao.slang uses, and for the same two reasons** (notes 4.86): one fixed
// pattern shared by every pixel is the friendliest case there is for the texture cache, and - far
// more important here - it produces no noise, so nothing has to blur the result. A per-pixel
// rotation is what textbook PCSS does, and it trades this pattern's banding for grain that only a
// blur removes; there is no blur pass on the shadow term, and adding one over a 640x480 frame
// would halo every silhouette exactly as 4.86 says it would.
//
// Duplicated rather than shared because a `.slang` here is standalone - the build compiles each
// file on its own, with no include and no module (gen-shaders.py). This is ao.slang's 32-entry
// maximin prefix verbatim, so a shorter `shadow_soft_taps` is still a well-spread subset of it
// rather than a corner of the plot.
static const uint kShadowDiscSize = 32u;
static const float2 kShadowDisc[kShadowDiscSize] = {
    float2(+0.000000, +0.000000), float2(-0.700000, -0.675000),
    float2(+0.000000, +0.950000), float2(+0.950000, +0.000000),
    float2(-0.800000, +0.425000), float2(+0.500000, -0.750000),
    float2(+0.425000, +0.425000), float2(+0.000000, -0.925000),
    float2(-0.475000, -0.025000), float2(+0.500000, -0.012500),
    float2(-0.925000, +0.000000), float2(+0.000000, +0.500000),
    float2(-0.012500, -0.500000), float2(-0.400000, +0.425000),
    float2(-0.425000, -0.450000), float2(+0.750000, -0.500000),
    float2(+0.425000, -0.350000), float2(+0.500000, +0.762500),
    float2(-0.250000, +0.750000), float2(+0.750000, +0.250000),
    float2(-0.600000, +0.675000), float2(-0.750000, -0.250000),
    float2(-0.250000, -0.750000), float2(-0.250000, -0.250000),
    float2(+0.725000, +0.500000), float2(+0.250000, +0.750000),
    float2(+0.750000, -0.250000), float2(+0.250000, -0.750000),
    float2(+0.250000, +0.250000), float2(+0.250000, -0.200000),
    float2(-0.250000, +0.250000), float2(-0.750000, +0.050000),
};

// How lit this fragment is by the sun, 0 fully shadowed to 1 fully lit.
//
// Three paths now, and the first is a lookup rather than a filter. `shadowmask.slang` resolves the
// whole term into a screen-space mask with a rotated kernel and blurs it; where that mask covers
// the surface being shaded, this reads it and is done. Where it does not - the mask is off, the
// pass could not be built, or this pixel is a different surface from the one the prepass saw -
// the filter runs here as it always did: a 3x3 PCF when `shadow_softness` is 0, PCSS over one
// fixed lattice when it is not.
//
// Manual comparison rather than a comparison sampler in both, because the bindless set's sampler
// array is `SamplerState` and adding a second array of `SamplerComparisonState` would mean a
// third binding on a set whose last binding is pinned by VARIABLE_DESCRIPTOR_COUNT - for taps
// that cost the same either way. PCSS could not use one for its first loop regardless: a blocker
// search needs the depth that is stored, not the verdict of a comparison against it.
// How far off this fragment's own position the prepass's may be and still be the same surface, as
// a fraction of the distance to the eye. **Relative and not absolute**, because the two positions
// are the same arithmetic in a different pass and differ only by rasterisation precision, which
// scales with magnitude - an absolute epsilon would be slack up close and would reject the far
// half of every level.
//
// What it is really rejecting is not precision but *a different surface*: a transparent draw over
// the ground, a fragment MSAA-resolved from a triangle the prepass's single sample did not see, a
// draw the prepass never rasterised at all. Each of those falls back on the inline filter below,
// which is what makes the mask a substitution rather than a replacement - nothing loses its shadow
// because a screen-space pass could not see it.
static const float kMaskMatch = 0.01;

float sun_visibility(float3 world_position, int2 pixel, float3 eye) {
    const GpuFrameData frame = pc.frame[0];
    if (frame.shadow_texture == kNoTexture || frame.shadow_cascades == 0u) {
        return 1.0;
    }
    if (frame.shadow_mask_texture != kNoTexture) {
        const float4 seen = textures[frame.shadow_mask_position].Load(int3(pixel, 0));
        const float3 delta = seen.xyz - world_position;
        const float tolerance = kMaskMatch * max(length(world_position - eye), 1.0);
        if (seen.w != 0.0 && dot(delta, delta) <= tolerance * tolerance) {
            // **Raw visibility**: `shadow_strength` is applied here and not in the mask, so the
            // knob costs no re-resolve and there is one definition of what it means.
            return lerp(1.0, textures[frame.shadow_mask_texture].Load(int3(pixel, 0)).r,
                        saturate(frame.shadow_strength));
        }
    }
    // ONE transform, however many cascades: `sun_matrix` is the sun's basis with no scale, so this
    // is the fragment in light space in world units, and a cascade is a centre and a half-extent
    // in it. Four matrix multiplies would give the same answer and cost four times as much.
    const float3 ls = mul(float4(world_position, 1.0),
                          float4x4(frame.sun_matrix[0], frame.sun_matrix[1],
                                   frame.sun_matrix[2], frame.sun_matrix[3])).xyz;
    const float depth = (ls.z - frame.shadow_z_near) / frame.shadow_z_span;
    // Outside the box the map covers is LIT, not shadowed. The alternative - clamping into the
    // edge texel - smears whatever is at the boundary across the rest of the level, which reads
    // as a huge shadow that follows the camera.
    if (depth < 0.0 || depth > 1.0) {
        return 1.0;
    }

    // The FIRST cascade that contains this fragment, which is the smallest one because they are
    // ordered inward-out - so the sharpest map that covers a fragment is the one it uses, and the
    // selection needs no view-space depth. 4.56's warning applies here too: the view matrix on
    // this side is per DRAW, so anything derived from it is a quiet source of error.
    uint cascade = frame.shadow_cascades;
    float2 local = float2(0.0, 0.0);
    for (uint i = 0u; i < frame.shadow_cascades; ++i) {
        const float2 p = (ls.xy - frame.cascades[i].xy) * frame.cascades[i].z;
        if (all(abs(p) <= 1.0)) {
            cascade = i;
            local = p * 0.5 + 0.5;
            break;
        }
    }
    if (cascade >= frame.shadow_cascades) {
        return 1.0; // past the outermost box, which is where shadows stop
    }
    // The bias is per cascade because the knob is in TEXELS and a texel is a different world
    // distance in each one - which is the whole reason one number works across all four.
    const float reference = depth - frame.cascades[cascade].w;
    const float2 tile = float2(float(cascade & 1u), float(cascade >> 1));
    // Half a texel in from the tile's own edge. A PCF tap at the boundary would otherwise read
    // the neighbouring cascade's depth, which is a different scale entirely and paints a hard
    // line along the seam.
    const float2 lo = float2(0.5, 0.5) * frame.shadow_texel;
    const float2 hi = float2(1.0, 1.0) - lo;

    float lit = 0.0;
    if (frame.shadow_softness <= 0.0) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                const float2 tap =
                    clamp(local + float2(float(x), float(y)) * frame.shadow_texel, lo, hi);
                // SampleLevel, not Sample: this runs under a branch on the material, where an
                // implicit-derivative fetch is undefined - the same rule shade_lighting_map
                // follows.
                const float d =
                    textures[frame.shadow_texture].SampleLevel(samplers[frame.shadow_sampler],
                                                               (tap + tile) * 0.5, 0.0).r;
                lit += reference <= d ? 1.0 : 0.0;
            }
        }
        lit /= 9.0;
    } else {
        // --- PCSS -----------------------------------------------------------------------------
        //
        // The penumbra is worked out in WORLD units and converted to texels last, which is what
        // carries it across a cascade boundary: the same blocker at the same distance produces the
        // same penumbra on the ground whichever map answered for it, even though that is eight
        // texels in cascade 0 and one in cascade 3. A radius quoted in texels would paint a
        // visible ring where two cascades meet, in every frame, for nothing.
        //
        // A cascade's texel is `2 * extent / tile` world units, `cascades[i].z` is `1 / extent`
        // and `shadow_texel` is `1 / tile`, so that is this product and no new field is needed.
        const float world_texel = 2.0 * frame.shadow_texel / frame.cascades[cascade].z;
        const uint taps = min(frame.shadow_soft_taps, kShadowDiscSize);
        const float ceiling = max(frame.shadow_soft_max, frame.shadow_soft_min);

        // **The search radius is the ceiling**, which is an argument rather than a knob nobody
        // wrote: a blocker found beyond it could only ask for a penumbra the ceiling clamps back
        // to the ceiling, so searching further cannot change the answer - it can only cost taps
        // and drag in occluders the filter will never reach.
        float blocker_sum = 0.0;
        float blocker_count = 0.0;
        for (uint i = 0u; i < taps; ++i) {
            const float2 tap =
                clamp(local + kShadowDisc[i] * ceiling * frame.shadow_texel, lo, hi);
            const float d =
                textures[frame.shadow_texture].SampleLevel(samplers[frame.shadow_sampler],
                                                           (tap + tile) * 0.5, 0.0).r;
            // Against the SAME biased reference the filter compares with, so a surface that is not
            // its own blocker there is not one here either - otherwise every lit slope would find
            // a blocker about a bias away and soften itself for no reason.
            if (d < reference) {
                blocker_sum += d;
                blocker_count += 1.0;
            }
        }
        // Nothing between this fragment and the sun anywhere in the search: fully lit, and the
        // second loop never runs. That is the common case over an open frame, and it is what keeps
        // the average cost well below two full kernels.
        if (blocker_count < 0.5) {
            return 1.0;
        }

        // Similar triangles, in the sun's own space: the light is a disc of angular radius
        // `atan(softness)`, so a blocker `distance` away casts a penumbra of `softness * distance`
        // in this fragment's plane. `shadow_z_span` is what turns the map's 0..1 depth back into
        // the world units both the softness and the texel are quoted in.
        const float distance = max(depth - blocker_sum / blocker_count, 0.0) * frame.shadow_z_span;
        const float radius =
            clamp(frame.shadow_softness * distance / world_texel, frame.shadow_soft_min, ceiling);

        // **The bias does not scale with the radius, and that is not an oversight.** The usual
        // reason a wide PCF wants more bias is that it compares distant texels of a sloped surface
        // against one plane - but under PCSS a surface that is its own blocker reports a distance
        // near zero and therefore a radius near `shadow_soft_min`, so a wide kernel only happens
        // where the blocker really is somewhere else. Acne stays in the regime `shadow_bias` was
        // swept in (4.59); scaling here would buy nothing and peter-pan every soft edge.
        for (uint i = 0u; i < taps; ++i) {
            const float2 tap =
                clamp(local + kShadowDisc[i] * radius * frame.shadow_texel, lo, hi);
            const float d =
                textures[frame.shadow_texture].SampleLevel(samplers[frame.shadow_sampler],
                                                           (tap + tile) * 0.5, 0.0).r;
            lit += reference <= d ? 1.0 : 0.0;
        }
        lit /= float(taps);
    }
    return lerp(1.0, lit, saturate(frame.shadow_strength));
}

// The level's own baked lighting, re-created at this fragment.
//
// **This is a fitted model, not a chosen one** (notes §4.54). It was recovered by least squares
// against `SHPVTINT` itself over four levels, and it reaches r = 0.957 on level05 (MAE 4.87/255),
// 0.926 on level04 and 0.875 on level01. Every term below earned its place in an ablation:
//
//     distance only          r 0.36 .. 0.51
//     + max(0, N.L)          r 0.72 .. 0.84
//     + cone, omni exempt    r 0.87 .. 0.96
//
// Three things that look arbitrary and are measurements:
//
//   - the falloff is **linear in range**, beating a raised cosine and an inverse square on every
//     level tried;
//   - the axis is **row 2** of the orientation. Rows 0 and 1 both fit worse, and negating row 2
//     collapses the fit to r 0.02 - a wrong axis degrades, the opposite axis destroys;
//   - `spread` is **not** here. As a cone exponent it makes the fit worse everywhere, so the
//     field is authored on all 3,794 shipped lights and shapes nothing.
//
// **No gamma conversion, and that is deliberate.** The fit was run against the stored bytes over
// 255, so what this returns is already in the encoding the texture stages consume - the fit
// absorbed the transfer function rather than leaving one to apply here.
// Which lights this fragment has to consider.
//
// **The grid is exact, not an approximation.** A light's `range` is a hard cutoff in the fitted
// model (§4.54), so a light whose sphere misses a cell contributes exactly zero to every fragment
// in it - which is why `render.map_light_cull` on and off must produce a *bit-identical* frame
// rather than a close one, and why that is the test.
//
// A fragment outside the map's bounds clamps into the edge cell rather than going unlit.
uint2 map_light_range(float3 world_position) {
    if ((pc.frame[0].map_flags & kMapLightingGrid) == 0u) {
        return uint2(0u, pc.frame[0].map_light_count); // no grid: every light, phase 3b's behaviour
    }
    // The header, written by vkCmdUpdateBuffer before the build: [0] allocator, [1..3] dims,
    // [4..6] origin, [7..9] cell size, cells from [12]. It lives here rather than in the push
    // block because that block is full at Vulkan's guaranteed 128 bytes.
    const uint3 dims = uint3(pc.frame[0].light_grid[1], pc.frame[0].light_grid[2], pc.frame[0].light_grid[3]);
    const float3 origin = float3(asfloat(pc.frame[0].light_grid[4]), asfloat(pc.frame[0].light_grid[5]),
                                 asfloat(pc.frame[0].light_grid[6]));
    const float3 cell_size = float3(asfloat(pc.frame[0].light_grid[7]), asfloat(pc.frame[0].light_grid[8]),
                                    asfloat(pc.frame[0].light_grid[9]));
    const float3 cell = (world_position - origin) / cell_size;
    // Clamped rather than rejected: a fragment outside the map's bounds takes the edge cell, so
    // geometry that pokes past them is lit rather than black.
    const uint3 coordinate = uint3(clamp(int3(floor(cell)), int3(0, 0, 0), int3(dims) - 1));
    const uint index = coordinate.x + dims.x * (coordinate.y + dims.y * coordinate.z);
    const uint base = pc.frame[0].light_grid[12u + 2u * index + 0u];
    const uint count = pc.frame[0].light_grid[12u + 2u * index + 1u];
    return uint2(base, count);
}


// This pixel's ambient occlusion, or 1.0 wherever the pass did not run (§4.86).
//
// **`Load` and not a sample**, which is the whole reason this needs no sampler and no uv: the AO
// target is the render target's own size and the prepass rasterised it through the same
// half-pixel viewport origin (§4.28), so pixel (i,j) here IS pixel (i,j) there. A filtered
// sample would only reintroduce the blur this technique exists to avoid.
float ao_at(int2 pixel, bool map_geometry) {
    if (pc.frame[0].ao_texture == kNoTexture) {
        return 1.0;
    }
    if ((pc.frame[0].ao_flags & kAoMapOnly) != 0u && !map_geometry) {
        return 1.0;
    }
    return textures[pc.frame[0].ao_texture].Load(int3(pixel, 0)).x;
}

// `ao` scales this rig's whole contribution - see the return statement for why the whole of it and
// not just the ambience floor. Passing 1.0 leaves the function as it was, bit for bit.
float3 map_light_sum(float3 world_position, float3 normal, float ao) {
    float3 acc = float3(0, 0, 0);
    const uint2 span = map_light_range(world_position);
    const bool culled = (pc.frame[0].map_flags & kMapLightingGrid) != 0u;
    // Hoisted: the loop runs up to a hundred times on a busy fragment and this is one load.
    const uint frame_shadow_texture = pc.frame[0].map_shadow_texture;
    // `map_shadow_texture` says the atlas exists; this bit says the MAP LIGHTS may sample it.
    // Two features share the image now (§4.65), so each has to ask about itself.
    const bool map_shadows_on = (pc.frame[0].light_flags & kMapShadowsOn) != 0u;
    for (uint slot = 0u; slot < span.y; ++slot) {
        // One indirection when the grid is up, none when it is not - so the fallback really is
        // the same loop phase 3b ran rather than a second code path to keep in step.
        const uint i = culled ? pc.frame[0].light_indices[span.x + slot] : slot;
        const GpuMapLight light = pc.frame[0].map_lights[i];
        const float3 delta = light.position.xyz - world_position;
        const float distance_squared = dot(delta, delta);
        const float range = light.position.w;
        if (distance_squared >= range * range) {
            continue;
        }
        const float distance = sqrt(max(distance_squared, 1e-12));
        const float3 to_light = delta / distance;
        // **The fitted linear falloff, with its tail windowed to zero** (notes §4.64).
        //
        // `1 - d/range` reaches zero with a NON-ZERO slope, and per pixel that draws a visible
        // rim around every light - a first-derivative jump, which the eye reads as a hard disc
        // even though the value itself is continuous. Play reported it as "an unnatural disk"
        // and a scan across one measures exactly that: flat, then a linear ramp starting
        // abruptly from zero.
        //
        // Per VERTEX it is invisible, which is why §4.54's fit never saw it - the tail is
        // interpolated across whole terrain triangles. `(1 - t)(1 - t^4)` has zero derivative at
        // t = 1 and is within 6% of linear at the half-range, so it changes the model only where
        // the vertex data had least to say: refitting costs r 0.957 -> 0.949 on level05, 0.926 ->
        // 0.925 on level04, 0.875 -> 0.861 on level01. Still nearer linear than the cosine
        // falloff §4.54 rejected, which is the other smooth-tailed candidate.
        const float t = distance / range;
        float atten = (1.0 - t) * (1.0 - t * t * t * t) * max(0.0, dot(normal, to_light));
        if (atten <= 0.0) {
            continue;
        }
        if ((uint(light.colour.w) & kMapLightOmni) == 0u) {
            // The cone: how far off the light's own axis this fragment lies. Measured from the
            // light TOWARDS the fragment, which is the opposite of `to_light`.
            atten *= max(0.0, dot(light.axis.xyz, -to_light));
            if (atten <= 0.0) {
                continue;
            }
        }
        // The static shadow, LAST - after every cheap rejection above. A light that this fragment
        // is out of range of, facing away from or outside the cone of costs no texture fetch at
        // all, which is what keeps the atlas affordable over a mean of 11.5 lights a fragment.
        // `axis.w` is -1 until the bake has reached this light, so a level shadows progressively
        // rather than starting black.
        const int shadow_slot = int(light.axis.w);
        if (shadow_slot >= 0 && frame_shadow_texture != kNoTexture && map_shadows_on) {
            atten *= cube_shadow_visibility(world_position, normal, light.position.xyz,
                                            light.position.w, uint(shadow_slot), static_atlas());
            if (atten <= 0.0) {
                continue;
            }
        }
        acc += light.colour.rgb * atten;
    }
    // AMBIENCE is a per-channel max(), not an added term - AvP's own renderer clamps upward
    // against it, and adding it instead would lift a fully-lit surface past white.
    //
    // **The occlusion scales the whole rig, floor included**, and that is a claim about what this
    // rig IS rather than a convenience. Occlusion belongs on light that arrives from everywhere
    // rather than along one ray, and the `STDLIGHT` set is exactly that here: 51 static lights on
    // level02 whose whole job was to bake this level's vertex colours, standing in for an
    // environment. D3D's own lights are the other kind - the sun and the fires, few and moving -
    // and they are occluded by shadow maps instead, per light, which is why `ao_direct` defaults
    // to leaving them alone.
    //
    // Putting it on the ambience floor alone was measured and is very nearly inert: `acc * gain`
    // wins the max almost everywhere a level is lit at all, so AO reached 0.31% of the frame.
    return ao * max(saturate(acc * pc.frame[0].map_light_gain),
                    float3(pc.frame[0].map_ambience, pc.frame[0].map_ambience,
                           pc.frame[0].map_ambience));
}

// The body, with the flat copies left to the entry point below. Two exits, and a third would be
// easy to add - so nothing here writes `color_flat`.
//
// **Takes a surface point rather than a vertex id** so that the domain shader (§4.71) can hand it
// a point on a PN patch instead of a pulled vertex. Everything downstream - the two non-lighting
// paths, the per-pixel exit, `D3DRS_NORMALIZENORMALS`, the whole per-vertex light sum - is then
// shared rather than written twice, which is what keeps the tessellated and untessellated paths
// from drifting apart. `local_pos` and `local_normal` are OBJECT space, exactly as the canonical
// vertex stores them.
VertexOut shade_surface(float3 local_pos, float3 local_normal, float4 uv, float4 vertex_colour) {
    GpuDrawRecord draw = pc.draws[pc.record];

    VertexOut output;
    // No branch on pre-transformed vertices on purpose: the CPU puts either world*view*proj or
    // a pixels-to-clip orthographic matrix in `mvp`, so both FVF families are one code path. A
    // branch would have had to know the viewport, which the shader does not.
    //
    // `mul(v, M)` and not `mul(M, v)`: that is D3D's row-vector convention, and combined with
    // -matrix-layout-row-major it means the CPU can upload world*view*proj exactly as the game
    // composes it, with no transpose step to get backwards.
    const float4 local = float4(local_pos, 1.0);
    output.position =
        mul(local, rows_to_matrix(draw.mvp[0], draw.mvp[1], draw.mvp[2], draw.mvp[3]));
    output.uv = uv;
    output.specular = float4(0, 0, 0, 0);
    // Written before either exit below, because Slang requires every output to be assigned on
    // every path and the fragment shader reads these only on the lit one.
    output.world_position = float3(0, 0, 0);
    output.world_normal = float3(0, 0, 1);

    if ((draw.lighting & kLightSum) == 0u) {
        // Two non-lighting paths. kLitCollapse is the previous build's behaviour, kept so the
        // two can be compared on one paused frame rather than across two launches - which is
        // the only comparison with a noise floor small enough to see this (§4.21). Neither set
        // means lighting is off, and D3D's lighting-off result is the vertex colour itself.
        output.color = (draw.lighting & kLitCollapse) != 0u
                           ? unpack_colour(draw.lit_colour)
                           : vertex_colour;
        return output;
    }

    const float3 world_position =
        mul(local, rows_to_matrix(draw.world[0], draw.world[1], draw.world[2], draw.world[3]))
            .xyz;
    // The inverse transpose, so a non-uniform scale in the world matrix does not tilt the
    // normal. Normalised only under D3DRS_NORMALIZENORMALS, because D3D does not do it
    // otherwise and the resulting length change is a real difference in brightness.
    float3 normal = mul(local_normal,
                        float3x3(draw.normal_transform[0].xyz, draw.normal_transform[1].xyz,
                                 draw.normal_transform[2].xyz));
    if ((draw.lighting & kNormaliseNormals) != 0u) {
        normal = normalize(normal);
    }
    output.world_position = world_position;
    output.world_normal = normal;

    // Per-pixel lighting: hand the fragment shader the ingredients and stop. `color` carries the
    // RAW vertex colour rather than a lit one, which is what D3DRS_*MATERIALSOURCE needs and the
    // only thing per-vertex data can still contribute - so this needs no varying that was not
    // already here, and `world_position`/`world_normal` are the two the lighting map added.
    //
    // The fragment shader distinguishes the two meanings of `color` exactly as this does: by
    // `kLightSum` and this same push constant. Nothing else reads it.
    if (pc.frame[0].per_pixel_lighting != 0.0) {
        output.color = vertex_colour;
        return output;
    }

    const bool want_specular = (draw.lighting & kSpecularEnable) != 0u;
    const float3 to_eye = want_specular ? normalize(draw.eye.xyz - world_position) : float3(0, 0, 1);
    // Visibility 1: the per-VERTEX path reproduces the fixed function, which has no shadow at all.
    const LightSums sums = light_sum(draw, world_position, normal, want_specular, to_eye,
                                     draw.material_specular.w, 1.0);
    const LitColour lit = resolve_lit_colour(draw, sums, vertex_colour, want_specular);
    output.color = lit.color;
    output.specular = float4(lit.specular, 0.0);
    return output;
}

[shader("vertex")]
VertexOut vertex_main(uint vertex_id: SV_VertexID) {
    Vertex v = pc.vertices[pc.base_vertex + vertex_id];
    VertexOut output = shade_surface(v.pos.xyz, v.normal_and_color.xyz, v.uv,
                                     unpack_colour(asuint(v.normal_and_color.w)));
    // The same values, differing only in how they are interpolated. Mirrored here, at the one
    // exit there is, rather than at each exit of the body above - which is the whole reason the
    // body is a separate function.
    output.color_flat = output.color;
    output.specular_flat = output.specular;
    return output;
}

// --- PN-triangle amplification (§4.71) --------------------------------------------------------
//
// Vlachos et al.'s curved point-normal triangle: a cubic Bezier patch fitted to three corner
// positions and three corner normals. The whole reason it is the right construction for THIS game
// is one property of its edge control point,
//
//     b210 = (2*P1 + P2 - dot(P2 - P1, N1) * N1) / 3
//
// which collapses to the linear `(2*P1 + P2)/3` exactly when `N1` is perpendicular to the edge -
// i.e. when the corner's normal is its face normal. A flat-shaded wall therefore reproduces
// itself bit for bit at any tessellation factor, with no threshold and no per-material opt-in,
// while a smooth-normalled pipe curves. Hard edges cost nothing because the mathematics, not a
// heuristic, leaves them alone.
//
// The same expression is why the patch is **watertight across a shared edge**: `b210` and `b120`
// for edge (P1,P2) are functions of P1, P2, N1 and N2 alone, so two triangles sharing that edge
// build the same boundary curve. Everything below preserves that invariant, and the one thing
// that would break it is a term computed from the triangle's centroid or its third vertex - which
// is why the tessellation factors are per-edge and `pn_flat_threshold` tests a per-edge quantity.
//
// Everything here is in OBJECT space, so `shade_surface`'s own `mvp` projects the result and no
// extra matrix is needed anywhere.

// What the vertex stage hands the tessellator: the canonical vertex, unpacked and untransformed.
struct ControlPoint {
    float3 pos;
    float3 normal;
    float4 uv;
    float4 colour;
    // Whether the mesh has split THIS corner into vertices carrying different normals, in which
    // case its tangent term is zeroed and the two edges leaving it stay linear. See
    // `is_split_corner` - this is looked up in the vertex stage because that is the only stage
    // that still knows the vertex index.
    nointerpolation uint split;
};

// **The condition the watertight property actually rests on, made checkable.**
//
// `b210` for edge (P1,P2) is a function of P1, P2 and N1 alone, so the two triangles sharing that
// edge build the same curve *provided each presents the same normal at each end*. Where an
// exporter has split a corner at a material boundary or a smoothing-group break, they do not, and
// the patches pull apart - one shared map edge in five on level02, worst 0.0696 world units at the
// defaults (§4.71). No patch can see the other side, so the only quantities both could agree on
// are the two positions; the fix is therefore to answer the question on the CPU, where the whole
// mesh is visible, and hand the answer down as one bit per canonical vertex.
//
// The bit says "this corner is not smooth here", which is what the split is telling us, so zeroing
// its term is also the *right* answer rather than merely a safe one: an intentional hard edge
// stays hard instead of being averaged smooth. Both sides read the same bit - a split marks every
// vertex at that position - so they agree by construction.
bool is_split_corner(uint vertex) {
    const uint count = pc.frame[0].split_count;
    if (count == 0u || vertex < pc.frame[0].split_base) {
        return false;
    }
    const uint index = vertex - pc.frame[0].split_base;
    if (index >= count) {
        return false;
    }
    return (pc.frame[0].split_corners[index >> 5u] & (1u << (index & 31u))) != 0u;
}

struct PatchConstants {
    float edges[3]  : SV_TessFactor;
    float inner     : SV_InsideTessFactor;
};

[shader("vertex")]
ControlPoint tess_vertex_main(uint vertex_id: SV_VertexID) {
    Vertex v = pc.vertices[pc.base_vertex + vertex_id];
    ControlPoint cp;
    cp.pos = v.pos.xyz;
    cp.normal = v.normal_and_color.xyz;
    cp.uv = v.uv;
    // Unpacked here rather than in the domain shader because the tessellator interpolates it: a
    // packed D3DCOLOR cannot be blended as an integer, and unpacking after interpolation would
    // mean interpolating the *bits*.
    cp.colour = unpack_colour(asuint(v.normal_and_color.w));
    cp.split = is_split_corner(pc.base_vertex + vertex_id) ? 1u : 0u;
    return cp;
}

// One patch edge's factor, in the target's own pixels.
//
// **A function of its two endpoints and nothing else.** That is the watertight rule, and it is
// why this takes two clip positions rather than the patch: a factor derived from the centroid, or
// from the triangle's area, differs between the two triangles sharing an edge and tears a seam
// open along it. `length(a - b)` is symmetric, so the two also agree bit for bit rather than
// merely closely.
float edge_factor(float4 a, float4 b) {
    // `abs(w)` rather than a guard on the sign: a vertex behind the eye has a negative w, and the
    // divide would otherwise mirror it to the far side of the screen and read as a short edge.
    // The magnitude is meaningless there, but it is large and finite, so the clamp below takes it
    // to `tess_max` - which is the right answer for something straddling the near plane.
    const float wa = max(abs(a.w), 1e-4);
    const float wb = max(abs(b.w), 1e-4);
    const float2 half_target = float2(pc.frame[0].target_width, pc.frame[0].target_height) * 0.5;
    const float2 pa = a.xy / wa * half_target;
    const float2 pb = b.xy / wb * half_target;
    const float pixels = length(pa - pb);
    return clamp(pixels / max(pc.frame[0].tess_edge_pixels, 1.0), pc.frame[0].tess_min,
                 pc.frame[0].tess_max);
}

PatchConstants hull_constants(InputPatch<ControlPoint, 3> patch) {
    PatchConstants out;
    const float4x4 mvp = rows_to_matrix(pc.draws[pc.record].mvp[0], pc.draws[pc.record].mvp[1],
                                        pc.draws[pc.record].mvp[2], pc.draws[pc.record].mvp[3]);
    const float4 c0 = mul(float4(patch[0].pos, 1.0), mvp);
    const float4 c1 = mul(float4(patch[1].pos, 1.0), mvp);
    const float4 c2 = mul(float4(patch[2].pos, 1.0), mvp);
    // **Edge i is the one OPPOSITE control point i**, which is the domain's own convention: outer
    // factor 0 governs the edge where the first barycentric coordinate is zero, and that edge
    // joins control points 1 and 2. Assigning them in the obvious (0,1), (1,2), (2,0) order
    // instead still produces a plausible picture and cracks along every shared edge, because the
    // two triangles either side then apply their agreed factor to different edges.
    out.edges[0] = edge_factor(c1, c2);
    out.edges[1] = edge_factor(c2, c0);
    out.edges[2] = edge_factor(c0, c1);
    // The interior has no neighbour to agree with, so it is free to be the mean.
    out.inner = (out.edges[0] + out.edges[1] + out.edges[2]) / 3.0;
    return out;
}

[shader("hull")]
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("hull_constants")]
ControlPoint hull_main(InputPatch<ControlPoint, 3> patch, uint id: SV_OutputControlPointID) {
    return patch[id];
}

// The tangent term `dot(Pj - Pi, Ni)`, with the floor, the ceiling and the strength applied.
//
// The floor is tested on the term divided by the edge's length - the same quantity
// `render.normal_census` reports - so it means the same thing on a large triangle and a small one.
// The ceiling is the opposite: it is in WORLD UNITS and deliberately does not scale with the edge,
// because what it exists to bound is a distance on screen.
//
// **Why a ceiling is needed at all** (§4.74). The construction is not wrong here and the normals
// are not creases: on level02's pipe the tangent terms measure 0.2 to 0.35, which is a 23 to 41
// degree turn per edge - exactly the cross-section of a ten- to sixteen-sided tube, and the
// normals are asking for precisely the cylinder the artist approximated. The trouble is that
// `w * length / 3` has no bound: Gunlok builds its round objects from very few, very long
// segments, so honouring a legitimate 0.3 term over a 3-unit edge moves the surface half a unit
// and the pipe reads as inflated rather than as rounded. `pn_flat_threshold` cannot reach this -
// it is a floor, and this is a failure at the ceiling.
//
// Every operation is a function of (Pi, Pj, Ni) alone, so the watertight property survives all
// three: the triangle on the other side of edge (i,j) computes, tests, snaps and clamps the
// identical number - **provided it has the same Ni**, which is exactly what `split` says it does
// not. A split corner returns zero here, so both sides build the linear boundary and agree.
float pn_weight(float3 pi, float3 pj, float3 ni, uint split) {
    if (split != 0u) {
        return 0.0;
    }
    const float3 edge = pj - pi;
    const float length_sq = dot(edge, edge);
    if (length_sq < 1e-18) {
        return 0.0;
    }
    const float w = dot(edge, ni);
    // `w * w` against `threshold^2 * length^2` rather than a square root on the hot path; the
    // comparison is exact in the same places the rooted one would be.
    const float threshold = pc.frame[0].pn_flat_threshold;
    if (w * w <= threshold * threshold * length_sq) {
        return 0.0; // near enough flat that the corner is made exactly flat
    }
    // Clamped and not zeroed: a bulge that merely overshoots the cap becomes exactly the cap, so
    // the surface stays continuous as an object grows across the limit instead of snapping flat.
    // The control point moves `|w| / 3`, so the cap on `w` is three times the cap on the distance.
    const float limit = 3.0 * pc.frame[0].pn_max_offset;
    return clamp(w, -limit, limit) * pc.frame[0].pn_strength;
}

// **The spacing and the winding are repeated here on purpose.** In HLSL they are hull-shader
// attributes, but SPIR-V carries the tessellation execution modes on the *evaluation* stage, and
// Slang emits only what each entry point declares. Without these the domain shader defaults to
// SpacingEqual and the two stages disagree - which Vulkan validation reports on every draw as
// "the spacing specified in tessellation control shader does not match". Measured, not guessed.
[shader("domain")]
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
VertexOut domain_main(PatchConstants constants, float3 bary: SV_DomainLocation,
                      const OutputPatch<ControlPoint, 3> patch) {
    const float3 p1 = patch[0].pos, p2 = patch[1].pos, p3 = patch[2].pos;
    // Normalised here and not in the vertex stage: the construction's flatness property is about
    // the *direction* of the normal, and an unnormalised one would scale the tangent offset by
    // its length. `shade_surface` still receives the raw interpolated normal below, because
    // whether D3D normalises is `D3DRS_NORMALIZENORMALS`'s business and not this pass's.
    const float3 n1 = normalize(patch[0].normal);
    const float3 n2 = normalize(patch[1].normal);
    const float3 n3 = normalize(patch[2].normal);

    // The six edge control points. Named for their barycentric exponents, as Vlachos does:
    // b210 carries u^2*v, so it sits on edge 1->2, nearer corner 1.
    // The split flag gates by whose NORMAL each control point uses, not by which edge it sits on:
    // b210 and b201 both bend along n1, so both answer to corner 0's bit.
    const uint s1 = patch[0].split, s2 = patch[1].split, s3 = patch[2].split;
    const float3 b210 = (2.0 * p1 + p2 - pn_weight(p1, p2, n1, s1) * n1) / 3.0;
    const float3 b120 = (2.0 * p2 + p1 - pn_weight(p2, p1, n2, s2) * n2) / 3.0;
    const float3 b021 = (2.0 * p2 + p3 - pn_weight(p2, p3, n2, s2) * n2) / 3.0;
    const float3 b012 = (2.0 * p3 + p2 - pn_weight(p3, p2, n3, s3) * n3) / 3.0;
    const float3 b102 = (2.0 * p3 + p1 - pn_weight(p3, p1, n3, s3) * n3) / 3.0;
    const float3 b201 = (2.0 * p1 + p3 - pn_weight(p1, p3, n1, s1) * n1) / 3.0;
    // The interior point, pulled half again from the average of the six towards itself. This one
    // IS a function of all three corners - which is allowed precisely because it only ever
    // contributes where u, v and w are all non-zero, i.e. strictly inside the patch, and the
    // watertight requirement is about the boundary.
    const float3 e = (b210 + b120 + b021 + b012 + b102 + b201) / 6.0;
    const float3 v = (p1 + p2 + p3) / 3.0;
    const float3 b111 = e + (e - v) * 0.5;

    const float u = bary.x, vv = bary.y, w = bary.z;
    const float u2 = u * u, v2 = vv * vv, w2 = w * w;
    const float3 position = p1 * (u2 * u) + p2 * (v2 * vv) + p3 * (w2 * w) +
                            b210 * (3.0 * u2 * vv) + b120 * (3.0 * u * v2) +
                            b201 * (3.0 * u2 * w) + b021 * (3.0 * v2 * w) +
                            b102 * (3.0 * u * w2) + b012 * (3.0 * vv * w2) +
                            b111 * (6.0 * u * vv * w);

    // **Linearly interpolated, not Vlachos' quadratic normal field**, and deliberately: the
    // per-pixel path (§4.52) already interpolates and normalises linearly across a triangle, so
    // keeping it means a tessellated draw differs from an untessellated one ONLY in silhouette.
    // That is what makes the A/B readable - a quadratic field would move the shading too, and the
    // two effects could not be told apart in a difference image.
    const float3 normal = patch[0].normal * u + patch[1].normal * vv + patch[2].normal * w;
    const float4 uv = patch[0].uv * u + patch[1].uv * vv + patch[2].uv * w;
    const float4 colour = patch[0].colour * u + patch[1].colour * vv + patch[2].colour * w;

    VertexOut output = shade_surface(position, normal, uv, colour);
    // Flat shading and tessellation do not compose - the provoking vertex of a GENERATED triangle
    // is not the provoking vertex of the patch - but nothing tessellated here is flat-shaded:
    // D3DRS_SHADEMODE is FLAT on 2% of draws and all of them are the stencil shadow (§4.31),
    // which is not map geometry. Mirrored anyway so the varying is written on every path.
    output.color_flat = output.color;
    output.specular_flat = output.specular;
    return output;
}

// --- the fixed-function texture stages ----------------------------------------------------
//
// D3D8's multitexture cascade, reproduced rather than approximated: each stage computes a
// colour from its own two arguments and an alpha from its own two, both from the state as it
// stood *before* the stage, and the pair becomes CURRENT for the next one.
//
// Only the ops the game is measured to use are implemented (§4.19): SELECTARG1/2, MODULATE and
// its 2X/4X, ADD, ADDSIGNED and its 2X, SUBTRACT, and the three alpha blends. The rest fall
// through to "the first argument", and the CPU counts a draw that needs one - a silent wrong
// answer here would look like a shading bug rather than a missing feature.

float4 select_arg(uint arg, float4 tex, float4 diffuse, float4 current) {
    float4 v;
    switch (arg & 0x0fu) {          // D3DTA_SELECTMASK
    case 0u: v = diffuse; break;    // D3DTA_DIFFUSE
    case 1u: v = current; break;    // D3DTA_CURRENT
    case 2u: v = tex; break;        // D3DTA_TEXTURE
    case 3u: v = float4(1, 1, 1, 1); break; // D3DTA_TFACTOR - the game never sets one
    // D3DTA_SPECULAR. The canonical vertex drops the specular colour (§4.10), so this is the
    // fixed-function default of black rather than a value that was thrown away: the game does
    // not name it in any measured stage argument.
    case 4u: v = float4(0, 0, 0, 0); break;
    default: v = current; break;
    }
    if ((arg & 0x10u) != 0u) {      // D3DTA_COMPLEMENT
        v = 1.0 - v;
    }
    if ((arg & 0x20u) != 0u) {      // D3DTA_ALPHAREPLICATE
        v = v.wwww;
    }
    return v;
}

// **The 0.5 here is unconditional, and that is the point of running the cascade encoded.**
//
// `D3DTOP_ADDSIGNED` is `a1 + a2 - 0.5`, where 0.5 is where the fixed function's 0..255 fixed-point
// encoding puts "no change" - it is a property of the domain, not a quantity of light. The first
// attempt at §4.96 kept the operands decoded and moved the constant to `srgb_decode(0.5)` instead;
// that restores the neutral point and **is not enough**, because a sum does not survive a
// non-linear transform however its bias is chosen. `fragment_main` re-encodes the operands instead,
// so every constant in this function is back in the domain it was written for.
float4 apply_op(uint op, float4 a1, float4 a2, float4 tex, float4 diffuse, float4 current) {
    switch (op) {
    case 2u:  return a1;                             // SELECTARG1
    case 3u:  return a2;                             // SELECTARG2
    case 4u:  return a1 * a2;                        // MODULATE
    case 5u:  return a1 * a2 * 2.0;                  // MODULATE2X
    case 6u:  return a1 * a2 * 4.0;                  // MODULATE4X
    case 7u:  return a1 + a2;                        // ADD
    case 8u:  return a1 + a2 - 0.5;                  // ADDSIGNED
    case 9u:  return (a1 + a2 - 0.5) * 2.0;          // ADDSIGNED2X
    case 10u: return a1 - a2;                        // SUBTRACT
    case 12u: return lerp(a2, a1, diffuse.a);        // BLENDDIFFUSEALPHA
    case 13u: return lerp(a2, a1, tex.a);            // BLENDTEXTUREALPHA
    case 16u: return lerp(a2, a1, current.a);        // BLENDCURRENTALPHA
    default:  return a1;                             // counted on the CPU, never silent
    }
}

struct Stage {
    uint texture;
    uint sampler_index;
    uint color;
    uint alpha;
};

Stage stage_at(GpuMaterial m, uint index) {
    Stage s;
    if (index == 0u) {
        s.texture = m.stage0_texture;
        s.sampler_index = m.stage0_sampler;
        s.color = m.stage0_color;
        s.alpha = m.stage0_alpha;
    } else {
        s.texture = m.stage1_texture;
        s.sampler_index = m.stage1_sampler;
        s.color = m.stage1_color;
        s.alpha = m.stage1_alpha;
    }
    return s;
}

// The floor on both halves of the lighting map's diffuse ratio, in the light sum's own units
// (§4.98). Its job is that `0 / 0` is 1 rather than 0 - any positive value does that - and the
// magnitude only decides how far into the near-dark band the ratio is softened before
// `bump_diffuse_limit` clamps it. 0.05 is well under a twentieth of what level02's key light
// contributes at normal incidence (`diffuse 4.0`, and `k` near 1 in its own room), so it cancels
// wherever the dynamic lights are what is lighting a surface.
static const float kBumpRatioFloor = 0.05;

// --- lighting maps ----------------------------------------------------------------------------
//
// What a `<texture> lighting.dds` beside a `.RIM` adds, and the one thing in this shader the game
// never asked for beyond the override's tint. src/VkLighting.h is the file format and where it
// comes from; this is what the three channels do.
//
// **The normal is derived, not stored.** R is a height field, and its gradient over one texel
// tilts the geometric normal inside a tangent frame taken from the fragment's own derivatives -
// Mikkelsen's cotangent frame, which needs no tangent in the vertex. That matters here more than
// usual: the canonical vertex is 48 bytes and carries none (§4.10), and adding one would cost 12
// bytes on every vertex in the game to serve the few surfaces a mod ships a map for.
//
// **The highlight is a second, per-pixel specular sum**, added after the texture stages exactly
// where D3D adds its own second colour. It runs the same `light_geometry` the vertex shader does,
// so range, attenuation and the spot cone cannot disagree between the two, and it takes the
// light's **specular** colour, which Gunlok's lights really do author - all 49 `light` sections in
// the shipped `.gsh` set a `specular red/green/blue`.
//
// **The bump also reaches the diffuse**, as a ratio rather than a re-lighting: the same sum with
// the bumped normal over the same sum with the geometric one, blended by `bump_diffuse`. It is a
// ratio because the fragment does not know which colour D3DRS_DIFFUSEMATERIALSOURCE selected, and
// the ratio cancels it. That is exact only where the material's ambient and diffuse colours agree
// - the ambient term is in both halves to keep a surface facing away from every light from
// dividing by nothing - and it is why `render.bump_diffuse` exists as a knob rather than being
// folded in.
struct LightingMapShade {
    float3 specular;      // to add after the stages
    float3 diffuse_scale; // to multiply the vertex-lit colour by BEFORE them
    // The tilted normal, in world space, and the two channels the chrome pass reads. They fall out
    // of work this function already does - the gradient taps and one `centre` fetch - so a caller
    // that wants only these pays for the taps and nothing else, which is what `texgen_only` is.
    float3 bumped_normal;
    float metallic;
    float roughness;
};

// `texgen_only` runs the height gradient and stops: no light loop, no highlight, no diffuse ratio.
// That is what the chrome pass wants, and not as an optimisation - the base pass has ALREADY added
// both terms for this surface, because the chrome pass is a second draw of the same mesh with the
// same stage-0 texture and therefore the same lighting map. Running them again would apply the map
// twice to one pixel. It is also, incidentally, much the cheaper half.
LightingMapShade shade_lighting_map(GpuMaterial material, GpuDrawRecord draw, float2 uv,
                                    float2 uv_dx, float2 uv_dy, float3 position_dx,
                                    float3 position_dy, float3 world_position,
                                    float3 world_normal, bool texgen_only) {
    LightingMapShade out;
    out.specular = float3(0, 0, 0);
    out.diffuse_scale = float3(1, 1, 1);
    out.bumped_normal = normalize(world_normal);
    out.metallic = 1.0;
    out.roughness = 0.0;

    const uint index = material.lighting_texture;
    const uint sampler_index = material.stage0_sampler;

    // SampleGrad, not Sample: this whole function runs under a branch on the material, and an
    // implicit-derivative fetch in non-uniform control flow is undefined. The gradients are taken
    // once at the top of the entry point, where control flow is uniform, and passed in - which
    // also keeps the mip selection identical to the base texture's.
    const float4 centre = textures[index].SampleGrad(samplers[sampler_index], uv, uv_dx, uv_dy);
    const float metallic = centre.g;
    const float roughness = centre.b;
    out.metallic = metallic;
    out.roughness = roughness;

    uint width, height;
    textures[index].GetDimensions(width, height);
    const float2 texel = float2(1.0 / float(max(width, 1u)), 1.0 / float(max(height, 1u)));

    // A central difference over one texel each way, so the gradient is in HEIGHT PER TEXEL and
    // `bump_scale` means the same thing whatever the map's resolution: at 1.0, a full-contrast
    // step across one texel is a 45-degree slope.
    const float left =
        textures[index].SampleGrad(samplers[sampler_index], uv - float2(texel.x, 0.0), uv_dx, uv_dy).r;
    const float right =
        textures[index].SampleGrad(samplers[sampler_index], uv + float2(texel.x, 0.0), uv_dx, uv_dy).r;
    const float down =
        textures[index].SampleGrad(samplers[sampler_index], uv - float2(0.0, texel.y), uv_dx, uv_dy).r;
    const float up =
        textures[index].SampleGrad(samplers[sampler_index], uv + float2(0.0, texel.y), uv_dx, uv_dy).r;
    const float2 gradient = float2(right - left, up - down) * 0.5;

    const float3 normal = normalize(world_normal);
    // The cotangent frame: solve for the tangent and bitangent that carry this triangle's uv
    // parameterisation, from the derivatives of position and uv that the rasteriser already has.
    const float3 dp2perp = cross(position_dy, normal);
    const float3 dp1perp = cross(normal, position_dx);
    float3 tangent = dp2perp * uv_dx.x + dp1perp * uv_dy.x;
    float3 bitangent = dp2perp * uv_dx.y + dp1perp * uv_dy.y;
    const float tangent_length = length(tangent);
    const float bitangent_length = length(bitangent);
    if (tangent_length < 1e-12 || bitangent_length < 1e-12) {
        // A degenerate frame: a zero-area triangle, or a draw whose uv does not vary across the
        // pixel. There is no direction for a bump to lean in, so the map contributes nothing
        // rather than a normalised NaN - which would paint one black pixel and be blamed on the
        // artwork.
        //
        // `bumped_normal` stays the geometric one and `metallic` stays what was sampled, so a
        // chrome pass here still reflects and still respects the channel - it just reflects off
        // the flat surface. Bailing out of the whole effect instead would make a degenerate
        // triangle a HOLE in the reflection, which is far more visible than a missing bump.
        return out;
    }
    tangent /= tangent_length;
    bitangent /= bitangent_length;
    const float3 bumped = normalize(
        normal - pc.frame[0].bump_scale * (tangent * gradient.x + bitangent * gradient.y));
    out.bumped_normal = bumped;
    if (texgen_only) {
        return out;
    }

    // Roughness to exponent, interpolated in log2: a linear walk from 4 to 256 spends nearly all
    // of its range on highlights that are already broad, where the eye reads the difference
    // between 4 and 8 as large and between 200 and 256 as nothing.
    const float exponent = exp2(lerp(log2(max(pc.frame[0].gloss_min, 1.0)), log2(max(pc.frame[0].gloss_max, 1.0)),
                                     saturate(1.0 - roughness)));
    const float3 to_eye = normalize(draw.eye.xyz - world_position);

    float3 ambient_sum = draw.global_ambient.rgb;
    float3 diffuse_flat = float3(0, 0, 0);
    float3 diffuse_bumped = float3(0, 0, 0);
    float3 specular_sum = float3(0, 0, 0);

    for (uint i = 0u; i < draw.light_count; ++i) {
        const GpuLight light = pc.lights[draw.light_offset + i];
        const LightGeometry geometry = light_geometry(light, world_position, normal);
        if (!geometry.valid) {
            continue;
        }
        // The same split as the fixed-function sum: ambient on `k` alone, the direct terms on
        // `k * shadow`. **The diffuse pair would cancel it** - this function returns a bumped /
        // flat ratio, so a factor on both is invisible there - but the specular below is not a
        // ratio, and a highlight that survives a wall the diffuse is blocked by is exactly the
        // class of self-disagreement §4.46 was.
        const float k = geometry.k * geometry.shadow;
        ambient_sum += geometry.k * light.ambient.rgb;
        const float lambert = max(0.0, dot(normal, geometry.to_light));
        const float lambert_bumped = max(0.0, dot(bumped, geometry.to_light));
        diffuse_flat += k * light.diffuse.rgb * lambert;
        diffuse_bumped += k * light.diffuse.rgb * lambert_bumped;
        // Gated on the BUMPED lambert, which is §4.46's condition applied to the normal this term
        // is actually shaded with: a facet turned away from the light carries no highlight, and
        // the whole point of the map is that a facet is not the triangle.
        if (lambert_bumped > 0.0) {
            const float3 halfway = normalize(geometry.to_light + to_eye);
            // NOT necessarily light.specular: see `specular_from_diffuse`. The fixed-function
            // term above uses the authored specular colour because that is what D3D does; this
            // one is a mod's response and defaults to the light a player can actually see,
            // because level02's lights all author a black specular.
            const float3 colour =
                lerp(light.specular.rgb, light.diffuse.rgb, saturate(pc.frame[0].specular_from_diffuse));
            specular_sum += k * colour * pow(max(0.0, dot(bumped, halfway)), exponent);
        }
    }

    out.specular = specular_sum * metallic * pc.frame[0].specular_scale;
    // The diffuse ratio, and both of its ends are load-bearing (§4.98).
    //
    // **The floor term is on BOTH halves, so a fragment no light reaches is INERT and not black.**
    // This function's header says the ambient term keeps a surface facing away from every light
    // from dividing by nothing, and it does - but the same term is in the numerator, so where the
    // ambient really is zero, which §4.55 records as the usual case in this game, the pair was
    // `0 / 1e-3` and the scale came out **0**. Every surface outside every light's reach was
    // therefore painted black by a feature that had nothing to say about it. `kBumpRatioFloor`
    // added to both halves makes that case exactly 1, and it cancels wherever there is real light
    // to divide - it is a conditioning term, not a look parameter, which is why it is a constant
    // here and `bump_diffuse_limit` is the knob.
    //
    // **Clamped both ways.** The ceiling was already here: where the geometric normal faces away
    // from every light, an unbounded ratio would make a bumped pixel many times brighter than
    // anything around it. The floor is that same argument's other half, and it was missing. A
    // near-vertical surface sits near the terminator of an overhead light, so `bump_scale 1`'s
    // 26-degree tilt takes `lambert_bumped` to exactly zero across the whole of it while the
    // geometric normal still faces the light - and a *ratio* of zero is a black surface rather
    // than a dark one. What the scale multiplies is the whole colour the game lit this fragment
    // with, ambient, emissive and a baked `SHPVTINT` alike, and none of those is a term a height
    // field is entitled to delete: this is a bump shaping a lit surface, not a visibility test. It
    // may deepen a crevice; it may not put a hole in one.
    const float3 floor_term = float3(kBumpRatioFloor, kBumpRatioFloor, kBumpRatioFloor);
    const float limit = max(pc.frame[0].bump_diffuse_limit, 1.0);
    const float3 ratio = clamp((ambient_sum + diffuse_bumped + floor_term) /
                                   max(ambient_sum + diffuse_flat + floor_term,
                                       float3(1e-3, 1e-3, 1e-3)),
                               float3(1.0 / limit, 1.0 / limit, 1.0 / limit),
                               float3(limit, limit, limit));
    out.diffuse_scale = lerp(float3(1, 1, 1), ratio, saturate(pc.frame[0].bump_diffuse));
    return out;
}

[shader("fragment")]
float4 fragment_main(VertexOut input) : SV_Target {
    // What the texture stages take as D3DTA_DIFFUSE, which is D3D's own order: lighting produces
    // a colour, the multitexture cascade consumes it. Where that colour is COMPUTED is the one
    // thing `per_pixel_lighting` moves - either the vertex shader already ran the equation and
    // `input.color` is its result, or it passed the raw vertex colour through and the block below
    // runs the equation here. Everything after that block is identical either way.
    const GpuMaterial material = pc.materials[pc.material];
    const GpuDrawRecord draw = pc.draws[pc.record];
    // D3DSHADE_FLAT takes both output colours from the primitive's first vertex; the texture
    // coordinates are interpolated either way, which is why only these two have a flat copy.
    const bool flat_shaded = material.shading == kShadeFlat;
    float4 diffuse = flat_shaded ? input.color_flat : input.color;
    float3 specular_in = flat_shaded ? input.specular_flat.rgb : input.specular.rgb;

    // Per-pixel lighting. `diffuse` is the raw vertex colour on this path - see shade_vertex -
    // and D3D's equation runs here instead, at this fragment's own position and normal.
    //
    // It has to happen BEFORE the lighting map below, because that feature's whole output is a
    // scale on the diffuse the texture stages are about to consume: computing the sum afterwards
    // would throw the scale away and make the map look like it had stopped working.
    if (pc.frame[0].per_pixel_lighting != 0.0 && (draw.lighting & kLightSum) != 0u) {
        // **Renormalised unconditionally**, where the vertex path does it only under
        // D3DRS_NORMALIZENORMALS. Linear interpolation of two unit normals across a triangle
        // yields a SHORTER vector everywhere between them - most so at the midpoint of a wide
        // corner - so skipping this darkens the middle of every triangle by an amount that grows
        // with how sharply the normal turns. That is interpolation's artefact rather than
        // anything D3D decided, and there is no per-vertex equivalent of it to reproduce.
        const float3 normal = normalize(input.world_normal);
        // Runtime map lighting: **replace the baked vertex colour, do not add to it.**
        //
        // `D3DRS_DIFFUSEMATERIALSOURCE` is `D3DMCS_COLOR1` on every lit draw, so this colour is
        // the *material diffuse* that the sum below multiplies - not the final pixel. Swapping
        // one term for a runtime evaluation of the same rig leaves the live D3D light sum, both
        // texture stages and the gamma-space multiply exactly as they were, which is why the
        // level's brightness balance survives. Neutralising it to white and lighting on top
        // instead is §4.25's defect: everything unfogged and far too bright.
        //
        // The alpha is kept: it is the vertex's own and nothing here computes one.
        //
        // **Only the map's own geometry**, and the marker is the fog stage. The rig being
        // evaluated is the LEVEL's; a prop or a unit is a separate `RBOBJECT` whose `SHPVTINT`
        // was baked from its own file's lights, so substituting the level's rig there replaces
        // one object's bake with another's - which the fit never covered and which measures
        // worse on screen than leaving it alone.
        //
        // Stage 1 is the fog-of-war grid on exactly the draws `SubmitAndFlushMapGeometry`
        // submits, and the only other stage-1 configuration in a frame is the chrome pass
        // (notes §4.51 tallies all 273 draws of a level02 frame into those three groups). So
        // "two stages and not chrome" is "this is the map", measured rather than assumed.
        // `map_lighting_all` turns the restriction off, which is how that claim stays checkable.
        const bool map_geometry = material.stage_count == 2u && material.chrome == 0u;
        // Read once, here, and handed to both sums below. The pixel is the fragment's own
        // framebuffer coordinate, which is what the prepass wrote.
        const float ao = ao_at(int2(input.position.xy), map_geometry);
        if ((pc.frame[0].map_flags & kMapLightingOn) != 0u &&
            (map_geometry || (pc.frame[0].map_flags & kMapLightingAll) != 0u)) {
            diffuse = float4(map_light_sum(input.world_position, normal, ao), diffuse.a);
        }
        const bool want_specular = (draw.lighting & kSpecularEnable) != 0u;
        // Per fragment, which is what D3DRS_LOCALVIEWER means taken literally. The vertex path
        // computes this once per vertex and interpolates the result of the whole highlight.
        const float3 to_eye =
            want_specular ? normalize(draw.eye.xyz - input.world_position) : float3(0, 0, 1);
        // The sun's shadow rides INTO the loop rather than being applied to the result, so it
        // reaches the one light it describes - see light_sum. Per pixel only: the vertex path is
        // the fixed-function reproduction and this is a departure from it.
        LightSums sums = light_sum(draw, input.world_position, normal, want_specular,
                                   to_eye, draw.material_specular.w,
                                   sun_visibility(input.world_position,
                                                  int2(input.position.xy), draw.eye.xyz));
        // D3D's own sum, and it is occluded on **different** terms from the map rig above. These
        // are the level's dynamic lights - the sun, level02's two fires - and each of them already
        // has a shadow map answering "is this light blocked" exactly, per light, inside the loop.
        // Multiplying them by a hemisphere's worth of nearby geometry as well is double-counting,
        // so `ao_direct` defaults to 0 and leaves them alone.
        //
        // The ambient term is not in that argument and is always occluded: nothing shadows it, and
        // it is by definition the light that did not arrive along one ray. It is usually zero in
        // this game, which is why it is not where the visible occlusion comes from.
        //
        // The specular is untouched at every setting - a highlight is a mirror of one light in one
        // direction, and nearby geometry says nothing about whether that particular path is clear.
        sums.ambient *= ao;
        sums.diffuse *= lerp(1.0, ao, pc.frame[0].ao_direct);
        const LitColour lit = resolve_lit_colour(draw, sums, diffuse, want_specular);
        diffuse = lit.color;
        specular_in = lit.specular;
    }

    // The lighting map, in two halves: a scale on the diffuse the texture stages are about to
    // consume, and a highlight added after them. Both derivatives are taken HERE, in uniform
    // control flow, because that is where they are defined - the branch below is uniform per
    // draw, but the alpha test's `discard` further down is not, and a helper that took its own
    // would be reading them across a divergence.
    const uint stage0_set = (material.stage0_color >> 24) & 0xffu;
    const float2 base_uv = stage0_set == 1u ? input.uv.zw : input.uv.xy;
    const float2 uv_dx = ddx(base_uv);
    const float2 uv_dy = ddy(base_uv);
    const float3 position_dx = ddx(input.world_position);
    const float3 position_dy = ddy(input.world_position);
    float3 map_specular = float3(0, 0, 0);
    // The chrome pass's inputs. `chrome_metallic` starts at 1 so that a surface the map says
    // nothing about reflects at the engine's own strength - the whole feature has to be inert
    // without a companion file, and a 0 default would silently delete the reflection from every
    // reflective unit on a stock install.
    const bool chrome = material.chrome != 0u;
    float chrome_metallic = 1.0;
    float chrome_roughness = 0.0;
    float3 chrome_normal = float3(0, 0, 1);
    bool chrome_texgen = false;
    // Only on a draw the game itself lit: the map's whole contribution is a light sum, and a
    // pre-transformed or unlit draw has neither a world position to run it at nor lights to run
    // it over. `kNoTexture` is what every material without a companion file carries, so nothing
    // here costs anything on a stock install.
    if (material.lighting_texture != kNoTexture) {
        if ((draw.lighting & kLightSum) != 0u) {
            const LightingMapShade shade =
                shade_lighting_map(material, draw, base_uv, uv_dx, uv_dy, position_dx,
                                   position_dy, input.world_position, input.world_normal, chrome);
            // Both are identities under `texgen_only`, so this stays one code path rather than
            // branching on which half ran.
            diffuse = float4(diffuse.rgb * shade.diffuse_scale, diffuse.a);
            map_specular = shade.specular;
            chrome_metallic = shade.metallic;
            chrome_roughness = shade.roughness;
            // World -> view. `mul(vector, matrix)` treats the vector as a ROW vector, which is
            // the same convention D3D's matrices are written in - so this is literally the
            // `v * View` the game means, with the rows in the order StoreViewRotation wrote them.
            // The same pattern the vertex shader uses for `normal_transform`; no transpose is
            // involved in either, and inserting one would mirror the reflection left-to-right.
            //
            // Still unit-length afterwards: the view matrix is rigid, so its 3x3 is orthonormal.
            chrome_normal = mul(shade.bumped_normal,
                                float3x3(draw.view_rotation[0].xyz, draw.view_rotation[1].xyz,
                                         draw.view_rotation[2].xyz));
            // Only where the shade actually ran: `view_rotation` is written on the same branch of
            // BuildDrawRecord that sets kLightSum, and is zero otherwise - which would collapse
            // every generated coordinate onto one texel.
            chrome_texgen = chrome && pc.frame[0].chrome_texgen != 0.0;
        }
    }
    // With every stage disabled the fixed function outputs the diffuse colour alone. That is
    // 2,160 draws a frame on level01 and used to sample whatever texture happened to still be
    // bound at stage 0, because the old path keyed off the binding rather than off COLOROP.
    float4 current = diffuse;

    // How much of the surface the FOG OF WAR leaves visible, 1 fully discovered and 0 fully
    // hidden. It is not a knob and not a second sampler: the fog is a texture stage, and this is
    // read out of the cascade as it runs (§4.51 measured every stage-1 configuration in a level02
    // frame into three groups - the fog, the chrome pass, and none - so `BLENDTEXTUREALPHA` with
    // `(TEXTURE, CURRENT)` names the fog and nothing else). The grid is `D3DFMT_A8`, so the
    // stage is `lerp(current, black, tex.a)` and `1 - tex.a` is exactly what survived.
    //
    // What it is FOR is below, at the specular add: the lighting map's highlight is added after
    // the stages, so without this it is the one part of the surface the fog cannot hide, and a
    // player sees unexplored ground picked out in gloss.
    float fog_visibility = 1.0;

    for (uint i = 0u; i < material.stage_count && i < 2u; ++i) {
        const Stage s = stage_at(material, i);
        // The chrome pass's reflect layer, which is always stage 1 - the unit's own texture is
        // stage 0 and is what the lighting map above was keyed on.
        const bool chrome_stage = chrome && i == 1u;
        // Texture coordinate set, from D3DTSS_TEXCOORDINDEX. The canonical vertex carries two,
        // which is every set the game's FVFs declare; anything else falls back to the first.
        float2 uv = ((s.color >> 24) & 0xffu) == 1u ? input.uv.zw : input.uv.xy;
        if (chrome_stage && chrome_texgen) {
            // The sphere-map coordinate, from the BUMPED normal - which is the whole point: the
            // engine's UV1 is fixed to the surface and cannot answer to a height field.
            //
            // The formula is Gunlok's own. Its map-wide chrome variant asks D3D for
            // D3DTSS_TCI_CAMERASPACENORMAL with no texture transform, which is literally the
            // camera-space normal's xy - so this is what the engine already does for that path,
            // per pixel instead of per vertex. The *0.5+0.5 is ours: D3D's raw form runs -1..1 and
            // leans on the sampler's address mode for the half outside the unit square, which is
            // a property of whatever `units\reflect.rim` was acquired with rather than a decision.
            uv = chrome_normal.xy * 0.5 + 0.5;
        }
        float4 tex = float4(1, 1, 1, 1);
        if (s.texture != kNoTexture) {
            // Roughness widens the reflection by fetching a coarser mip. B already means the
            // highlight's sharpness; a rough surface reflecting less sharply is the same statement
            // made in the sphere map, and a mip is the cheapest honest blur available here.
            const float chrome_lod = chrome_stage ? saturate(chrome_roughness) * pc.frame[0].chrome_blur : 0.0;
            // SampleLevel and not Sample with a bias: the probe has to REPLACE the computed LOD,
            // not nudge it, or it cannot answer which level the hardware would have chosen.
            tex = pc.frame[0].force_lod >= 0.0
                      ? textures[s.texture].SampleLevel(samplers[s.sampler_index], uv,
                                                        pc.frame[0].force_lod)
                      : (chrome_lod > 0.0
                             ? textures[s.texture].SampleBias(samplers[s.sampler_index], uv,
                                                              chrome_lod)
                             : textures[s.texture].Sample(samplers[s.sampler_index], uv));
        }

        // The fog of war, read as it goes past: `BLENDTEXTUREALPHA(TEXTURE, CURRENT)` on an
        // alpha-only grid. The args are compared whole rather than masked with D3DTA_SELECTMASK,
        // so a complemented or alpha-replicated argument does NOT match and leaves this at 1 -
        // the fog stage names neither, and treating some other blend as the fog would mask a
        // highlight that nothing is hiding.
        if ((s.color & 0xffu) == 13u && ((s.color >> 8) & 0xffu) == 2u &&
            ((s.color >> 16) & 0xffu) == 1u) {
            fog_visibility *= 1.0 - tex.a;
        }

        // Colour and alpha both read the state from BEFORE this stage, so `current` is
        // replaced once, at the end - not updated between the two.
        const float3 rgb =
            apply_op(s.color & 0xffu, select_arg((s.color >> 8) & 0xffu, tex, diffuse, current),
                     select_arg((s.color >> 16) & 0xffu, tex, diffuse, current), tex, diffuse,
                     current)
                .rgb;
        const float a =
            apply_op(s.alpha & 0xffu, select_arg((s.alpha >> 8) & 0xffu, tex, diffuse, current),
                     select_arg((s.alpha >> 16) & 0xffu, tex, diffuse, current), tex, diffuse,
                     current)
                .a;
        // The metallic channel scales the chrome stage's RESULT, not its argument. Scaling the
        // argument looks equivalent and is not: this stage is ADDSIGNED, `current + (tex - 0.5)`,
        // so a zeroed argument darkens the surface by half instead of leaving it alone. Blending
        // between the value before the stage and the value after it degenerates exactly at 0 and
        // is bit-identical at 1, whatever the op - which also means this does not hard-code
        // ADDSIGNED, and keeps working if some other material reaches the path.
        const float weight = chrome_stage ? saturate(chrome_metallic * pc.frame[0].chrome_scale) : 1.0;
        current = lerp(current, float4(rgb, a), weight);
    }

    // D3D's alpha test compares the 0..255 reference against the fragment's alpha, so the
    // comparison is done in those units rather than normalising the reference - `>= 31/255`
    // and `>= 31` differ by a rounding step, and the test is exactly where that shows.
    const uint alpha_func = material.flags & 0x0fu;
    if (alpha_func != 0u) {
        const float value = current.a * 255.0;
        const float reference = float((material.flags >> 8) & 0xffu);
        bool pass;
        switch (alpha_func) {
        case 1u: pass = false; break;                   // D3DCMP_NEVER
        case 2u: pass = value <  reference; break;      // LESS
        case 3u: pass = value == reference; break;      // EQUAL
        case 4u: pass = value <= reference; break;      // LESSEQUAL
        case 5u: pass = value >  reference; break;      // GREATER
        case 6u: pass = value != reference; break;      // NOTEQUAL
        case 7u: pass = value >= reference; break;      // GREATEREQUAL
        default: pass = true; break;                    // ALWAYS
        }
        if (!pass) {
            discard;
        }
    }

    // D3DRS_SPECULARENABLE adds the second vertex colour AFTER the texture stages, which is why
    // it rides its own varying instead of being folded into the diffuse. Zero unless the vertex
    // shader was asked for it, so this costs an add on draws that do not use it - level01 uses
    // none, which is how GKPLUS_NO_SPECULAR ruled it out as a gap (§4.20).
    // ... and the lighting map's highlight rides beside it, in the same place and for the same
    // reason: it is a specular term, so it is added after the stages rather than modulated by
    // them, and after the alpha test so it cannot change which fragments survive.
    //
    // **But it is masked by the fog of war, where D3D's own specular would not be.** That is not
    // an inconsistency: `specular_in` is the fixed function's, and reproducing it means adding it
    // exactly where D3D does - after every stage, fog included. The highlight is OURS, and the
    // fog stage is a visibility mask rather than shading, so a term the game never authored has
    // no business being the one thing that survives it. Left unmasked it draws the unexplored
    // parts of a level in gloss, which is what a play report described as objects shining
    // through the fog of war.
    //
    // `fog_visibility` is 1 on every draw with no fog stage - which is every unit, every effect
    // and the whole HUD - so this multiply changes nothing outside the map's own geometry, and
    // nothing at all on a level whose fog is off.
    const float4 lit =
        float4(current.rgb + specular_in + map_specular * fog_visibility, current.a);

    // The material override's tint, last: after the stages, after the alpha test and after the
    // specular add. It is the only thing in this shader the game did not ask for, and it is
    // deliberately not part of the fixed-function cascade - a mod tinting a surface should not
    // have to know which stage its colour would have survived. Multiplying by an unpacked
    // 0xffffffff is exactly 1.0 per channel, so the un-overridden path is bit-identical.
    const float4 tint = float4(uint4(material.tint, material.tint >> 8, material.tint >> 16,
                                     material.tint >> 24) & 0xffu) * (1.0 / 255.0);
    // The AO debug view, after everything, so what it shows is the pass's output and not the
    // pass's output as some later stage happened to modulate it. It ignores `kAoMapOnly` on
    // purpose: the buffer covers every opaque draw the prepass rasterised, and seeing what is in
    // it - including the half the restriction is about to throw away - is the point of looking.
    if ((pc.frame[0].ao_flags & kAoDebug) != 0u && pc.frame[0].ao_texture != kNoTexture) {
        const float value = textures[pc.frame[0].ao_texture].Load(int3(int2(input.position.xy), 0)).x;
        const float3 grey = float3(value, value, value);
        // Through the same decode as everything else, so what the tonemap re-encodes is the value
        // the prepass wrote rather than a curve of it.
        return float4((pc.frame[0].colour_flags & kLinearInput) != 0u ? srgb_decode(grey) : grey,
                      lit.a);
    }
    // --- and out of the fixed function's domain, once, at the very end -------------------------
    //
    // **This single line is what `render.linear_input` now is** (§4.97). Everything above it - the
    // light sum, the material terms, the texture cascade, the specular add, the override's tint -
    // has run exactly as Gunlok's fixed function runs it, on the values the game authored, in the
    // domain they were authored in. What comes out is display-referred, and this turns it into
    // light so that the framebuffer blend and the tonemap can work on light.
    //
    // The reason it is one line and not a decode at each input is the whole of §4.94-§4.97: this
    // game's shading was balanced by people watching a display-referred renderer, and there is no
    // *partial* decode of it that preserves what they balanced. Decoding the albedos crushed the
    // chrome pass, decoding the light colours flattened every light's falloff, and each fix only
    // exposed the next term still in the other domain. The arithmetic says why linear lighting was
    // never going to pay here either: `encode(decode(a) * decode(S))` is exactly `a * S` for a
    // power law, so a light multiplying a surface is the *same* in both domains. Only sums differ,
    // and every sum in this game was balanced in the other one.
    //
    // What is left is what HDR is actually for: the **blend** is linear, so additive fires, flares
    // and muzzle flashes accumulate past 1 instead of clipping, and the tonemap gets a real
    // over-range to compress. An opaque draw round-trips `encode(decode(x)) == x` and comes out
    // bit-exact against the stock renderer.
    const float4 output_colour = lit * tint;
    if ((pc.frame[0].colour_flags & kLinearInput) == 0u) {
        return output_colour;
    }
    return float4(srgb_decode(output_colour.rgb), output_colour.a);
}
