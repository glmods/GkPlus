// The tonemap pass: what turns the world pass's fp16 target into the swapchain's 8 bits.
//
// It exists because of what it REPLACES. Until HDR the offscreen target reached the swapchain
// through `vkCmdBlitImage` with `VK_FILTER_NEAREST`, and notes 4.37 measured that the point sample
// there is not a lazy default - the original's windowed stretch preserves a 4-bit texture's
// sixteen distinct values, so D3D drops columns rather than mixing them. This pass has to do that
// same scale, so it does it the same way rather than approximately: `Load` at
// `floor((dst + 0.5) * src / dst)`, which is what the spec says nearest blitting resolves to. The
// source is therefore never filtered; the only filtered fetches in this file are the bloom layers,
// which are smaller than the frame and have to be.
//
// Everything else it does is per pixel and in that order: the bloom composite, exposure, the
// operator, the sRGB encode.

// How many layers, and it is the shape of the feature rather than a tuning value: three is what
// covers a tight halo, a mid-range glow and a wide bloom in one frame. Checked against the C++
// `kBloomLayers` by src/gen-shader-abi.py.
static const uint kBloomLayers = 3;

// The blend modes, checked against C++ the same way. Each one is a different answer to "what does
// added light do to a pixel that is already bright", and all three are meaningful in unbounded
// linear light - which rules out most of the Photoshop list, since those are defined on [0,1].
//
//   off     the layer is skipped entirely, here and in RecordBloom
//   add     `c + b`. What a lens actually does: light lands on top of light.
//   screen  `c + b * max(1 - c, 0)`. Identical to the classic `1 - (1 - c)(1 - b)` wherever the
//           base is in [0,1], and well-behaved above it, where the classic form starts *reducing*
//           the result. Bloom fills in what is not already lit and leaves a blown highlight alone.
//   max     `max(c, b)`. No accumulation at all - a halo that reaches a given brightness and stops,
//           which is the one of the three that cannot make a bright area brighter.
static const uint kBloomBlendOff = 0;
static const uint kBloomBlendAdd = 1;
static const uint kBloomBlendScreen = 2;
static const uint kBloomBlendMax = 3;

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

    // --- bloom (src/shaders/bloom.slang) ------------------------------------------------------
    //
    // The three layers are composited here rather than in a pass of their own, for the reason
    // bloom.slang gives: this pass already reads the HDR target at full resolution and writes the
    // LDR one, so a fourth full-screen pass would buy nothing. It happens **before `exposure` and
    // before the operator**, which is where added light belongs - bloom is light reaching the
    // sensor, so the sensor's response applies to it too.
    //
    // A layer whose blend is `kBloomBlendOff` is skipped, and `RecordBloom` skips its two passes as
    // well, so an off layer costs one not-taken branch here and nothing at all upstream. The
    // texture index is still valid in that case: the images exist for the life of the target.
    uint bloom_texture[kBloomLayers];
    uint bloom_blend[kBloomLayers];
    float bloom_intensity[kBloomLayers];
    // The layers are lower-resolution than this pass's output, so unlike the source they are
    // *sampled* rather than loaded - a point fetch of a 60-line image stretched over the frame is
    // visibly blocky, and a smooth glow is the entire point of the effect.
    uint bloom_sampler;
};

// The buffer holds LINEAR light and the swapchain wants sRGB, so encode on the way out. Off when
// `render.linear_input` is off, where the buffer holds the gamma-encoded values the fixed function
// always produced and encoding them again would brighten the whole frame.
static const uint kEncodeSrgb = 0x1u;

[[vk::push_constant]]
ConstantBuffer<TonemapPush> pc;

// The bindless set, exactly as world.slang and ao.slang declare it. Both arrays are used here: the
// source is `Load`ed out of `textures` and the bloom layers are sampled through `samplers`.
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

// The Uncharted 2 filmic curve (Hable), which is what "Filmic" means in almost every game that
// offers it. A toe, a linear midsection and a shoulder, from six constants nobody has ever had a
// reason to change.
//
// `white` is the linear value the curve maps to exactly 1.0, so it means the same thing here as it
// does for `reinhard` and shares `render.tonemap_white`. Hable's own default is 11.2, which
// belongs to a different exposure convention; this game's over-range is around 4.
float3 hable_curve(float3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float3 filmic(float3 c, float white) {
    const float w = max(white, 1e-3);
    // Normalised by the curve at the white point, so `white` really does land on 1.0 rather than
    // wherever the unnormalised curve happens to put it.
    return saturate(hable_curve(c) / hable_curve(float3(w, w, w)));
}

// --- AgX ---------------------------------------------------------------------------------------
//
// Troy Sobotka's, in the form everyone ships: an inset matrix that desaturates towards the
// achromatic axis, a log2 encode over a fixed exposure window, a sigmoid, and the inverse matrix.
// What it buys over the two curves above is **hue stability at the top end** - a bright saturated
// light desaturates towards white the way film does, instead of clipping one channel at a time and
// sliding towards whichever primary survives longest. That is the failure mode this game has most
// of: a `diffuse 4.0` light on a red surface goes orange under ACES and stays red under AgX.
//
// It ignores `render.tonemap_white` and `render.tonemap_knee`. Its range is fixed by the log
// window below, so **`render.exposure` is the only control it answers to** - which is the point of
// AgX rather than a limitation of this implementation.

// The 6th-order fit to AgX's sigmoid that every shader implementation uses (mean error^2 3.67e-6).
float3 agx_contrast(float3 x) {
    const float3 x2 = x * x;
    const float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 +
           0.1191 * x - 0.00232;
}

float3 agx(float3 c) {
    // **Written as ROWS, which is the transpose of how every GLSL copy of these numbers reads.**
    // A GLSL `mat3(...)` constructor is column-major and `m * v` treats v as a column; Slang's
    // `float3x3(a, b, c)` takes rows and `mul(m, v)` does the same product. Getting it the other
    // way round is not obviously broken on screen - it is still a plausible colour transform - so
    // the check that matters is that each ROW sums to 1, which is what makes the inset preserve
    // white. These do, to five decimal places.
    const float3x3 inset = float3x3(float3(0.842479062253094, 0.0784335999999992, 0.0792237451477643),
                                    float3(0.0423282422610123, 0.878468636469772, 0.0791661274605434),
                                    float3(0.0423756549057051, 0.0784336, 0.879142973793104));
    const float3x3 outset = float3x3(float3(1.19687900512017, -0.0980208811401368, -0.0990297440797205),
                                     float3(-0.0528968517574562, 1.15190312990417, -0.0980434501171241),
                                     float3(-0.0529716355144438, -0.0980434501171241, 1.15107367264116));
    // The log window, in stops either side of mid grey. Fixed, and the reason exposure is AgX's
    // only knob: everything below -12.47 EV is black and everything above +4.03 EV is white.
    const float min_ev = -12.47393, max_ev = 4.026069;

    float3 v = mul(inset, max(c, float3(0, 0, 0)));
    // 1e-10 rather than 0: log2(0) is -inf, and an inf that survives the sigmoid comes out as a
    // NaN pixel rather than as black.
    v = clamp(log2(max(v, float3(1e-10, 1e-10, 1e-10))), min_ev, max_ev);
    v = (v - min_ev) / (max_ev - min_ev);
    v = agx_contrast(saturate(v));
    v = mul(outset, v);
    // **Back to linear, because the sigmoid's output is display-referred.** Everything else in
    // this file hands `srgb_encode` a linear value and this has to as well, or the frame is
    // encoded twice. A 2.2 power and not the exact sRGB inverse: that is what the AgX reference
    // uses, and matching it matters more here than internal consistency with the encode - the
    // constants above were fitted against it.
    return pow(max(v, float3(0, 0, 0)), float3(2.2, 2.2, 2.2));
}

// The three layers, in index order - which matters for `screen` and `max` and does not for `add`.
// Index order is also scale order (layer 0 is the tightest), so the wide layers see what the tight
// ones already contributed, which is the way round that keeps a wide `max` from erasing a tight
// halo.
float3 apply_bloom(float3 c, float2 uv) {
    for (uint i = 0u; i < kBloomLayers; ++i) {
        const uint mode = pc.bloom_blend[i];
        if (mode == kBloomBlendOff) {
            continue;
        }
        const float3 b =
            textures[pc.bloom_texture[i]].SampleLevel(samplers[pc.bloom_sampler], uv, 0.0).rgb *
            pc.bloom_intensity[i];
        if (mode == kBloomBlendScreen) {
            c = c + b * max(1.0 - c, float3(0, 0, 0));
        } else if (mode == kBloomBlendMax) {
            c = max(c, b);
        } else {
            c = c + b;
        }
    }
    return c;
}

float3 apply_operator(float3 c) {
    switch (pc.op) {
    case 1u: return rolloff(c, pc.knee);
    case 2u: return reinhard(c, pc.white);
    case 3u: return aces(c);
    case 4u: return filmic(c, pc.white);
    case 5u: return agx(c);
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
    // The bloom layers cover the same scene as the source, so their uv is this pixel's position in
    // the DESTINATION - which is the same normalised coordinate whether or not the two extents
    // match, and therefore does not repeat the nearest-blit mapping above.
    float3 colour =
        apply_bloom(texel.rgb, position.xy / float2(float(max(pc.dest_width, 1u)),
                                                    float(max(pc.dest_height, 1u))));
    colour = apply_operator(colour * pc.exposure);
    if ((pc.flags & kEncodeSrgb) != 0u) {
        colour = srgb_encode(colour);
    }
    // Alpha is passed through rather than tonemapped - it is coverage, not light, and nothing
    // downstream reads it anyway: the swapchain is opaque and `compositeAlpha` is OPAQUE.
    return float4(colour, texel.a);
}
