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

static const uint MaxLightsPerCluster = 64u;

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
        if (affectsCluster(frame.localLights[lightIndex], id) && count < MaxLightsPerCluster)
            clusterLightIndices[offset + count++] = lightIndex;
    }
    ClusterLightRange range;
    range.offset = offset;
    range.count = count;
    clusterRanges[clusterIndex] = range;
}
