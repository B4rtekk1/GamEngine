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
// Every local light; frame.localLights holds only the first 32.
[[vk::binding(5, 0)]] StructuredBuffer<LocalLightData> localLights;

static const uint MaxLightsPerCluster = 64u;
static const uint MaxLocalLightsPerCluster = 48u;
static const uint MaxProbesPerCluster = MaxLightsPerCluster - MaxLocalLightsPerCluster;
static const uint Probe_Enabled = 1u << 2u;
static const float ClusterTilePixels = float(ClusterTilePixelSize);

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

    // Bound the sphere by its view-space box. Each lateral extreme is taken
    // at the box's near or far face, whichever maximises it, so the result
    // stays conservative off-axis (center +- f*r/(d-r) does not).
    const float nearDepth = max(depth - radius, nearPlane);
    const float farDepth = max(depth + radius, nearPlane);
    const float2 low = viewPosition.xy - radius;
    const float2 high = viewPosition.xy + radius;
    const float2 tangentLow = low / select(low < 0.0, nearDepth, farDepth);
    const float2 tangentHigh = high / select(high > 0.0, nearDepth, farDepth);
    const float2 focal = float2(uniforms.projection[0][0], uniforms.projection[1][1]);
    const float2 ndcA = tangentLow * focal;
    const float2 ndcB = tangentHigh * focal;
    const float2 sphereMin = min(ndcA, ndcB);
    const float2 sphereMax = max(ndcA, ndcB);
    // Fragments pick tiles of ClusterTilePixels from the top-left, so the
    // last row and column extend past the viewport; map through pixels
    // rather than dividing the viewport evenly by the grid size.
    const float2 viewport = uniforms.viewportNearFar.xy;
    const float2 minNdc = float2(cluster.xy) * ClusterTilePixels / viewport * 2.0 - 1.0;
    const float2 maxNdc = float2(cluster.xy + 1u) * ClusterTilePixels / viewport * 2.0 - 1.0;
    return all(sphereMax >= minNdc) && all(sphereMin <= maxNdc);
}

// View-space bounding sphere of one cluster: the frustum slab between its
// two depth planes, spanned by the pixel tile it covers.
float4 clusterBoundingSphere(uint3 cluster)
{
    const float nearPlane = uniforms.viewportNearFar.z;
    const float farPlane = uniforms.viewportNearFar.w;
    const float z0 = nearPlane * pow(farPlane / nearPlane, float(cluster.z) / float(uniforms.gridAndLightCount.z));
    const float z1 = nearPlane * pow(farPlane / nearPlane, float(cluster.z + 1u) / float(uniforms.gridAndLightCount.z));
    const float2 viewport = uniforms.viewportNearFar.xy;
    const float2 focal = float2(uniforms.projection[0][0], uniforms.projection[1][1]);
    const float2 ndcMin = float2(cluster.xy) * ClusterTilePixels / viewport * 2.0 - 1.0;
    const float2 ndcMax = float2(cluster.xy + 1u) * ClusterTilePixels / viewport * 2.0 - 1.0;
    // A view-space xy is ndc / focal * depth; take the extremes over both planes.
    const float2 a = ndcMin / focal, b = ndcMax / focal;
    const float2 tangentMin = min(a, b), tangentMax = max(a, b);
    const float3 boxMin = float3(min(tangentMin * z0, tangentMin * z1), -z1);
    const float3 boxMax = float3(max(tangentMax * z0, tangentMax * z1), -z0);
    return float4((boxMin + boxMax) * 0.5, length(boxMax - boxMin) * 0.5);
}

// Spot lights pass the sphere test for every cluster within their range,
// including those behind or beside the beam. Reject a cluster whose bounding
// sphere lies wholly outside the outer cone (Wronski, "Cull that cone").
bool spotMissesCluster(LocalLightData light, float4 clusterSphere)
{
    const float3 origin = mul(float4(light.positionRange.xyz, 1.0), uniforms.view).xyz;
    const float3 axis = normalize(mul(float4(light.directionOuterCos.xyz, 0.0), uniforms.view).xyz);
    const float cosAngle = light.directionOuterCos.w;
    const float sinAngle = sqrt(saturate(1.0 - cosAngle * cosAngle));
    const float3 toCenter = clusterSphere.xyz - origin;
    const float alongAxis = dot(toCenter, axis);
    const float lengthSquared = dot(toCenter, toCenter);
    const float closest = cosAngle * sqrt(max(lengthSquared - alongAxis * alongAxis, 0.0)) - alongAxis * sinAngle;
    return closest > clusterSphere.w || alongAxis > clusterSphere.w + light.positionRange.w ||
           alongAxis < -clusterSphere.w;
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
    uint lightCount = 0u;
    const float4 clusterSphere = clusterBoundingSphere(id);
    // Hundreds of lights are tested per cluster; stop once the list is full.
    for (uint lightIndex = 0u; lightIndex < uniforms.gridAndLightCount.w && lightCount < MaxLocalLightsPerCluster;
         ++lightIndex)
    {
        const LocalLightData light = localLights[lightIndex];
        if (!affectsCluster(light, id))
            continue;
        if (light.parameters.y > 1.5 && spotMissesCluster(light, clusterSphere)) // Spot
            continue;
        clusterLightIndices[offset + lightCount++] = lightIndex;
    }
    const uint probeOffset = offset + MaxLocalLightsPerCluster;
    uint probeCount = 0u;
    for (uint probeIndex = 0u; probeIndex < frame.reflectionProbeCount; ++probeIndex)
    {
        if (affectsCluster(reflectionProbes[probeIndex], id) && probeCount < MaxProbesPerCluster)
            clusterLightIndices[probeOffset + probeCount++] = probeIndex;
    }
    ClusterLightRange range;
    range.lightOffset = offset;
    range.lightCount = lightCount;
    range.probeOffset = probeOffset;
    range.probeCount = probeCount;
    clusterRanges[clusterIndex] = range;
}
