import Culling.CullingTypes;
import Grass.GrassTypes;

// Consumes the compact GPU-scene visibility list and emits fixed-capacity
// mesh/LOD bins. firstInstance addresses the compact renderer-index list,
// not the legacy instance table.
struct GPUSceneInstanceRecord { float4x4 worldMatrix; float4 localBoundsMin; float4 localBoundsMax; uint4 idsAndFlags; };
struct GPUSceneMeshRecord { uint4 draw; uint4 lod; };
struct GrassIndirectUniforms { uint visibleCapacity; uint binCapacity; uint compactBase; uint maxBins; float4 cameraPosition; };
[[vk::binding(0, 0)]] StructuredBuffer<GPUSceneInstanceRecord> instances;
[[vk::binding(1, 0)]] StructuredBuffer<uint> visibleInstanceIds;
[[vk::binding(2, 0)]] StructuredBuffer<uint> visibleCount;
[[vk::binding(3, 0)]] StructuredBuffer<GPUSceneMeshRecord> meshes;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> binCounts;
[[vk::binding(5, 0)]] ConstantBuffer<GrassIndirectUniforms> uniforms;

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    uint visibleIndex = id.x;
    uint count = min(visibleCount[0], uniforms.visibleCapacity);
    if (visibleIndex >= count) return;
    GPUSceneInstanceRecord instance = instances[visibleInstanceIds[visibleIndex]];
    // Flag bit 2 (value 4) identifies grass/foliage in the GPU scene.
    if ((instance.idsAndFlags.w & 4u) == 0u) return;
    GPUSceneMeshRecord mesh = meshes[instance.idsAndFlags.x];
    float3 center = mul(float4((instance.localBoundsMin.xyz + instance.localBoundsMax.xyz) * 0.5, 1.0),
                        instance.worldMatrix).xyz;
    float distanceToCamera = length(center - uniforms.cameraPosition.xyz);
    uint lod = distanceToCamera > 60.0 ? 2u : (distanceToCamera > 28.0 ? 1u : 0u);
    uint bin = instance.idsAndFlags.x * 3u + lod;
    if (bin >= uniforms.maxBins) return;
    uint slot;
    InterlockedAdd(binCounts[bin], 1u, slot);
}
