struct GPUSceneInstanceRecord { float4x4 worldMatrix; float4 localBoundsMin; float4 localBoundsMax; uint4 idsAndFlags; };
struct ScatterUniforms { uint visibleCapacity; uint maxBins; uint _pad0; uint _pad1; float4 cameraPosition; };
[[vk::binding(0, 0)]] StructuredBuffer<GPUSceneInstanceRecord> instances;
[[vk::binding(1, 0)]] StructuredBuffer<uint> visibleInstanceIds;
[[vk::binding(2, 0)]] StructuredBuffer<uint> visibleCount;
[[vk::binding(3, 0)]] StructuredBuffer<uint> binOffsets;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> binCursors;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> compactRendererIndices;
[[vk::binding(6, 0)]] ConstantBuffer<ScatterUniforms> uniforms;

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    uint visibleIndex = id.x;
    if (visibleIndex >= min(visibleCount[0], uniforms.visibleCapacity)) return;
    GPUSceneInstanceRecord instance = instances[visibleInstanceIds[visibleIndex]];
    if ((instance.idsAndFlags.w & 4u) == 0u) return;
    float3 center = mul(float4((instance.localBoundsMin.xyz + instance.localBoundsMax.xyz) * 0.5, 1.0),
                        instance.worldMatrix).xyz;
    float distanceToCamera = length(center - uniforms.cameraPosition.xyz);
    uint lod = distanceToCamera > 60.0 ? 2u : (distanceToCamera > 28.0 ? 1u : 0u);
    uint bin = instance.idsAndFlags.x * 3u + lod;
    if (bin >= uniforms.maxBins) return;
    uint localOffset;
    InterlockedAdd(binCursors[bin], 1u, localOffset);
    compactRendererIndices[binOffsets[bin] + localOffset] = instance.idsAndFlags.z;
}
