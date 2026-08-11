// The sun's shadow map: a depth-only pass over the same draw list the world pass walks.
//
// Vertex only - there is no fragment stage at all, which is legal for a pipeline with no colour
// attachment and is what makes this pass nearly free per fragment.
//
// It reads the SAME `GpuDrawRecord` array the world pass does, which is the whole point of §2's
// design: a draw here is an index into shared per-frame state rather than a bundle of its own, so
// a second walk over the frame costs a pipeline and a push block and no new data at all.

// Must match `Vertex` in world.slang and vulkan::CanonicalVertex in src/VertexFormat.h.
struct Vertex {
    float4 pos;
    float4 normal_and_color;
    float4 uv;
};

// Only the first 20 float4s of GpuDrawRecord are read here - `world` is the second group - but the
// struct has to be declared in full or the stride is wrong and every draw past the first reads
// the wrong matrix. That is the same trap the arena's slot alignment was (§4.16): addressing an
// array wrongly yields *other data* rather than garbage.
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

// One block for both entry points below, because a stage may only have one - so the fields the
// direct path uses (`record`, `base_vertex`) and the one the indirect path uses (`params`) sit
// side by side and each entry point reads its own. It is 96 bytes either way: three addresses,
// two words and a matrix.
struct ShadowPush {
    ConstBufferPointer<Vertex> vertices;
    ConstBufferPointer<GpuDrawRecord> draws;
    // **The indirect path only.** {record, base_vertex} per command, parallel to the indirect
    // buffer and indexed by SV_DrawIndex - the two things that vary per draw and therefore cannot
    // ride in a push when one command draws a batch (§4.62). Null on the direct path.
    ConstBufferPointer<uint2> params;
    uint record;      // the direct path only
    uint base_vertex; // ... and so is this
    // World -> this light's clip space, which is the one thing uniform over an indirect batch.
    // Row-vector, like every other matrix on this side.
    float4 light_matrix[4];
    // The PN-triangle pass (§4.71). Only the two that shape the PATCH are here, and that is the
    // point: the shadow surface has to be the same surface the colour pass draws, and the patch is
    // a function of these two and the control points alone. The tessellation FACTORS need not
    // match - they only decide how finely the same surface is sampled - which is why there is one
    // uniform factor here instead of the world pass's screen-space rule.
    float pn_strength;
    float pn_flat_threshold;
    // ... and the world-unit ceiling on a control point's offset, which shapes the patch too and
    // therefore has to be here for the same reason the other two are. **This was `pad_shadow`**,
    // so the block is still 112.
    float pn_max_offset;
    // Uniform over every edge of every patch, which makes this pass watertight for free: the
    // adaptive rule is what needs care about shared edges, and a constant cannot disagree with
    // itself. A shadow map does not need adaptive density the way a silhouette does.
    float tess_factor;
};

[[vk::push_constant]]
ConstantBuffer<ShadowPush> pc;

static const uint kLightSum = 0x01u;

// The body both entry points share, so the two cannot drift on the one thing that matters: which
// draws cast. `kLightSum` is set only on a 3D draw the game lit, so it is exactly the test for
// "this has a world position at all" - a pre-transformed HUD quad has none and its `world` matrix
// is not a world matrix. Rejecting it here rather than on the CPU is what keeps the walks
// agreeing: the CPU cannot see the flags, which live in the record.
//
// Pushed behind the far plane rather than discarded, because there is no fragment stage to
// discard in.
float4 shadow_position_at(float3 local_pos, GpuDrawRecord draw, float4 light_matrix[4]) {
    if ((draw.lighting & kLightSum) == 0u) {
        return float4(0, 0, 2, 1);
    }
    const float4 local = float4(local_pos, 1.0);
    const float4 world = mul(local, float4x4(draw.world[0], draw.world[1], draw.world[2],
                                             draw.world[3]));
    return mul(float4(world.xyz, 1.0),
               float4x4(light_matrix[0], light_matrix[1], light_matrix[2], light_matrix[3]));
}

float4 shadow_position(Vertex v, GpuDrawRecord draw, float4 light_matrix[4]) {
    return shadow_position_at(v.pos.xyz, draw, light_matrix);
}

// The sun's cascades: one draw call per caster, so the record and the arena slot ride in the push.
[shader("vertex")]
float4 shadow_vertex(uint vertex_id: SV_VertexID) : SV_Position {
    return shadow_position(pc.vertices[pc.base_vertex + vertex_id], pc.draws[pc.record],
                           pc.light_matrix);
}

// The map lights' bake: one indirect command per FACE, drawing every caster. `SV_DrawIndex` is
// which command within that batch this vertex belongs to, so it indexes the parallel parameter
// array the CPU wrote beside the indirect buffer.
//
// It needs `shaderDrawParameters`, which is enabled already - Slang's `SV_VertexID` carries D3D
// semantics and compiles to `gl_VertexIndex - gl_BaseVertex`, so the capability was a hard
// requirement before this existed (see VkContext.cpp). That is what makes `SV_DrawIndex` free
// here, and why the alternative of smuggling the record through `firstInstance` - which would
// need `drawIndirectFirstInstance` and a correction by `SV_StartInstanceLocation`, since
// `SV_InstanceID` is D3D-flavoured the same way - was not worth it.
[shader("vertex")]
float4 map_shadow_vertex(uint vertex_id: SV_VertexID, uint draw_id: SV_DrawIndex) : SV_Position {
    const uint2 param = pc.params[draw_id];
    return shadow_position(pc.vertices[param.y + vertex_id], pc.draws[param.x], pc.light_matrix);
}

// --- the tessellated twins (§4.71) ------------------------------------------------------------
//
// The same PN-triangle patch the world pass builds, so a displaced silhouette casts the shadow it
// actually has rather than the one it had before amplification. The construction is duplicated
// here rather than shared with world.slang because the two files are separate translation units
// to slangc - what is shared, and what has to stay shared, is the two knobs in the push above.
//
// **One hull and one domain serve all four shadow pipelines.** The only thing that differs
// between the direct and indirect paths is where the draw record comes from, and carrying that in
// the control point moves the difference entirely into the vertex stage - which is the same trick
// `shadow_position` already used to keep the two walks agreeing.
struct ShadowControlPoint {
    float3 pos;
    float3 normal;
    // Flat, and read from control point 0 in the domain shader: every vertex of a patch belongs to
    // one draw, so the three copies always agree.
    nointerpolation uint record;
};

struct ShadowPatchConstants {
    float edges[3]  : SV_TessFactor;
    float inner     : SV_InsideTessFactor;
};

[shader("vertex")]
ShadowControlPoint shadow_tess_vertex(uint vertex_id: SV_VertexID) {
    Vertex v = pc.vertices[pc.base_vertex + vertex_id];
    ShadowControlPoint cp;
    cp.pos = v.pos.xyz;
    cp.normal = v.normal_and_color.xyz;
    cp.record = pc.record;
    return cp;
}

[shader("vertex")]
ShadowControlPoint map_shadow_tess_vertex(uint vertex_id: SV_VertexID, uint draw_id: SV_DrawIndex) {
    const uint2 param = pc.params[draw_id];
    Vertex v = pc.vertices[param.y + vertex_id];
    ShadowControlPoint cp;
    cp.pos = v.pos.xyz;
    cp.normal = v.normal_and_color.xyz;
    cp.record = param.x;
    return cp;
}

ShadowPatchConstants shadow_hull_constants(InputPatch<ShadowControlPoint, 3> patch) {
    ShadowPatchConstants out;
    const float factor = max(pc.tess_factor, 1.0);
    out.edges[0] = factor;
    out.edges[1] = factor;
    out.edges[2] = factor;
    out.inner = factor;
    return out;
}

[shader("hull")]
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("shadow_hull_constants")]
ShadowControlPoint shadow_hull(InputPatch<ShadowControlPoint, 3> patch,
                               uint id: SV_OutputControlPointID) {
    return patch[id];
}

// The same tangent term, the same three knobs, the same watertight property: a function of
// (Pi, Pj, Ni) alone. See world.slang's `pn_weight`, which this must agree with exactly - if the
// two ever differ, the shadow is cast by a surface the colour pass does not draw. **The ceiling
// belongs here as much as the floor does**, and leaving it out would be that defect: the colour
// pass would draw a rounded pipe that still cast the shadow of an inflated one.
float shadow_pn_weight(float3 pi, float3 pj, float3 ni) {
    const float3 edge = pj - pi;
    const float length_sq = dot(edge, edge);
    if (length_sq < 1e-18) {
        return 0.0;
    }
    const float w = dot(edge, ni);
    if (w * w <= pc.pn_flat_threshold * pc.pn_flat_threshold * length_sq) {
        return 0.0;
    }
    const float limit = 3.0 * pc.pn_max_offset;
    return clamp(w, -limit, limit) * pc.pn_strength;
}

[shader("domain")]
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
float4 shadow_domain(ShadowPatchConstants constants, float3 bary: SV_DomainLocation,
                     const OutputPatch<ShadowControlPoint, 3> patch) : SV_Position {
    const float3 p1 = patch[0].pos, p2 = patch[1].pos, p3 = patch[2].pos;
    const float3 n1 = normalize(patch[0].normal);
    const float3 n2 = normalize(patch[1].normal);
    const float3 n3 = normalize(patch[2].normal);

    const float3 b210 = (2.0 * p1 + p2 - shadow_pn_weight(p1, p2, n1) * n1) / 3.0;
    const float3 b120 = (2.0 * p2 + p1 - shadow_pn_weight(p2, p1, n2) * n2) / 3.0;
    const float3 b021 = (2.0 * p2 + p3 - shadow_pn_weight(p2, p3, n2) * n2) / 3.0;
    const float3 b012 = (2.0 * p3 + p2 - shadow_pn_weight(p3, p2, n3) * n3) / 3.0;
    const float3 b102 = (2.0 * p3 + p1 - shadow_pn_weight(p3, p1, n3) * n3) / 3.0;
    const float3 b201 = (2.0 * p1 + p3 - shadow_pn_weight(p1, p3, n1) * n1) / 3.0;
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

    return shadow_position_at(position, pc.draws[patch[0].record], pc.light_matrix);
}
