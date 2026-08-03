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
    uint light_offset;
    uint light_count;
    uint lighting;
    uint lit_colour;
};

// The two texture stages, flat rather than an array, for the same layout reason the vertex is
// three float4s: an array of scalars is padded to a 16-byte stride under std140 and is not
// under scalar rules, so `uint stage_texture[2]` would be a bet on which one Slang picked.
// Eight named scalars cannot be laid out two ways.
//
// `color` and `alpha` each pack one D3D texture-stage operation:
//     bits  0..7   D3DTEXTUREOP
//     bits  8..15  the first argument  (D3DTA_*, complement and alpha-replicate included)
//     bits 16..23  the second argument
//     bits 24..31  D3DTSS_TEXCOORDINDEX, meaningful on the colour word only
//
// 72 bytes of a guaranteed 128 now that the matrices and the lighting are in the draw record -
// it used to be 120, which is why a light array could not have gone here (§4.19, §4.26).
struct Push {
    ConstBufferPointer<Vertex> vertices;
    ConstBufferPointer<GpuDrawRecord> draws;
    ConstBufferPointer<GpuLight> lights;
    uint record;         // this draw's entry in `draws`
    uint base_vertex;    // where this draw's buffer starts, in canonical vertices
    uint stage_count;    // 0, 1 or 2 - stages up to the first D3DTOP_DISABLE
    // The alpha test: D3DCMPFUNC in bits 0..3, 0 meaning the test is off, and D3DRS_ALPHAREF
    // in bits 8..15. A `discard` and not pipeline state, because it varies per draw with the
    // same blend and depth settings - making it part of the pipeline key would multiply the
    // pipeline count by the number of distinct references for nothing.
    uint flags;
    uint stage0_texture; // bindless slot, 0xffffffff for untextured
    uint stage0_sampler;
    uint stage0_color;
    uint stage0_alpha;
    uint stage1_texture;
    uint stage1_sampler;
    uint stage1_color;
    uint stage1_alpha;
};

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

struct VertexOut {
    float4 position : SV_Position;
    float4 color : COLOR;
    float4 specular : COLOR1; // D3D's second output colour, added after the texture stages
    float4 uv : TEXCOORD0;    // xy = uv0, zw = uv1 - stage 1 is the lightmap set
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
//     specular   = Cs * SUM k*L.specular*pow(max(0, N.H), power)
//
// with k = attenuation * spot per light. The C* are the material colours, each of which the
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

[shader("vertex")]
VertexOut vertex_main(uint vertex_id: SV_VertexID) {
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

    const bool want_specular = (draw.lighting & kSpecularEnable) != 0u;
    const float3 to_eye = want_specular ? normalize(draw.eye.xyz - world_position) : float3(0, 0, 1);
    const float power = draw.material_specular.w;

    float3 ambient_sum = draw.global_ambient.rgb;
    float3 diffuse_sum = float3(0, 0, 0);
    float3 specular_sum = float3(0, 0, 0);

    for (uint i = 0u; i < draw.light_count; ++i) {
        const GpuLight light = pc.lights[draw.light_offset + i];
        const float type = light.spot.w;

        float3 to_light;
        float k = 1.0;
        if (type >= 2.5) {
            // Directional: no position, no attenuation. The CPU has normalised the direction.
            to_light = -light.direction.xyz;
        } else {
            const float3 delta = light.position.xyz - world_position;
            const float distance = length(delta);
            if (distance > light.attenuation.w) {
                continue; // outside Range, which switches the light off entirely
            }
            to_light = distance > 0.0 ? delta / distance : float3(0, 0, 1);
            const float denominator = light.attenuation.x + light.attenuation.y * distance +
                                      light.attenuation.z * distance * distance;
            k = denominator > 0.0 ? 1.0 / denominator : 1.0;
            if (type >= 1.5) {
                // Spot. rho is measured along the cone axis, so it needs the direction from the
                // light to the vertex - the opposite of the vector the diffuse term wants.
                const float rho = dot(light.direction.xyz, -to_light);
                const float cos_theta = light.spot.x;
                const float cos_phi = light.spot.y;
                if (rho <= cos_phi) {
                    continue;
                }
                if (rho < cos_theta) {
                    k *= pow(saturate((rho - cos_phi) / max(cos_theta - cos_phi, 1e-6)),
                             light.spot.z);
                }
            }
        }

        const float lambert = max(0.0, dot(normal, to_light));
        ambient_sum += k * light.ambient.rgb;
        diffuse_sum += k * light.diffuse.rgb * lambert;
        if (want_specular) {
            const float3 halfway = normalize(to_light + to_eye);
            specular_sum +=
                k * light.specular.rgb * pow(max(0.0, dot(normal, halfway)), max(power, 1e-4));
        }
    }

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

    // Saturated because the fixed function's output is a D3DCOLOR: the clamp is where the
    // pipeline is, not an approximation of it.
    output.color = float4(saturate(emissive.rgb + ambient.rgb * ambient_sum +
                                   diffuse.rgb * diffuse_sum),
                          diffuse.a);
    if (want_specular) {
        output.specular = float4(saturate(specular.rgb * specular_sum), 0.0);
    }
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

Stage stage_at(uint index) {
    Stage s;
    if (index == 0u) {
        s.texture = pc.stage0_texture;
        s.sampler_index = pc.stage0_sampler;
        s.color = pc.stage0_color;
        s.alpha = pc.stage0_alpha;
    } else {
        s.texture = pc.stage1_texture;
        s.sampler_index = pc.stage1_sampler;
        s.color = pc.stage1_color;
        s.alpha = pc.stage1_alpha;
    }
    return s;
}

[shader("fragment")]
float4 fragment_main(VertexOut input) : SV_Target {
    // Already the lit colour: the vertex shader ran D3D's lighting equation and what comes out
    // of it IS the diffuse the texture stages take as D3DTA_DIFFUSE. That is also D3D's own
    // order - lighting produces a vertex colour, the multitexture cascade consumes it - so
    // nothing here has to know whether the draw was lit.
    const float4 diffuse = input.color;
    // With every stage disabled the fixed function outputs the diffuse colour alone. That is
    // 2,160 draws a frame on level01 and used to sample whatever texture happened to still be
    // bound at stage 0, because the old path keyed off the binding rather than off COLOROP.
    float4 current = diffuse;

    for (uint i = 0u; i < pc.stage_count && i < 2u; ++i) {
        const Stage s = stage_at(i);
        // Texture coordinate set, from D3DTSS_TEXCOORDINDEX. The canonical vertex carries two,
        // which is every set the game's FVFs declare; anything else falls back to the first.
        const float2 uv = ((s.color >> 24) & 0xffu) == 1u ? input.uv.zw : input.uv.xy;
        const float4 tex = s.texture == kNoTexture
                               ? float4(1, 1, 1, 1)
                               : textures[s.texture].Sample(samplers[s.sampler_index], uv);

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
        current = float4(rgb, a);
    }

    // D3D's alpha test compares the 0..255 reference against the fragment's alpha, so the
    // comparison is done in those units rather than normalising the reference - `>= 31/255`
    // and `>= 31` differ by a rounding step, and the test is exactly where that shows.
    const uint alpha_func = pc.flags & 0x0fu;
    if (alpha_func != 0u) {
        const float value = current.a * 255.0;
        const float reference = float((pc.flags >> 8) & 0xffu);
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
    return float4(current.rgb + input.specular.rgb, current.a);
}
