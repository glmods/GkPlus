// Bloom: three independent layers, each extracted from the HDR target with its own threshold,
// blurred at its own scale, and composited by the tonemap pass with its own blend mode.
//
// **It exists only under `render.hdr`, and that is a structural dependency rather than a policy.**
// A threshold is a statement about light, and the 8-bit target has none to speak of: every value in
// it is already clamped to 1, so "brighter than white" is not a question that can be asked of it.
// The float target is what makes a threshold at 1.0 mean "this pixel is over-range" - which in this
// game is exactly the additive fires, the flares, and the surfaces a `diffuse 4.0` light lands on
// (notes 4.48). So this file reads the same image the tonemap reads, and runs between the world
// pass and it.
//
// Two passes per layer, three entry points in total:
//
//   extract  the HDR target -> the layer's buffer A, downsampling and thresholding in one step
//   blur     buffer A -> B horizontally, then B -> A vertically, a separable Gaussian
//
// The result therefore always ends up in buffer A, which is what the tonemap's push names. The
// composite itself is in tonemap.slang and not here, and that is the one structural decision in
// this feature worth defending: bloom is light added before the sensor sees it, so it belongs
// *before* exposure and the operator - and the tonemap pass is already reading the HDR target at
// full resolution and writing the LDR one. Compositing there costs no extra full-screen pass and no
// extra full-resolution image.

// Must match `BloomPushConstants` in src/VkDraw.cpp - every field a 4-byte scalar, so the scalar
// and std430 layouts agree and src/gen-shader-abi.py can check the pair. One block for both passes:
// Slang gives one file one push block, and a field the other pass ignores costs nothing.
struct BloomPush {
    // The bindless index to read. `kTonemapSourceSlot` for the extract - the HDR target, the same
    // image the tonemap reads - and a layer buffer for each blur.
    uint source_texture;
    // The size of the image being WRITTEN. There is deliberately no source size here: the extract
    // needs the ratio only to pick `taps`, which the CPU does, and the blur's source and destination
    // are the same image size by construction.
    uint dest_width;
    uint dest_height;
    // **Extract**: box taps per axis over the downsample footprint. **Blur**: the kernel half-width
    // in destination texels, so the loop runs `2 * taps + 1` fetches.
    uint taps;
    // Blur only: 0 horizontal, 1 vertical.
    uint direction;
    // Extract only: which bindless sampler to fetch through. `RecordBloom` acquires a LINEAR/CLAMP
    // one the same way the shadow passes acquire theirs, and pushes its index - rather than this
    // pass assuming `samplers[0]`, which is whatever filter the first draw of the session happened
    // to ask for and would make the downsample a point sample on a session that opened with a
    // POINT-filtered texture.
    uint sampler_index;
    // Extract only. The linear luminance at which a pixel starts to contribute, and the width of
    // the soft ramp around it - see `bright_pass`.
    float threshold;
    float knee;
    // Blur only, in destination texels. `taps` above is derived from it on the CPU.
    float sigma;
};

[[vk::push_constant]]
ConstantBuffer<BloomPush> pc;

// The bindless set, exactly as world.slang declares it. The binding numbers have to match the
// shared layout, so the sampler array is declared whether a pass uses it or not.
[[vk::binding(0, 0)]] SamplerState samplers[];
[[vk::binding(1, 0)]] Texture2D textures[];

// Rec.709 luma, on LINEAR values - which is what makes these the right coefficients rather than a
// habit: they describe the eye's response to linear light. Applied to the gamma-encoded values the
// buffer holds when `render.linear_input` is off they over-weight the dark end. It is still the best
// available answer there, and it is one more reason that sub-knob defaults to on.
float bloom_luminance(float3 c) {
    return dot(max(c, float3(0, 0, 0)), float3(0.2126, 0.7152, 0.0722));
}

// The threshold with a soft knee, in the form Unity and Call of Duty both ship.
//
// **A hard `l > threshold` is the thing being avoided**, and not for a subtle reason: a surface
// drifting across the threshold as the camera moves would pop its whole bloom contribution on and
// off between two frames. The quadratic ramp over `[threshold - knee, threshold + knee]` is C1 at
// both ends, so a pixel crossing it fades instead.
//
// The colour is SCALED rather than replaced, which is what keeps the glow the same hue as the thing
// glowing: only the *amount* passed through is decided by luminance.
float3 bright_pass(float3 c, float threshold, float knee) {
    const float l = bloom_luminance(c);
    const float k = max(knee, 1e-4);
    // The knee's own contribution: zero below `threshold - k`, exactly `l - threshold` at
    // `threshold + k`, quadratic between.
    float soft = clamp(l - threshold + k, 0.0, 2.0 * k);
    soft = soft * soft / (4.0 * k);
    const float pass = max(soft, l - threshold);
    return c * (pass / max(l, 1e-4));
}

// The full-screen triangle, its own copy rather than a reuse of tonemap.slang's or ao.slang's, for
// the reason tonemap.slang gives for having its own: each of these passes is allowed to fail to
// build without taking the others down, and a shared module would couple them.
[shader("vertex")]
float4 bloom_fullscreen_vertex(uint vertex_id: SV_VertexID) : SV_Position {
    const float2 uv = float2((vertex_id << 1u) & 2u, vertex_id & 2u);
    return float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

// Downsample and threshold in one pass, because they commute and doing them together saves a
// full-resolution write of the frame.
//
// **The box covers the whole footprint, and its tap count follows the ratio.** A layer is allocated
// at a fixed line count (kBloomLayerHeights in src/VkDraw.cpp), so at 480 lines the first layer is a
// 2x downsample and at 1728 lines it is 7.2x - and a single bilinear tap over a 7.2-texel footprint
// is a point sample of a moving image, which reads as bright pixels flickering rather than as
// aliasing. Each tap is a LINEAR fetch and so covers about two source texels; `taps` per axis is
// therefore about half the ratio, capped, and the CPU computes it.
[shader("fragment")]
float4 bloom_extract_fragment(float4 position: SV_Position) : SV_Target {
    const float2 dest_size = float2(float(max(pc.dest_width, 1u)), float(max(pc.dest_height, 1u)));
    const uint taps = max(pc.taps, 1u);
    const float step = 1.0 / float(taps);

    float3 sum = float3(0, 0, 0);
    for (uint y = 0u; y < taps; ++y) {
        for (uint x = 0u; x < taps; ++x) {
            // The tap centres divide the destination texel into `taps` equal cells; sampling each
            // cell's centre with a linear filter is a box over the footprint to within the filter's
            // own width, which is the whole point of using one.
            const float2 offset = (float2(float(x), float(y)) + 0.5) * step;
            const float2 uv = (floor(position.xy) + offset) / dest_size;
            sum += textures[pc.source_texture].SampleLevel(samplers[pc.sampler_index], uv, 0.0).rgb;
        }
    }
    const float3 average = sum / float(taps * taps);
    // Thresholded AFTER the average, not before. Before it, a single over-range pixel inside the
    // footprint would pass at full strength and the downsample would then dim it by the tap count
    // anyway - the same answer by a longer route on the common case, and the wrong one wherever the
    // knee is doing its job, since the ramp would be evaluated on values the frame does not have.
    return float4(bright_pass(average, pc.threshold, pc.knee), 1.0);
}

// One axis of a separable Gaussian, over the layer's own buffer at its own size.
//
// `Load` and not a sampler: source and destination are the same size here, so every tap is an exact
// texel and a filtered fetch would add a half-texel question to a pass that has none. Coordinates
// are CLAMPED rather than left to fall out of bounds - `Load` returns zero outside the image, which
// would darken the frame's border by however much of the kernel hangs off it, and a bright thing at
// the edge of the screen is exactly the case bloom is asked about most.
[shader("fragment")]
float4 bloom_blur_fragment(float4 position: SV_Position) : SV_Target {
    const int2 centre = int2(position.xy);
    const int2 limit = int2(int(pc.dest_width) - 1, int(pc.dest_height) - 1);
    const int2 axis = pc.direction == 0u ? int2(1, 0) : int2(0, 1);
    const int taps = int(pc.taps);
    // Guarded: sigma reaches this pass alongside a `taps = ceil(3 * sigma)` computed from it, so a
    // zero would be a zero-width kernel and a division by zero in the weight below.
    const float sigma = max(pc.sigma, 1e-3);
    const float denominator = 2.0 * sigma * sigma;

    float3 sum = float3(0, 0, 0);
    float weight_sum = 0.0;
    for (int i = -taps; i <= taps; ++i) {
        const float weight = exp(-float(i * i) / denominator);
        const int2 at = clamp(centre + axis * i, int2(0, 0), limit);
        sum += textures[pc.source_texture].Load(int3(at, 0)).rgb * weight;
        weight_sum += weight;
    }
    // Normalised by the weights actually taken rather than by the analytic integral, so a truncated
    // kernel preserves the image's mean instead of dimming it by the tail it dropped.
    return float4(sum / max(weight_sum, 1e-6), 1.0);
}
