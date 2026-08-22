// The sun's shadow as a SCREEN-SPACE MASK: rotate the kernel per pixel, then blur the result.
//
// `world.slang`'s `sun_visibility` filters the shadow map inline, with one pattern shared by every
// pixel (4.86's argument, carried into 4.100). That is what lets it run with no pass and no target
// - and it is also its ceiling. A fixed pattern cannot trade its artefact for noise, so an
// under-sampled kernel produces *structure* rather than grain, and a wide PCSS penumbra is exactly
// the under-sampled case. Rotating the pattern per pixel turns that structure into noise, and
// noise has to be blurred - which cannot happen inline, because a fragment shader cannot see its
// neighbours' shadow terms. Hence this file: one pass computes visibility per pixel into a target,
// a second blurs it, and the world shader reads the result.
//
// **The rotation and the blur are one design, not two steps.** The rotation is chosen from a 4x4
// tile of the pixel's own coordinates - 16 angles, indexed `(x & 3, y & 3)` - and the blur is a
// 4x4 box. Any four consecutive integers cover each residue mod 4 exactly once, so *every* pixel's
// blur window contains all 16 rotations exactly once, wherever it sits. The output is a
// 16 x `taps` estimate with no residual dither, rather than a noisy estimate smoothed until the
// noise is bearable. It is also **deterministic**: the rotation is a function of the pixel and not
// of the frame, so nothing crawls as the camera moves - which matters here because there is no
// temporal filter to lean on.
//
// The blur is bilateral. Where a tap is rejected the 16-rotation identity breaks, so a silhouette
// is the one place this can still show noise - the same place a plain Gaussian would instead have
// bled one surface's shadow onto another.
//
// **It reads the AO prepass's targets and rasterises no geometry of its own.** That is why the
// prepass is now gated on either feature wanting it (src/VkDraw.cpp): it already writes the world
// position and normal per pixel, which is everything this needs to place a fragment in the sun's
// light space and to decide whether two pixels are the same surface.

// Must match kMaxShadowCascades in src/VkDraw.h, exactly as world.slang's copy does.
static const uint kMaxShadowCascades = 4u;
static const uint kNoTexture = 0xffffffffu;

// A copy of world.slang's, and `src/gen-shader-abi.py` checks BOTH against src/VkDraw.h - so this
// cannot drift from either without a compile error naming the field. Declared in full even though
// this pass reads nine of its members, for the reason ao.slang's GpuDrawRecord copy is: the struct
// is reached through a pointer, so a short declaration puts every field after the first omission
// at the wrong offset.
// GpuFrameData points at an array of these, so the pointer's target type has to exist here
// even though this pass never dereferences one. Lifted from world.slang like the struct
// below it, and paired in `src/gen-shader-abi.py` for the same reason.
struct GpuMapLight {
    float4 position;  // xyz world space, w = range
    float4 colour;    // rgb premultiplied, w = engine_light_flags as a float
    float4 axis;      // xyz = row 2 of the orientation, the light's axis
};

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

// One block for both of this file's passes, because a pipeline layout is per pass and two blocks
// would be two layouts for no gain.
struct MaskPush {
    // **This frame's block, and it is filled AFTER this pass is recorded.** That is legal and it is
    // why `ReserveFrameData` exists in src/VkDraw.cpp: a push constant needs the *address* at
    // record time, and the CPU fills the block long before the queue executes any of these
    // commands.
    ConstBufferPointer<GpuFrameData> frame;
    uint position_texture; // the prepass's world position, xyz with w = 1 where it drew
    uint normal_texture;   // ... and its normal, biased into [0,1]. The blur's second guard
    uint source_texture;   // the blur's input. The resolve does not read it
    uint pad0;
};

[[vk::push_constant]]
ConstantBuffer<MaskPush> pc;

// The bindless set, exactly as world.slang and ao.slang declare it.
[[vk::binding(0, 0)]] SamplerState samplers[];
[[vk::binding(1, 0)]] Texture2D textures[];

// The full-screen triangle, and its own copy for the reason tonemap.slang keeps one: the AO pass
// is allowed to fail to build and this one still has to run, so borrowing its module would couple
// the two.
[shader("vertex")]
float4 mask_fullscreen_vertex(uint vertex_id: SV_VertexID) : SV_Position {
    const float2 uv = float2((vertex_id << 1u) & 2u, vertex_id & 2u);
    return float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

// --- the resolve --------------------------------------------------------------------------------

// Which of the 16 rotations a pixel takes, as a **Bayer** order over the 4x4 tile rather than
// `y * 4 + x`. The blur covers all 16 either way, so this changes nothing about the converged
// result; what it changes is the *unconverged* one - neighbouring pixels get widely separated
// angles instead of adjacent ones, so the raw mask reads as fine dither rather than as diagonal
// banding, and a partially-rejected blur window degrades gracefully instead of toward one angle.
static const uint kRotations = 16u;
static const uint kBayer[16] = {
    0u, 8u, 2u, 10u,
    12u, 4u, 14u, 6u,
    3u, 11u, 1u, 9u,
    15u, 7u, 13u, 5u,
};

// The golden angle. A Vogel spiral - `r = sqrt((i + 0.5) / n)`, `theta = i * this` - is
// **equal-area at every prefix length**, which a baked table is not: a 32-point table's first 16
// points cover the inner half of the disc only. Generating the offsets costs one sqrt and one
// sincos a tap and buys `shadow_soft_taps` meaning the same thing here as it does inline.
//
// A spiral rather than ao.slang's lattice, and the rotation is the whole reason: a lattice is
// stratified in one fixed frame, which is exactly right when every pixel must share one pattern
// and wrong once each pixel turns it. A spiral rotates into itself.
static const float kGoldenAngle = 2.39996323;

// The offsets are in texels of the fragment's own cascade tile.
float2 mask_tap(uint i, uint taps, float2 rotation) {
    const float radius = sqrt((float(i) + 0.5) / float(taps));
    const float angle = float(i) * kGoldenAngle;
    const float2 unrotated = float2(cos(angle), sin(angle)) * radius;
    // Complex multiply: one sincos for the pixel, not one per tap.
    return float2(unrotated.x * rotation.x - unrotated.y * rotation.y,
                  unrotated.x * rotation.y + unrotated.y * rotation.x);
}

[shader("fragment")]
float mask_resolve_fragment(float4 position: SV_Position) : SV_Target {
    const GpuFrameData frame = pc.frame[0];
    const int2 pixel = int2(position.xy);
    // Every early exit returns 1.0 - "fully lit" - because this target is a multiplier on the
    // sun's contribution, exactly as ao.slang's is on the ambient one. There is no "no data" value
    // and there must not be: the world shader falls back on its own inline filter when the pixel
    // it reads is not the surface it is shading, and everything else here has to be a number the
    // shading can use as it stands.
    if (frame.shadow_texture == kNoTexture || frame.shadow_cascades == 0u) {
        return 1.0;
    }
    const float4 centre = textures[pc.position_texture].Load(int3(pixel, 0));
    if (centre.w == 0.0) {
        return 1.0; // the sky, or a pixel only a pre-transformed draw covers
    }

    // From here to the second loop this is world.slang's `sun_visibility`, and it has to stay that
    // way: the two paths answer the same question and a difference between them would present as
    // the blur "changing the shadow" rather than as the two disagreeing.
    const float3 ls = mul(float4(centre.xyz, 1.0),
                          float4x4(frame.sun_matrix[0], frame.sun_matrix[1],
                                   frame.sun_matrix[2], frame.sun_matrix[3])).xyz;
    const float depth = (ls.z - frame.shadow_z_near) / frame.shadow_z_span;
    if (depth < 0.0 || depth > 1.0) {
        return 1.0;
    }
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
        return 1.0;
    }
    const float reference = depth - frame.cascades[cascade].w;
    const float2 tile = float2(float(cascade & 1u), float(cascade >> 1));
    const float2 lo = float2(0.5, 0.5) * frame.shadow_texel;
    const float2 hi = float2(1.0, 1.0) - lo;

    // This pixel's rotation, once. `pixel & 3` and not a hash: the blur's window is 4x4, and the
    // identity that makes it converge is that the tile and the window are the same size.
    const uint slot = kBayer[uint(pixel.y & 3) * 4u + uint(pixel.x & 3)];
    const float rotation_angle = (float(slot) + 0.5) * (6.28318531 / float(kRotations));
    const float2 rotation = float2(cos(rotation_angle), sin(rotation_angle));

    const float world_texel = 2.0 * frame.shadow_texel / frame.cascades[cascade].z;
    const uint taps = max(frame.shadow_soft_taps, 1u);
    const float ceiling = max(frame.shadow_soft_max, frame.shadow_soft_min);

    // The blocker search, over the ceiling for the reason 4.100 gives: a blocker further away
    // could only ask for a penumbra the ceiling clamps back to the ceiling.
    float blocker_sum = 0.0;
    float blocker_count = 0.0;
    for (uint i = 0u; i < taps; ++i) {
        const float2 tap =
            clamp(local + mask_tap(i, taps, rotation) * ceiling * frame.shadow_texel, lo, hi);
        const float d = textures[frame.shadow_texture].SampleLevel(
            samplers[frame.shadow_sampler], (tap + tile) * 0.5, 0.0).r;
        if (d < reference) {
            blocker_sum += d;
            blocker_count += 1.0;
        }
    }
    if (blocker_count < 0.5) {
        return 1.0;
    }
    const float distance = max(depth - blocker_sum / blocker_count, 0.0) * frame.shadow_z_span;
    const float radius =
        clamp(frame.shadow_softness * distance / world_texel, frame.shadow_soft_min, ceiling);

    float lit = 0.0;
    for (uint i = 0u; i < taps; ++i) {
        const float2 tap =
            clamp(local + mask_tap(i, taps, rotation) * radius * frame.shadow_texel, lo, hi);
        const float d = textures[frame.shadow_texture].SampleLevel(
            samplers[frame.shadow_sampler], (tap + tile) * 0.5, 0.0).r;
        lit += reference <= d ? 1.0 : 0.0;
    }
    // **Raw visibility, with `shadow_strength` NOT applied.** The world shader applies it, so
    // moving that knob costs no re-resolve and there is exactly one definition of what it means.
    return lit / float(taps);
}

// --- the blur -----------------------------------------------------------------------------------

// How far off the centre pixel's tangent plane a tap may be, as a fraction of its own distance -
// so it is dimensionless and needs no world-space scale, no eye position and no depth range. A
// tap on the same flat surface is exactly in plane whatever the surface's size on screen; one
// across a depth step is not.
static const float kPlaneTolerance = 0.2;
// ... and how far the two normals may disagree. This is the guard the plane test cannot make: a
// wall meeting a floor puts both surfaces near each other with the delta lying in the floor's
// plane, and their shadows are unrelated.
static const float kNormalTolerance = 0.7;

[shader("fragment")]
float mask_blur_fragment(float4 position: SV_Position) : SV_Target {
    const int2 pixel = int2(position.xy);
    const float raw = textures[pc.source_texture].Load(int3(pixel, 0)).r;
    const float4 centre = textures[pc.position_texture].Load(int3(pixel, 0));
    if (centre.w == 0.0) {
        return raw; // uncovered, so the resolve returned 1.0 and there is nothing to average
    }
    const float3 normal =
        normalize(textures[pc.normal_texture].Load(int3(pixel, 0)).xyz * 2.0 - 1.0);

    // **-1..2, which is four wide and therefore covers each residue mod 4 exactly once.** A
    // symmetric 5x5 would cover one residue twice and bias the estimate toward that rotation; a
    // 3x3 would miss seven of the sixteen. The asymmetry is the price of the identity and is
    // invisible: it shifts the estimate by half a pixel, in a term that is already an average over
    // a penumbra.
    float sum = 0.0;
    float weight = 0.0;
    for (int y = -1; y <= 2; ++y) {
        for (int x = -1; x <= 2; ++x) {
            const int2 tap = pixel + int2(x, y);
            const float4 p = textures[pc.position_texture].Load(int3(tap, 0));
            // Load clamps out of bounds to zero, and a zero texel has `w == 0` - so the frame's
            // border needs no special case, exactly as it does not in ao.slang.
            if (p.w == 0.0) {
                continue;
            }
            const float3 delta = p.xyz - centre.xyz;
            const float length_squared = dot(delta, delta);
            if (length_squared > 0.0) {
                const float off_plane = dot(delta, normal);
                if (off_plane * off_plane >
                    kPlaneTolerance * kPlaneTolerance * length_squared) {
                    continue;
                }
            }
            const float3 tap_normal =
                normalize(textures[pc.normal_texture].Load(int3(tap, 0)).xyz * 2.0 - 1.0);
            if (dot(tap_normal, normal) < kNormalTolerance) {
                continue;
            }
            sum += textures[pc.source_texture].Load(int3(tap, 0)).r;
            weight += 1.0;
        }
    }
    // `weight` is never 0 - the centre tap passes both tests against itself - but the guard costs
    // nothing and a divide by zero here would be a NaN straight into the light sum.
    return weight > 0.0 ? sum / weight : raw;
}
