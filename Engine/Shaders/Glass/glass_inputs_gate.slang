// GlassPass's indirect arguments for this frame: the depth seed draw and the
// snapshot and depth pyramid dispatches, zeroed when GPU culling left no
// glass draw in either stream. Most frames see no pane, and the inputs
// (a mip-mapped colour copy and a depth pyramid at full resolution) cost as
// much as a small glass draw.
//
// gate: words 0 and 1 hold the two streams' glass draw counts (copied in by
// GlassPass); entry i then fills words 4 + 4i .. 7 + 4i. Entry 0 is a
// VkDrawIndirectCommand, the others VkDispatchIndirectCommands.
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> gate;

struct GateConstants
{
    uint entries;
    // Scalars: a uint3 would align to 16 and move `groups` to offset 32.
    uint padding0;
    uint padding1;
    uint padding2;
    // Two (x, y) group counts per element, in recording order.
    uint4 groups[6];
};
[[vk::push_constant]] ConstantBuffer<GateConstants> constants;

[numthreads(16, 1, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint entry = id.x;
    if (entry >= constants.entries)
        return;
    const bool visible = (gate[0] | gate[1]) != 0u;
    const uint4 pair = constants.groups[entry >> 1u];
    const uint2 size = (entry & 1u) != 0u ? pair.zw : pair.xy;
    const uint base = 4u + entry * 4u;
    // Draw: vertexCount, instanceCount, firstVertex, firstInstance.
    // Dispatch: x, y, z (and an unused word).
    gate[base] = visible ? size.x : 0u;
    gate[base + 1u] = size.y;
    gate[base + 2u] = entry == 0u ? 0u : 1u;
    gate[base + 3u] = 0u;
}
