// Binning the level's static lights into a world-space grid, on the GPU.
//
// The map's `STDLIGHT` rig is 431 lights on level04 and 686 on level01, and phase 3b evaluated
// every one of them per pixel. This is what makes that affordable: each cell of a grid over the
// level's own bounds records which lights reach it, and a fragment reads only its own cell.
//
// **A world-space grid rather than the view-space cluster grid the plan named**, and the reason
// is that these lights never move relative to the world:
//
//   - it is built **once per level** instead of once per frame, so the steady-state cost is zero
//     on both sides rather than small on one;
//   - it needs no view matrix in the fragment shader, which already carries `world_position` for
//     the lighting map - a screen-tile scheme would need a view-depth slice derived from a matrix
//     that is per *draw* here, which is a real source of quiet error;
//   - it is resolution-independent. At 3072x1728 - which is what this machine runs - a 16-pixel
//     tile grid with 24 depth slices is nearly 500,000 clusters, where this is 16,384 whatever
//     the window is.
//
// The trade is that it does not help a *moving* light. Gunlok's dynamic lights are the D3D ones,
// which are capped at eight per draw by the hardware and are already carried per draw
// (`GpuLight`), so there is nothing here for a view-space scheme to earn.
//
// Two passes over the lights per cell rather than one, deliberately: count, take one atomic, then
// write. Gathering into a local array first would spill 128 uints per thread, and the tests are
// cheap enough that doing them twice is the smaller cost.

// Must match GpuMapLight in src/VkDraw.h.
struct GpuMapLight {
    float4 position;  // xyz world space, w = range
    float4 colour;
    float4 axis;
};

// Must match LightGridPush in src/VkDraw.cpp.
struct GridPush {
    float4 grid_min;    // xyz world space, w unused
    float4 cell_size;   // xyz world units per cell, w unused
    uint4 dims;         // xyz cells, w = light_count
    uint index_capacity;
    uint pad0;
    uint pad1;
    uint pad2;
};

[[vk::push_constant]]
ConstantBuffer<GridPush> pc;

// Bound for this pass only - the graphics side reaches all three by device address instead, but a
// compute shader that WRITES is the one place a plain storage-buffer binding is simpler than
// arguing with pointer semantics.
[[vk::binding(0, 0)]] StructuredBuffer<GpuMapLight> lights;
// [0] is the allocator and [1..9] are the dims, origin and cell size the FRAGMENT shader reads
// back out - all written by a vkCmdUpdateBuffer before this runs. The cells start at [12], so the
// pair for cell i is at [12 + 2*i].
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> grid;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> indices;

static const uint kOmni = 0x4u;
static const uint kGridHeader = 12u;

// Does this light's sphere of influence reach this cell?
//
// The sphere is the light's `range`, which is a hard cutoff in the fitted model (§4.54) rather
// than a fade - so a light outside the cell contributes exactly nothing to any fragment in it and
// dropping it here is not an approximation. **That is what makes the grid exact**, and it is why
// the A/B against the brute-force path is expected to be bit-identical rather than close.
bool reaches(GpuMapLight light, float3 lo, float3 hi) {
    // Nearest point of the cell's box to the light's centre.
    const float3 nearest = clamp(light.position.xyz, lo, hi);
    const float3 delta = light.position.xyz - nearest;
    const float range = light.position.w;
    return dot(delta, delta) < range * range;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void build_grid(uint3 thread: SV_DispatchThreadID) {
    const uint cell = thread.x;
    const uint total = pc.dims.x * pc.dims.y * pc.dims.z;
    if (cell >= total) {
        return;
    }
    const uint3 coordinate =
        uint3(cell % pc.dims.x, (cell / pc.dims.x) % pc.dims.y, cell / (pc.dims.x * pc.dims.y));
    const float3 lo = pc.grid_min.xyz + float3(coordinate) * pc.cell_size.xyz;
    const float3 hi = lo + pc.cell_size.xyz;

    const uint light_count = pc.dims.w;
    uint count = 0u;
    for (uint i = 0u; i < light_count; ++i) {
        if (reaches(lights[i], lo, hi)) {
            ++count;
        }
    }

    uint base = 0u;
    if (count > 0u) {
        InterlockedAdd(grid[0], count, base);
        // The budget is finite and a pathological level could exhaust it. Dropping this cell
        // whole - rather than filling it partially - is what keeps the failure legible: a cell
        // with count 0 renders as unlit, which is visible, where a half-filled one would render
        // as plausible-but-wrong lighting that nothing counts.
        if (base + count > pc.index_capacity) {
            count = 0u;
        }
    }

    grid[kGridHeader + 2u * cell + 0u] = base;
    grid[kGridHeader + 2u * cell + 1u] = count;
    if (count == 0u) {
        return;
    }
    uint written = 0u;
    for (uint i = 0u; i < light_count && written < count; ++i) {
        if (reaches(lights[i], lo, hi)) {
            indices[base + written] = i;
            ++written;
        }
    }
}
