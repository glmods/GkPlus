// Screen-space ambient occlusion, without a blur pass.
//
// The technique is the one in https://www.youtube.com/watch?v=vJU1PgGdH3k, and its whole idea is
// to **reverse the sampling**. The textbook kernel generates 3D offsets around the fragment,
// projects each one onto the depth buffer and compares; the offsets are randomised per pixel so
// the pattern does not band, which makes the result noise and forces a blur to follow it. Here the
// offsets are generated in **2D**, in screen space, from one fixed disc shared by every pixel, and
// what is reconstructed is the 3D position *of that pixel*. The hemisphere test then runs on real
// positions rather than on a depth comparison.
//
// Two things follow, and they are the reason this is worth having:
//
//   - **The output is not noise**, so there is no blur pass, so there is no halo around a
//     silhouette and no bleed across a depth discontinuity. Gunlok renders 640x480 (notes §4.37),
//     where a blur's radius is a large fraction of a character, so this matters more here than it
//     would at a modern resolution.
//   - Every pixel walks the **same** texel pattern, which is the friendliest possible case for the
//     texture cache - the video's own argument, and the reason the disc is not rotated per pixel.
//
// **No matrices at all.** The prepass writes WORLD POSITION per pixel rather than a depth, so the
// resolve reconstructs nothing: a tap is one load and the hemisphere test is a subtraction and two
// dot products. That is not a micro-optimisation, it is what makes the pass possible here - the
// view and projection are folded into each draw's `mvp` on this side and are not available as a
// per-frame quantity (see BuildSunCascades in src/VkDraw.cpp, which says the same thing about the
// frustum). The world-space light grid was chosen for the same reason (lightgrid.slang).
//
// The cost of writing positions rather than depth is 16 bytes a pixel instead of 4 - 4.9 MB at
// 640x480, against the shadow atlas's 66 MB. Bought without hesitation.

// Must match `Vertex` in world.slang and vulkan::CanonicalVertex in src/VertexFormat.h.
struct Vertex {
    float4 pos;
    float4 normal_and_color;
    float4 uv;
};

// Must match GpuDrawRecord in src/VkDraw.h. Declared in full even though the prepass reads only
// `mvp`, `world` and `normal_transform`: the struct is an array element, so a short declaration
// gets the stride wrong and every draw past the first reads another draw's matrix - the same trap
// shadow.slang's copy carries the same warning about.
struct GpuDrawRecord {
    float4 mvp[4];
    float4 world[4];
    float4 normal_transform[3];
    float4 material_ambient;
    float4 material_diffuse;
    float4 material_specular;
    float4 material_emissive;
    float4 global_ambient;
    float4 eye;
    float4 view_rotation[3];
    uint light_offset;
    uint light_count;
    uint lighting;
    uint lit_colour;
};

// One block for both passes, because a pipeline layout is per pass and two blocks would be two
// layouts for no gain. The prepass reads the first four fields and the resolve reads the rest.
struct AoPush {
    ConstBufferPointer<Vertex> vertices;
    ConstBufferPointer<GpuDrawRecord> draws;
    uint record;      // the prepass only
    uint base_vertex; // ... and so is this
    // The resolve's two inputs, as bindless indices - the same array the world pass samples its
    // textures out of, so this pass needs no descriptor set of its own beyond the one that
    // already exists.
    uint position_texture;
    uint normal_texture;
    // The hemisphere's radius, in WORLD units. What "near enough to occlude" means.
    float radius;
    // The disc's radius, in PIXELS. Deliberately independent of the one above and deliberately
    // constant across the frame: that constancy is the technique's whole performance argument, and
    // Gunlok's camera - a fixed-height orbit - has a narrow enough depth spread across one frame
    // that a constant screen radius is very nearly a constant world radius anyway.
    float screen_radius;
    // How far along the normal a tap must be before it counts, in world units. Kills the
    // self-occlusion a flat surface would otherwise show from its own quantisation.
    float bias;
    // Scales the occlusion before it leaves this pass, so the resolve target already carries the
    // artistic weight and the world shader is a plain multiply.
    float strength;
    uint taps;         // how many of kDisc to walk, clamped to its length on the CPU
    float pad0;
};

[[vk::push_constant]]
ConstantBuffer<AoPush> pc;

// The bindless set, exactly as world.slang declares it - same bindings, same layout object.
[[vk::binding(0, 0)]] SamplerState samplers[];
[[vk::binding(1, 0)]] Texture2D textures[];

static const uint kLightSum = 0x01u;

// --- the prepass ------------------------------------------------------------------------------
//
// The same geometry the world pass is about to draw, rasterised with **each draw's own `mvp`** -
// so this is the camera's view by construction, with no camera matrix anywhere on this side. What
// comes out is a world position and a world normal per visible pixel.

struct PrepassOut {
    float4 position : SV_Position;
    float3 world_position : TEXCOORD0;
    float3 world_normal : TEXCOORD1;
};

struct PrepassTargets {
    // xyz world position, w = 1 to mark the pixel as covered. The target is cleared to zero, so
    // `w == 0` is "nothing was drawn here" and the resolve leaves such a pixel unoccluded.
    float4 position : SV_Target0;
    // The normal, biased into [0,1]. 8 bits a channel is ample for a hemisphere test whose only
    // output is a count.
    float4 normal : SV_Target1;
};

[shader("vertex")]
PrepassOut ao_prepass_vertex(uint vertex_id: SV_VertexID) {
    const Vertex v = pc.vertices[pc.base_vertex + vertex_id];
    const GpuDrawRecord draw = pc.draws[pc.record];

    PrepassOut output;
    const float4 local = float4(v.pos.xyz, 1.0);
    output.position = mul(local, float4x4(draw.mvp[0], draw.mvp[1], draw.mvp[2], draw.mvp[3]));
    // A draw with no world position at all - a pre-transformed HUD quad, whose `world` matrix is
    // not a world matrix. Pushed behind the far plane rather than discarded in the fragment stage,
    // which is what shadow.slang does with the same test and for the same reason. The CPU cannot
    // make this test: the flags live in the record.
    if ((draw.lighting & kLightSum) == 0u) {
        output.position = float4(0, 0, 2, 1);
    }
    output.world_position =
        mul(local, float4x4(draw.world[0], draw.world[1], draw.world[2], draw.world[3])).xyz;
    // The inverse transpose, so a non-uniform scale in the world matrix does not tilt the normal -
    // the same transform world.slang's `shade_surface` applies, and it has to be the same one or
    // the hemisphere is oriented differently from the surface being shaded.
    output.world_normal = mul(v.normal_and_color.xyz,
                              float3x3(draw.normal_transform[0].xyz, draw.normal_transform[1].xyz,
                                       draw.normal_transform[2].xyz));
    return output;
}

[shader("fragment")]
PrepassTargets ao_prepass_fragment(PrepassOut input) {
    PrepassTargets out;
    out.position = float4(input.world_position, 1.0);
    // Normalised HERE and not in the vertex stage: interpolating two unit normals across a
    // triangle yields a shorter vector everywhere between them, and the hemisphere test's whole
    // input is a direction. This is the same correction the per-pixel light sum makes.
    out.normal = float4(normalize(input.world_normal) * 0.5 + 0.5, 1.0);
    return out;
}

// --- the resolve ------------------------------------------------------------------------------

// One fixed disc, shared by every pixel of every frame. **Not rotated and not jittered per pixel**,
// which is the point: either is what turns this back into noise and brings the blur pass back
// with it.
//
// **The first 32 are the video's own pattern**, read off the plot it shows and divided by the 0.4
// its axes run to. It is worth saying what that pattern is, because it is not what a paper would
// reach for and the difference is the technique: it is a **lattice**, spacing 0.1 on those axes,
// with each point nudged slightly off its cell. Not Poisson, not blue noise, not random. A
// stratified set like this has no clumps and no holes *by construction*, where a blue-noise set
// only has few of them on average - and with one pattern reused by every pixel, "on average" is
// not a property anything gets to rely on. An earlier revision here used Mitchell's
// best-candidate and it was measurably worse at the same count.
//
// The other 32 are the same lattice **half a cell over**, chosen maximin against everything
// already placed, so the whole disc is that construction at twice the density rather than a
// second pattern bolted beside the first. Minimum separation 0.160 against the video's own 0.182.
//
// **`taps` is the one lever that matters, and 32 is exactly the video's set.** A fixed kernel
// cannot trade its artefact for noise the way a randomised one does, so what an under-sampled disc
// produces is not grain but *structure*: each tap contributes a shifted copy of every occluder's
// silhouette. Measured on level02 at a 250-pixel disc, where the two characters each left a fan of
// their own outlines. The ghosts' spacing is the minimum separation times the disc's radius, which
// is why density is the fix and why the video's pattern is as dense as it is.
static const uint kDiscSize = 64u;
static const float2 kDisc[kDiscSize] = {
    // The video's 32, normalised, and **maximin-ordered from the centre tap** - so `taps = 32` is
    // exactly its set and any smaller prefix is still a well-spread subset of it rather than a
    // corner of the plot. An 8-tap prefix has minimum separation 0.530 and still reaches the rim.
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
    // The infill: the same lattice, half a cell over, maximin against the above.
    float2(+0.125000, -0.375000), float2(-0.625000, +0.375000),
    float2(-0.875000, -0.375000), float2(-0.375000, -0.875000),
    float2(-0.375000, +0.875000), float2(+0.375000, -0.875000),
    float2(+0.875000, -0.375000), float2(+0.875000, +0.375000),
    float2(-0.375000, +0.125000), float2(-0.125000, -0.125000),
    float2(-0.125000, +0.125000), float2(-0.125000, +0.375000),
    float2(+0.125000, +0.125000), float2(+0.125000, +0.375000),
    float2(+0.375000, +0.125000), float2(-0.625000, -0.375000),
    float2(-0.625000, -0.125000), float2(-0.375000, -0.625000),
    float2(-0.375000, +0.625000), float2(-0.125000, +0.625000),
    float2(+0.125000, -0.625000), float2(+0.125000, +0.625000),
    float2(+0.375000, -0.625000), float2(+0.375000, +0.625000),
    float2(+0.625000, -0.625000), float2(+0.625000, -0.375000),
    float2(+0.625000, +0.125000), float2(+0.375000, +0.875000),
    float2(-0.125000, -0.625000), float2(-0.125000, -0.375000),
    float2(+0.625000, -0.125000), float2(+0.625000, +0.375000),
};

// The full-screen triangle. Three vertices, no vertex buffer and no uv output - the fragment
// shader wants integer pixels and `SV_Position` already is them.
[shader("vertex")]
float4 ao_fullscreen_vertex(uint vertex_id: SV_VertexID) : SV_Position {
    // (0,0) (2,0) (0,2) in uv, which is (-1,-1) (3,-1) (-1,3) in clip - one triangle covering the
    // target with no diagonal seam down the middle.
    const float2 uv = float2((vertex_id << 1u) & 2u, vertex_id & 2u);
    return float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

[shader("fragment")]
float ao_resolve_fragment(float4 position: SV_Position) : SV_Target {
    const int2 pixel = int2(position.xy);
    const float4 centre = textures[pc.position_texture].Load(int3(pixel, 0));
    // Nothing was drawn here - the sky, or a pixel only a pre-transformed draw covers. Unoccluded,
    // and it must be exactly 1 rather than 0: this target is a multiplier.
    if (centre.w == 0.0) {
        return 1.0;
    }
    const float3 origin = centre.xyz;
    const float3 normal =
        normalize(textures[pc.normal_texture].Load(int3(pixel, 0)).xyz * 2.0 - 1.0);

    const float radius_squared = pc.radius * pc.radius;
    const uint taps = min(pc.taps, kDiscSize);
    float occluded = 0.0;
    for (uint i = 0u; i < taps; ++i) {
        const int2 tap = pixel + int2(kDisc[i] * pc.screen_radius);
        // Load clamps out-of-bounds to zero on every implementation that matters, and a zero
        // texel has `w == 0`, which the test below rejects - so the frame's border needs no
        // special case and no clamp of its own.
        const float4 sample = textures[pc.position_texture].Load(int3(tap, 0));
        if (sample.w == 0.0) {
            continue;
        }
        const float3 delta = sample.xyz - origin;
        // **The hemisphere is a bounded volume, and that is the range check.** A tap on a distant
        // background surface projects into the disc but falls outside the half-ball, so it does
        // not count - which is the halo that a plain depth comparison has to add a range falloff
        // to suppress. Here it costs nothing because the test was always a containment test.
        if (dot(delta, delta) >= radius_squared) {
            continue;
        }
        // ... and the other half of the containment: in FRONT of the surface. `bias` lifts the
        // plane off the surface so a flat wall does not occlude itself out of its own
        // quantisation.
        if (dot(delta, normal) <= pc.bias) {
            continue;
        }
        occluded += 1.0;
    }

    const float occlusion = taps > 0u ? occluded / float(taps) : 0.0;
    return saturate(1.0 - occlusion * pc.strength);
}
