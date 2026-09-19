// One meshlet-culling workgroup owns one compact coarse-visible instance.
struct DispatchIndirectCommand { uint groupCountX; uint groupCountY; uint groupCountZ; };

[[vk::binding(0, 0)]] StructuredBuffer<uint> visibleInstanceCount;
[[vk::binding(1, 0)]] RWStructuredBuffer<DispatchIndirectCommand> command;

[numthreads(1, 1, 1)]
[shader("compute")]
void main() {
    command[0] = DispatchIndirectCommand(visibleInstanceCount[0], 1u, 1u);
}
