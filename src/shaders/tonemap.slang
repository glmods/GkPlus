// The tonemap pass: what turns the world pass's fp16 target into the swapchain's 8 bits.
//
// It exists because of what it REPLACES. Until HDR the offscreen target reached the swapchain
// through `vkCmdBlitImage` with `VK_FILTER_NEAREST`, and notes 4.37 measured that the point sample
// there is not a lazy default - the original's windowed stretch preserves a 4-bit texture's
// sixteen distinct values, so D3D drops columns rather than mixing them. This pass has to do that
// same scale, so it does it the same way rather than approximately: `Load` at
// `floor((dst + 0.5) * src / dst)`, which is what the spec says nearest blitting resolves to. No
// sampler is involved, which is also why this pass needs nothing from the sampler array.
//
// Everything else it does is per pixel and in that order: exposure, the operator, the sRGB encode.

// Must match `TonemapPushConstants` in src/VkDraw.cpp - and every field is a 4-byte scalar on
// purpose, so the scalar and std430 layouts agree and src/gen-shader-abi.py can check the pair.
struct TonemapPush {
    // The offscreen target, as a bindless index. `kTonemapSourceSlot` in src/VkResources.h - a
    // fixed high slot like the shadow atlases' and the AO pass's, for the same reason: it is not a
    // `TextureImage` and has no business in the allocator's range.
    uint source_texture;
    // 0 clamp, 1 rolloff, 2 reinhard, 3 aces. See `apply_operator`.
    uint op;
    uint flags;
    uint source_width;
    uint source_height;
    uint dest_width;
    uint dest_height;
    // A linear multiplier, applied before the operator - which is the only place it can go. After
    // it, it would just brighten an already-compressed image and undo the compression's point.
    float exposure;
    // Where `rolloff` stops being the identity. See below.
    float knee;
    // What `reinhard` maps to exactly 1.0.
    float white;
};

// The buffer holds LINEAR light and the swapchain wants sRGB, so encode on the way out. Off when
// `render.linear_input` is off, where the buffer holds the gamma-encoded values the fixed function
// always produced and encoding them again would brighten the whole frame.
static const uint kEncodeSrgb = 0x1u;

[[vk::push_constant]]
ConstantBuffer<TonemapPush> pc;

// The bindless set, exactly as world.slang and ao.slang declare it. The sampler array is declared
// but unused - the binding numbers have to match the shared layout, and dropping binding 0 would
// renumber binding 1.
[[vk::binding(0, 0)]] SamplerState samplers[];
[[vk::binding(1, 0)]] Texture2D textures[];

// The exact piecewise sRGB transfer function, not a 2.2 power. The two differ most below about
// 0.04, which is where this game spends a lot of a frame - Gunlok's interiors are dark - and the
// piecewise form is also what makes the round trip exact: an 8-bit value decoded to linear, stored
// as fp16 and encoded again comes back to the same 8-bit value, because fp16's ~5e-4 relative
// error times this curve's steepest slope is still under a hundredth of a level. That exactness is
// what makes `decode -> clamp -> encode` a true identity, which is the bisect setting the whole
// feature is checked against.
float3 srgb_encode(float3 linear_colour) {
    const float3 c = max(linear_colour, float3(0, 0, 0));
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

// **Identity below the knee, compressing above it**, and the default.
//
// It used to be the default for a reason that no longer holds: the pass ran over the whole frame,
// Gunlok's 2D half included, so an operator with a toe recoloured the menus and the HUD along with
// the world - ACES moved 99.61% of the main menu. Being the identity below a knee was the only
// thing keeping those screens intact. **That is now the pass structure's job** (notes §4.92): the
// 2D layers are drawn afterwards and nothing here can reach them.
//
// So this stays the default as the conservative choice rather than as a guard - it touches only
// what genuinely exceeds the knee, which in this game is very little - and the film curves below
// are now usable looks rather than traps.
//
// The upper branch is C1 at the knee - its derivative there is exactly 1 - so there is no visible
// crease where the two meet, and it asymptotes to 1 rather than reaching it, so no finite input
// ever clips.
float3 rolloff(float3 c, float knee) {
    const float k = clamp(knee, 0.0, 0.999);
    const float headroom = 1.0 - k;
    const float3 over = max(c - k, float3(0, 0, 0));
    return min(c, float3(k, k, k)) + headroom * (1.0 - exp(-over / headroom));
}

// Extended Reinhard: exactly 1.0 at `white`, and everything below compressed towards it. Unlike
// the rolloff it is nowhere the identity, so it darkens the whole world image rather than only its
// highlights - a look and a second opinion, not a default.
float3 reinhard(float3 c, float white) {
    const float w = max(white, 1e-3);
    return (c * (1.0 + c / (w * w))) / (1.0 + c);
}

// Narkowicz's fit to the ACES filmic curve. The other look, and the one most people mean by
// "tonemapping"; it has a toe, so blacks lift across the whole world image.
float3 aces(float3 c) {
    return saturate((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14));
}

float3 apply_operator(float3 c) {
    switch (pc.op) {
    case 1u: return rolloff(c, pc.knee);
    case 2u: return reinhard(c, pc.white);
    case 3u: return aces(c);
    // 0, and anything unrecognised: what the 8-bit target did by itself. The bisect setting - with
    // `render.linear_input` off it makes this pass a reproduction of the blit it replaced, so a
    // visible change at this setting is a defect in the pass rather than in the colour work.
    default: return saturate(c);
    }
}

[shader("vertex")]
float4 tonemap_fullscreen_vertex(uint vertex_id: SV_VertexID) : SV_Position {
    // (0,0) (2,0) (0,2) in uv, which is (-1,-1) (3,-1) (-1,3) in clip - one triangle covering the
    // target with no diagonal seam down the middle. Its own copy rather than a reuse of
    // ao.slang's: the AO pass is allowed to fail to build and this one must still run.
    const float2 uv = float2((vertex_id << 1u) & 2u, vertex_id & 2u);
    return float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

[shader("fragment")]
float4 tonemap_fragment(float4 position: SV_Position) : SV_Target {
    // The nearest-blit mapping, spelled out. `position.xy` is already the destination pixel's
    // centre, so the `+ 0.5` the spec's formula applies is already in it.
    const float2 scale = float2(float(pc.source_width) / float(max(pc.dest_width, 1u)),
                                float(pc.source_height) / float(max(pc.dest_height, 1u)));
    const int2 source = int2(min(floor(position.xy * scale),
                                 float2(float(pc.source_width - 1u), float(pc.source_height - 1u))));

    const float4 texel = textures[pc.source_texture].Load(int3(source, 0));
    float3 colour = apply_operator(texel.rgb * pc.exposure);
    if ((pc.flags & kEncodeSrgb) != 0u) {
        colour = srgb_encode(colour);
    }
    // Alpha is passed through rather than tonemapped - it is coverage, not light, and nothing
    // downstream reads it anyway: the swapchain is opaque and `compositeAlpha` is OPAQUE.
    return float4(colour, texel.a);
}
