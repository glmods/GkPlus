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
float4 shadow_position(Vertex v, GpuDrawRecord draw, float4 light_matrix[4]) {
    if ((draw.lighting & kLightSum) == 0u) {
        return float4(0, 0, 2, 1);
    }
    const float4 local = float4(v.pos.xyz, 1.0);
    const float4 world = mul(local, float4x4(draw.world[0], draw.world[1], draw.world[2],
                                             draw.world[3]));
    return mul(float4(world.xyz, 1.0),
               float4x4(light_matrix[0], light_matrix[1], light_matrix[2], light_matrix[3]));
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
