import Grass.GrassTypes;

// One dispatch group produces candidates for one spatial chunk.  A terrain
// height texture and density/biome maps are bound by the grass renderer; no
// CPU-side blade transforms are uploaded.
[[vk::binding(0, 0)]] StructuredBuffer<GrassChunkData> chunks;
[[vk::binding(1, 0)]] RWStructuredBuffer<GrassInstance> generatedInstances;
[[vk::binding(2, 0)]] Sampler2D heightMap;
[[vk::binding(3, 0)]] Sampler2D densityMap;

struct GrassGenerationUniforms
{
    uint chunkCount;
    uint heightMapWidth;
    uint heightMapHeight;
    uint maximumInstancesPerChunk;
    float2 terrainOrigin;
    float2 terrainSize;
    float minimumHeight;
    float heightRange;
};
[[vk::binding(4, 0)]] ConstantBuffer<GrassGenerationUniforms> generation;

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint chunkId = id.y;
    const uint localId = id.x;
    if (chunkId >= generation.chunkCount) return;
    GrassChunkData chunk = chunks[chunkId];
    if (localId >= min(chunk.candidateCount, generation.maximumInstancesPerChunk)) return;

    uint state = chunk.seed ^ (localId * 747796405u + 2891336453u);
    float2 position = chunk.origin + float2(grassRandom(state), grassRandom(state)) * chunk.size;
    float2 terrainUv = saturate((position - generation.terrainOrigin) / generation.terrainSize);
    uint2 densitySize;
    densityMap.GetDimensions(densitySize.x, densitySize.y);
    uint2 densityTexel = min(uint2(terrainUv * float2(densitySize)), densitySize - 1u);
    if (grassRandom(state) > densityMap.Load(int3(densityTexel, 0)).r * chunk.density) return;

    uint2 heightTexel = min(uint2(terrainUv * float2(generation.heightMapWidth,
                                                      generation.heightMapHeight)),
                            uint2(generation.heightMapWidth - 1u, generation.heightMapHeight - 1u));
    float height = generation.minimumHeight +
        heightMap.Load(int3(heightTexel, 0)).r * generation.heightRange;
    GrassInstance instance;
    instance.positionXZ = position;
    instance.heightRotation = grassPackUnorm16(
        (height - generation.minimumHeight) / max(generation.heightRange, 1.0e-5)) |
        (grassPackUnorm16(grassRandom(state)) << 16u);
    instance.scaleType = grassPackUnorm16(0.8 + grassRandom(state) * 0.4) |
                         ((chunk.grassType & 0xffffu) << 16u);
    generatedInstances[chunk.instanceOffset + localId] = instance;
}
