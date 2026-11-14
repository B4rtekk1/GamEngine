// The CPU writes PTLAS operation records (WRITE_INSTANCE or UPDATE_INSTANCE)
// with the cluster BLAS index in their acceleration-structure field. Cluster
// BLASes are built with implicit destinations, so only the GPU knows their
// addresses; this replaces each index with the address the build reported.
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> records;
[[vk::binding(1, 0)]] StructuredBuffer<uint2> blasAddresses;
// recordWords: 26 for VkPartitionedAccelerationStructureWriteInstanceDataNV,
// 4 for ...UpdateInstanceDataNV; addressWord: where the 64-bit address starts.
struct Settings { uint recordCount; uint recordWords; uint addressWord; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;

[numthreads(64, 1, 1)] [shader("compute")]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= settings.recordCount) return;
    const uint base = id.x * settings.recordWords + settings.addressWord;
    // ~0u marks an instance without a cluster BLAS; address 0 disables it.
    const uint blas = records[base];
    const uint2 address = blas == 0xffffffffu ? uint2(0u, 0u) : blasAddresses[blas];
    records[base] = address.x;
    records[base + 1u] = address.y;
}
