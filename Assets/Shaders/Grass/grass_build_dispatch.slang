// Converts compact-list counters into VkDispatchIndirectCommand records.
// Each command occupies three uints: groupCountX, groupCountY, groupCountZ.
[[vk::binding(0, 0)]] StructuredBuffer<uint> counts;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> commands;

[numthreads(1, 1, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    // Keeping three records also makes the visible-count version usable for
    // classify (record 0) and the classify-count version for all streams.
    for (uint stream = 0; stream < 3; ++stream) {
        commands[stream * 3 + 0] = (counts[stream] + 63u) / 64u;
        commands[stream * 3 + 1] = 1u;
        commands[stream * 3 + 2] = 1u;
    }
}
