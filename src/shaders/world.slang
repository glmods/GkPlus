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

struct Push {
    float4x4 mvp;
    ConstBufferPointer<Vertex> vertices;
    uint base_vertex;    // where this draw's buffer starts, in canonical vertices
    uint texture_index;  // bindless slot, 0xffffffff for untextured
    uint sampler_index;
    uint flags;
};

[[vk::push_constant]]
ConstantBuffer<Push> pc;

// The bindless set, exactly as VkResources.cpp builds it.
[[vk::binding(0, 0)]] SamplerState samplers[];
[[vk::binding(1, 0)]] Texture2D textures[];

static const uint kNoTexture = 0xffffffffu;

struct VertexOut {
    float4 position : SV_Position;
    float4 color : COLOR;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOut vertex_main(uint vertex_id: SV_VertexID) {
    Vertex v = pc.vertices[pc.base_vertex + vertex_id];

    VertexOut output;
    // No branch on pre-transformed vertices on purpose: the CPU puts either world*view*proj or
    // a pixels-to-clip orthographic matrix in `mvp`, so both FVF families are one code path. A
    // branch would have had to know the viewport, which the shader does not.
    //
    // `mul(v, M)` and not `mul(M, v)`: that is D3D's row-vector convention, and combined with
    // -matrix-layout-row-major it means the CPU can upload world*view*proj exactly as the game
    // composes it, with no transpose step to get backwards.
    output.position = mul(float4(v.pos.xyz, 1.0), pc.mvp);
    // D3DCOLOR is 0xAARRGGBB in a uint.
    uint packed = asuint(v.normal_and_color.w);
    output.color = float4(float((packed >> 16) & 0xffu), float((packed >> 8) & 0xffu),
                          float(packed & 0xffu), float((packed >> 24) & 0xffu)) / 255.0;
    output.uv = v.uv.xy;
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOut input) : SV_Target {
    float4 colour = input.color;
    if (pc.texture_index != kNoTexture) {
        colour *= textures[pc.texture_index].Sample(samplers[pc.sampler_index], input.uv);
    }
    return colour;
}
