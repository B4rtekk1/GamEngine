// Builds GlassPass's mip-mapped scene snapshot in two dispatches instead of a
// copy plus one blit (and one pipeline drain) per level.
//
// Each 16x16 workgroup owns a 16x16 block of its base level and reduces it
// in shared memory to four coarser levels (2x2 box filter, as the blit chain
// did for even sizes):
//  - pass 0 copies the HDR colour into level 0 and writes levels 1..4;
//  - pass 1 reads level 4 back and writes levels 5..7.
// Mip extents are floor(size / 2^level), so every written texel only covers
// in-bounds texels of the base level; out-of-bounds lanes feed nothing.
[[vk::binding(0, 0)]] Sampler2D source;
[[vk::binding(1, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> levels[8];

struct SnapshotParams
{
    uint2 size;      // level 0 extent
    uint pass;       // 0: base level 0, 1: base level 4
    uint levelCount; // levels in the snapshot (at most 8)
};
[[vk::push_constant]] ConstantBuffer<SnapshotParams> params;

groupshared float4 tile[256];

uint2 levelSize(uint level)
{
    return max(params.size >> level, uint2(1u, 1u));
}

void store(uint level, uint2 texel, float4 value)
{
    if (level >= params.levelCount || any(texel >= levelSize(level)))
        return;
    // Constant indices once unrolled: no dynamic descriptor indexing.
    switch (level)
    {
    case 0u: levels[0][texel] = value; break;
    case 1u: levels[1][texel] = value; break;
    case 2u: levels[2][texel] = value; break;
    case 3u: levels[3][texel] = value; break;
    case 4u: levels[4][texel] = value; break;
    case 5u: levels[5][texel] = value; break;
    case 6u: levels[6][texel] = value; break;
    default: levels[7][texel] = value; break;
    }
}

[numthreads(16, 16, 1)]
[shader("compute")]
void main(uint3 groupId : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    const uint baseLevel = params.pass == 0u ? 0u : 4u;
    const uint2 texel = groupId.xy * 16u + localId.xy;
    const bool inside = all(texel < levelSize(baseLevel));
    float4 value = float4(0.0, 0.0, 0.0, 0.0);
    if (inside)
    {
        if (params.pass == 0u)
        {
            value = source.Load(int3(texel, 0));
            levels[0][texel] = value;
        }
        else
        {
            value = levels[4][texel];
        }
    }
    const uint index = localId.y * 16u + localId.x;
    tile[index] = value;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint step = 1u; step <= 4u; ++step)
    {
        const uint stride = 1u << step;
        const uint half = stride >> 1u;
        // Lanes on this level's grid average the 2x2 cells of the previous
        // one; the cells they read belong to lanes idle at this level.
        if (all((localId.xy & (stride - 1u)) == 0u))
        {
            value = 0.25 * (tile[index] + tile[index + half] + tile[index + half * 16u] + tile[index + half * 17u]);
            tile[index] = value;
            store(baseLevel + step, texel >> step, value);
        }
        GroupMemoryBarrierWithGroupSync();
    }
}
