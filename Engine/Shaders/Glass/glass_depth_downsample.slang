// First levels of GlassPass's closest-surface (MIN) depth pyramid in one
// dispatch: each 16x16 workgroup reduces its block of the hardware depth in
// shared memory to up to four levels (1..4). Level 0 is the hardware depth
// itself, which the glass shaders sample directly; it is not copied.
//
// Only levels whose source extent is even in both axes are fused
// (params.fusedLevels, chosen on the CPU): there each texel covers exactly a
// 2x2 block of the level below, so the MIN stays conservative for the
// uv * size lookups of the Hi-Z trace. The remaining levels go through
// glass_depth_reduce.slang, which covers odd sizes with up to 3x3 texels.
[[vk::binding(0, 0)]] Sampler2D sourceDepth;
// levels[i] is pyramid level i + 1.
[[vk::binding(1, 0), vk::image_format("r32f")]] RWTexture2D<float> levels[4];

struct DepthParams
{
    uint2 size;       // level 0 extent
    uint fusedLevels; // levels written here (1..4)
    uint padding;
};
[[vk::push_constant]] ConstantBuffer<DepthParams> params;

groupshared float tile[256];

void store(uint level, uint2 texel, float value)
{
    if (level > params.fusedLevels || any(texel >= max(params.size >> level, uint2(1u, 1u))))
        return;
    switch (level)
    {
    case 1u: levels[0][texel] = value; break;
    case 2u: levels[1][texel] = value; break;
    case 3u: levels[2][texel] = value; break;
    default: levels[3][texel] = value; break;
    }
}

[numthreads(16, 16, 1)]
[shader("compute")]
void main(uint3 groupId : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    const uint2 texel = groupId.xy * 16u + localId.xy;
    // Far plane for lanes past the edge: they never win a MIN that matters,
    // since no in-bounds texel of a fused level covers them.
    float depth = 1.0;
    if (all(texel < params.size))
        depth = sourceDepth.Load(int3(texel, 0)).r;
    const uint index = localId.y * 16u + localId.x;
    tile[index] = depth;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint step = 1u; step <= 4u; ++step)
    {
        const uint stride = 1u << step;
        const uint half = stride >> 1u;
        if (step <= params.fusedLevels && all((localId.xy & (stride - 1u)) == 0u))
        {
            depth = min(min(tile[index], tile[index + half]), min(tile[index + half * 16u], tile[index + half * 17u]));
            tile[index] = depth;
            store(step, texel >> step, depth);
        }
        GroupMemoryBarrierWithGroupSync();
    }
}
