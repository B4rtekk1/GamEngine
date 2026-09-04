import Culling.CullingTypes;
import Grass.GrassTypes;

// Chunk first: inexpensive frustum/distance rejection precedes any optional
// instance/Hi-Z refinement.  A later pass consumes visibleInstances to build
// LOD-binned indirect commands.
[[vk::binding(0, 0)]] StructuredBuffer<GrassChunkData> chunks;
[[vk::binding(1, 0)]] StructuredBuffer<GrassInstance> instances;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> visibleInstances;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> visibleCount;
[[vk::binding(4, 0)]] ConstantBuffer<CullingUniformData> culling;

bool grassChunkOutsideFrustum(GrassChunkData chunk)
{
    bool outsideLeft = true, outsideRight = true, outsideBottom = true;
    bool outsideTop = true, outsideNear = true, outsideFar = true;
    for (uint i = 0; i < 8; ++i) {
        float3 point = lerp(chunk.boundsMin.xyz, chunk.boundsMax.xyz, UNIT_AABB_CORNERS[i]);
        float4 clip = mul(float4(point, 1.0), culling.viewProjection);
        outsideLeft = outsideLeft && clip.x < -clip.w;
        outsideRight = outsideRight && clip.x > clip.w;
        outsideBottom = outsideBottom && clip.y < -clip.w;
        outsideTop = outsideTop && clip.y > clip.w;
        outsideNear = outsideNear && clip.z < 0.0;
        outsideFar = outsideFar && clip.z > clip.w;
    }
    return outsideLeft || outsideRight || outsideBottom || outsideTop || outsideNear || outsideFar;
}

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    uint chunkId = id.y;
    uint localId = id.x;
    if (chunkId >= culling.objectCount) return;
    GrassChunkData chunk = chunks[chunkId];
    if (localId >= chunk.candidateCount || grassChunkOutsideFrustum(chunk)) return;
    GrassInstance instance = instances[chunk.instanceOffset + localId];
    float2 delta = instance.positionXZ - culling.cameraPosition.xz;
    if (dot(delta, delta) > chunk.maxDistance * chunk.maxDistance) return;
    uint destination;
    InterlockedAdd(visibleCount[0], 1u, destination);
    visibleInstances[destination] = chunk.instanceOffset + localId;
}
