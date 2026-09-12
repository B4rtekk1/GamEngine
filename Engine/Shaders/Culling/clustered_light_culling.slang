import Common.SceneData;

// One invocation owns one 3D view cluster.  The list has a fixed, bounded
// stride; overflow is deliberately clamped so malformed content cannot write
// beyond its SSBO.  A production scene should expose this overflow count in
// renderer diagnostics rather than silently increasing fragment work.

struct ClusteredLightingUniforms
{
    float4x4 view;
    float4x4 projection;
    uint4 gridAndLightCount;
    float4 viewportNearFar;
};

[[vk::binding(0, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(1, 0)]] RWStructuredBuffer<ClusterLightRange> clusterRanges;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> clusterLightIndices;
[[vk::binding(3, 0)]] ConstantBuffer<ClusteredLightingUniforms> uniforms;
struct ReflectionProbeData
{
    float4 positionAndBlendDistance;
    float4 extentsAndShape;
    uint textureIndex;
    int priority;
    uint flags;
    uint padding;
};
[[vk::binding(4, 0)]] StructuredBuffer<ReflectionProbeData> reflectionProbes;

static const uint MaxLightsPerCluster = 64u;
static const uint MaxLocalLightsPerCluster = 48u;
static const uint MaxProbesPerCluster = MaxLightsPerCluster - MaxLocalLightsPerCluster;
static const uint Probe_Enabled = 1u << 2u;
static const uint ProbeIndexBit = 0x80000000u;

// Conservative sphere-vs-cluster test in view space.  Every local light is
// represented by its influence sphere; spot lights can safely use the same
// broad phase and retain their cone test in the forward shader.
bool affectsCluster(LocalLightData light, uint3 cluster)
{
    float3 viewPosition = mul(float4(light.positionRange.xyz, 1.0), uniforms.view).xyz;
    float radius = light.positionRange.w;
    float nearPlane = uniforms.viewportNearFar.z;
    float farPlane = uniforms.viewportNearFar.w;
    float z0 = nearPlane * pow(farPlane / nearPlane, float(cluster.z) / float(uniforms.gridAndLightCount.z));
    float z1 = nearPlane * pow(farPlane / nearPlane, float(cluster.z + 1u) / float(uniforms.gridAndLightCount.z));
    float depth = -viewPosition.z;
    if (depth + radius < z0 || depth - radius > z1) return false;

    float4 clip = mul(float4(viewPosition, 1.0), uniforms.projection);
    if (clip.w <= 1e-5) return depth + radius >= z0;
    float2 center = clip.xy / clip.w;
    // Project the sphere radius conservatively using the focal lengths.
    float2 extent = abs(float2(uniforms.projection[0][0], uniforms.projection[1][1])) * radius / max(depth - radius, nearPlane);
    float2 minNdc = (float2(cluster.xy) / float2(uniforms.gridAndLightCount.xy)) * 2.0 - 1.0;
    float2 maxNdc = (float2(cluster.xy + 1u) / float2(uniforms.gridAndLightCount.xy)) * 2.0 - 1.0;
    return center.x + extent.x >= minNdc.x && center.x - extent.x <= maxNdc.x &&
           center.y + extent.y >= minNdc.y && center.y - extent.y <= maxNdc.y;
}

// A box is conservatively assigned through its bounding sphere.  The forward
// shader still evaluates the exact box/sphere volume and blend distance.
bool affectsCluster(ReflectionProbeData probe, uint3 cluster)
{
    if ((probe.flags & Probe_Enabled) == 0u) return false;
    LocalLightData bound;
    bound.positionRange = float4(probe.positionAndBlendDistance.xyz,
        max(length(probe.extentsAndShape.xyz), 1e-4));
    return affectsCluster(bound, cluster);
}

[numthreads(4, 4, 4)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= uniforms.gridAndLightCount.xyz)) return;
    uint clusterIndex = (id.z * uniforms.gridAndLightCount.y + id.y) * uniforms.gridAndLightCount.x + id.x;
    uint offset = clusterIndex * MaxLightsPerCluster;
    uint count = 0u;
    for (uint lightIndex = 0u; lightIndex < uniforms.gridAndLightCount.w; ++lightIndex)
    {
        if (affectsCluster(frame.localLights[lightIndex], id) && count < MaxLocalLightsPerCluster)
            clusterLightIndices[offset + count++] = lightIndex;
    }
    const uint probeStart = count;
    for (uint probeIndex = 0u; probeIndex < frame.reflectionProbeCount; ++probeIndex)
    {
        if (affectsCluster(reflectionProbes[probeIndex], id) && count - probeStart < MaxProbesPerCluster)
            clusterLightIndices[offset + count++] = ProbeIndexBit | probeIndex;
    }
    ClusterLightRange range;
    range.offset = offset;
    range.count = count;
    clusterRanges[clusterIndex] = range;
}
