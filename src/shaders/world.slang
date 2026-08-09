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
    uint pad0;
    uint pad1;
    uint pad2;
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
    bool valid;
};

LightGeometry light_geometry(GpuLight light, float3 world_position) {
    LightGeometry out;
    out.to_light = float3(0, 0, 1);
    out.k = 1.0;
    out.valid = true;

    const float type = light.spot.w;
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
LightSums light_sum(GpuDrawRecord draw, float3 world_position, float3 normal, bool want_specular,
                    float3 to_eye, float power) {
    LightSums out;
    out.ambient = draw.global_ambient.rgb;
    out.diffuse = float3(0, 0, 0);
    out.specular = float3(0, 0, 0);

    for (uint i = 0u; i < draw.light_count; ++i) {
        const GpuLight light = pc.lights[draw.light_offset + i];
        const LightGeometry geometry = light_geometry(light, world_position);
        if (!geometry.valid) {
            continue;
        }
        const float3 to_light = geometry.to_light;
        const float k = geometry.k;

        const float lambert = max(0.0, dot(normal, to_light));
        out.ambient += k * light.ambient.rgb;
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
    out.color = float4(saturate(emissive.rgb + ambient.rgb * sums.ambient +
                                diffuse.rgb * sums.diffuse),
                       diffuse.a);
    out.specular = want_specular ? saturate(specular.rgb * sums.specular) : float3(0, 0, 0);
    return out;
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

float3 map_light_sum(float3 world_position, float3 normal) {
    float3 acc = float3(0, 0, 0);
    const uint2 span = map_light_range(world_position);
    const bool culled = (pc.frame[0].map_flags & kMapLightingGrid) != 0u;
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
        float atten = (1.0 - distance / range) * max(0.0, dot(normal, to_light));
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
        acc += light.colour.rgb * atten;
    }
    // AMBIENCE is a per-channel max(), not an added term - AvP's own renderer clamps upward
    // against it, and adding it instead would lift a fully-lit surface past white.
    return max(saturate(acc * pc.frame[0].map_light_gain), float3(pc.frame[0].map_ambience, pc.frame[0].map_ambience,
                                                        pc.frame[0].map_ambience));
}

// The body, with the flat copies left to the entry point below. Two exits, and a third would be
// easy to add - so nothing here writes `color_flat`.
VertexOut shade_vertex(uint vertex_id) {
    Vertex v = pc.vertices[pc.base_vertex + vertex_id];
    GpuDrawRecord draw = pc.draws[pc.record];

    VertexOut output;
    // No branch on pre-transformed vertices on purpose: the CPU puts either world*view*proj or
    // a pixels-to-clip orthographic matrix in `mvp`, so both FVF families are one code path. A
    // branch would have had to know the viewport, which the shader does not.
    //
    // `mul(v, M)` and not `mul(M, v)`: that is D3D's row-vector convention, and combined with
    // -matrix-layout-row-major it means the CPU can upload world*view*proj exactly as the game
    // composes it, with no transpose step to get backwards.
    const float4 local = float4(v.pos.xyz, 1.0);
    output.position =
        mul(local, rows_to_matrix(draw.mvp[0], draw.mvp[1], draw.mvp[2], draw.mvp[3]));
    output.uv = v.uv;
    output.specular = float4(0, 0, 0, 0);
    // Written before either exit below, because Slang requires every output to be assigned on
    // every path and the fragment shader reads these only on the lit one.
    output.world_position = float3(0, 0, 0);
    output.world_normal = float3(0, 0, 1);

    const float4 vertex_colour = unpack_colour(asuint(v.normal_and_color.w));
    if ((draw.lighting & kLightSum) == 0u) {
        // Two non-lighting paths. kLitCollapse is the previous build's behaviour, kept so the
        // two can be compared on one paused frame rather than across two launches - which is
        // the only comparison with a noise floor small enough to see this (§4.21). Neither set
        // means lighting is off, and D3D's lighting-off result is the vertex colour itself.
        output.color = (draw.lighting & kLitCollapse) != 0u ? unpack_colour(draw.lit_colour)
                                                            : vertex_colour;
        return output;
    }

    const float3 world_position =
        mul(local, rows_to_matrix(draw.world[0], draw.world[1], draw.world[2], draw.world[3]))
            .xyz;
    // The inverse transpose, so a non-uniform scale in the world matrix does not tilt the
    // normal. Normalised only under D3DRS_NORMALIZENORMALS, because D3D does not do it
    // otherwise and the resulting length change is a real difference in brightness.
    float3 normal = mul(v.normal_and_color.xyz,
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
    const LightSums sums = light_sum(draw, world_position, normal, want_specular, to_eye,
                                     draw.material_specular.w);
    const LitColour lit = resolve_lit_colour(draw, sums, vertex_colour, want_specular);
    output.color = lit.color;
    output.specular = float4(lit.specular, 0.0);
    return output;
}

[shader("vertex")]
VertexOut vertex_main(uint vertex_id: SV_VertexID) {
    VertexOut output = shade_vertex(vertex_id);
    // The same values, differing only in how they are interpolated. Mirrored here, at the one
    // exit there is, rather than at each exit of the body above - which is the whole reason the
    // body is a separate function.
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
        const LightGeometry geometry = light_geometry(light, world_position);
        if (!geometry.valid) {
            continue;
        }
        ambient_sum += geometry.k * light.ambient.rgb;
        const float lambert = max(0.0, dot(normal, geometry.to_light));
        const float lambert_bumped = max(0.0, dot(bumped, geometry.to_light));
        diffuse_flat += geometry.k * light.diffuse.rgb * lambert;
        diffuse_bumped += geometry.k * light.diffuse.rgb * lambert_bumped;
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
            specular_sum += geometry.k * colour * pow(max(0.0, dot(bumped, halfway)), exponent);
        }
    }

    out.specular = specular_sum * metallic * pc.frame[0].specular_scale;
    // Clamped to 4x: where the geometric normal faces away from every light the denominator is
    // just the ambient term, and an unbounded ratio would make a bumped pixel there many times
    // brighter than anything around it.
    const float3 ratio = clamp((ambient_sum + diffuse_bumped) /
                                   max(ambient_sum + diffuse_flat, float3(1e-3, 1e-3, 1e-3)),
                               float3(0, 0, 0), float3(4, 4, 4));
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
        if ((pc.frame[0].map_flags & kMapLightingOn) != 0u &&
            (map_geometry || (pc.frame[0].map_flags & kMapLightingAll) != 0u)) {
            diffuse = float4(map_light_sum(input.world_position, normal), diffuse.a);
        }
        const bool want_specular = (draw.lighting & kSpecularEnable) != 0u;
        // Per fragment, which is what D3DRS_LOCALVIEWER means taken literally. The vertex path
        // computes this once per vertex and interpolates the result of the whole highlight.
        const float3 to_eye =
            want_specular ? normalize(draw.eye.xyz - input.world_position) : float3(0, 0, 1);
        const LightSums sums = light_sum(draw, input.world_position, normal, want_specular,
                                         to_eye, draw.material_specular.w);
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
    const float4 lit = float4(current.rgb + specular_in + map_specular, current.a);

    // The material override's tint, last: after the stages, after the alpha test and after the
    // specular add. It is the only thing in this shader the game did not ask for, and it is
    // deliberately not part of the fixed-function cascade - a mod tinting a surface should not
    // have to know which stage its colour would have survived. Multiplying by an unpacked
    // 0xffffffff is exactly 1.0 per channel, so the un-overridden path is bit-identical.
    const float4 tint = float4(uint4(material.tint, material.tint >> 8, material.tint >> 16,
                                     material.tint >> 24) & 0xffu) * (1.0 / 255.0);
    return lit * tint;
}
