// One meshlet-culling workgroup owns one compact coarse-visible instance.
struct DispatchIndirectCommand { uint groupCountX; uint groupCountY; uint groupCountZ; };

[[vk::binding(0, 0)]] StructuredBuffer<uint> visibleInstanceCount;
[[vk::binding(1, 0)]] RWStructuredBuffer<DispatchIndirectCommand> command;

// maxComputeWorkGroupCount is only guaranteed to be 65535 per dimension:
// larger counts use rows of InstanceRow groups (meshlet_culling.slang).
static const uint InstanceRow = 32768u;

[numthreads(1, 1, 1)]
[shader("compute")]
void main() {
    const uint count = visibleInstanceCount[0];
    command[0] = DispatchIndirectCommand(min(count, InstanceRow), (count + InstanceRow - 1u) / InstanceRow, 1u);
}
