struct PrefixUniforms { uint binCount; uint compactBase; uint _pad0; uint _pad1; };
[[vk::binding(0, 0)]] StructuredBuffer<uint> binCounts;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> binOffsets;
[[vk::binding(2, 0)]] ConstantBuffer<PrefixUniforms> uniforms;

// Bin count is bounded by grass meshes × LODs, so a single GPU invocation is
// cheaper and simpler than allocating a CPU readback or an oversized bin grid.
[numthreads(1, 1, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    uint offset = uniforms.compactBase;
    for (uint bin = 0; bin < uniforms.binCount; ++bin) {
        binOffsets[bin] = offset;
        offset += binCounts[bin];
    }
}
