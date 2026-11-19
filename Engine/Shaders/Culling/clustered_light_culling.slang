import Common.SceneData;

// Builds the light and reflection-probe list of every 3D view cluster.  The
// list has a fixed, bounded stride; overflow is deliberately clamped so malformed content cannot write
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

// One workgroup per screen tile walks every depth slice of that tile.
// The lateral (xy) part of the sphere test depends only on the light and the
// tile, and only the depth interval and the spot cone depend on the slice.
// A group per cluster re-read every light and redid the lateral test for each
// of the 24 slices: with ~160 lights at 1080p that was ~190 MB of L1/L2
// traffic per frame and bound the pass on memory, not on ALU.
static const uint ClusterCullThreads = 64u;
// Slice hit masks are 32 bits wide; ClusterDepthSlices is 24.
static const uint MaxClusterSlices = 32u;

// Depth of every slice plane of the grid: slice s spans [planes[s], planes[s + 1]].
groupshared float slicePlanes[MaxClusterSlices + 1u];
// View-space bounding sphere of every cluster of this tile, for the spot test.
groupshared float4 sliceSpheres[MaxClusterSlices];
// Per slice, which threads of the current batch hit it (two 32-bit words).
groupshared uint sliceHits[MaxClusterSlices][ClusterCullThreads / 32u];

struct TileBounds
{
    float2 minNdc;
    float2 maxNdc;
    float2 focal;
};

TileBounds makeTileBounds(uint2 tile)
{
    TileBounds bounds;
    // Fragments pick tiles of ClusterTilePixels from the top-left, so the
    // last row and column extend past the viewport; map through pixels
    // rather than dividing the viewport evenly by the grid size.
    const float2 viewport = uniforms.viewportNearFar.xy;
    bounds.minNdc = float2(tile) * ClusterTilePixels / viewport * 2.0 - 1.0;
    bounds.maxNdc = float2(tile + 1u) * ClusterTilePixels / viewport * 2.0 - 1.0;
    bounds.focal = float2(uniforms.projection[0][0], uniforms.projection[1][1]);
    return bounds;
}

float slicePlaneDepth(uint plane)
{
    const float nearPlane = uniforms.viewportNearFar.z;
    const float farPlane = uniforms.viewportNearFar.w;
    return nearPlane * pow(farPlane / nearPlane, float(plane) / float(uniforms.gridAndLightCount.z));
}

// View-space bounding sphere of one cluster: the frustum slab between its
// two depth planes, spanned by the pixel tile it covers.
float4 clusterBoundingSphere(TileBounds bounds, float z0, float z1)
{
    // A view-space xy is ndc / focal * depth; take the extremes over both planes.
    const float2 a = bounds.minNdc / bounds.focal, b = bounds.maxNdc / bounds.focal;
    const float2 tangentMin = min(a, b), tangentMax = max(a, b);
    const float3 boxMin = float3(min(tangentMin * z0, tangentMin * z1), -z1);
    const float3 boxMax = float3(max(tangentMax * z0, tangentMax * z1), -z0);
    return float4((boxMin + boxMax) * 0.5, length(boxMax - boxMin) * 0.5);
}

// Conservative sphere-vs-tile test in view space.  Every local light is
// represented by its influence sphere; spot lights can safely use the same
// broad phase and retain their cone test in the forward shader.
bool overlapsTile(float3 viewPosition, float radius, TileBounds bounds)
{
    const float nearPlane = uniforms.viewportNearFar.z;
    const float depth = -viewPosition.z;
    // Bound the sphere by its view-space box. Each lateral extreme is taken
    // at the box's near or far face, whichever maximises it, so the result
    // stays conservative off-axis (center +- f*r/(d-r) does not).
    const float nearDepth = max(depth - radius, nearPlane);
    const float farDepth = max(depth + radius, nearPlane);
    const float2 low = viewPosition.xy - radius;
    const float2 high = viewPosition.xy + radius;
    const float2 tangentLow = low / select(low < 0.0, nearDepth, farDepth);
    const float2 tangentHigh = high / select(high > 0.0, nearDepth, farDepth);
    const float2 ndcA = tangentLow * bounds.focal;
    const float2 ndcB = tangentHigh * bounds.focal;
    const float2 sphereMin = min(ndcA, ndcB);
    const float2 sphereMax = max(ndcA, ndcB);
    return all(sphereMax >= bounds.minNdc) && all(sphereMin <= bounds.maxNdc);
}

// Slices of this tile whose depth interval the sphere overlaps, as a bit mask.
uint sliceMask(float3 viewPosition, float radius, uint sliceCount)
{
    const float depth = -viewPosition.z;
    uint mask = 0u;
    for (uint slice = 0u; slice < sliceCount; ++slice)
        if (!(depth + radius < slicePlanes[slice] || depth - radius > slicePlanes[slice + 1u]))
            mask |= 1u << slice;
    return mask;
}

// Spot lights pass the sphere test for every cluster within their range,
// including those behind or beside the beam. Reject a cluster whose bounding
// sphere lies wholly outside the outer cone (Wronski, "Cull that cone").
bool spotMissesCluster(float3 origin, float3 axis, float cosAngle, float sinAngle, float range,
                       float4 clusterSphere)
{
    const float3 toCenter = clusterSphere.xyz - origin;
    const float alongAxis = dot(toCenter, axis);
    const float lengthSquared = dot(toCenter, toCenter);
    const float closest = cosAngle * sqrt(max(lengthSquared - alongAxis * alongAxis, 0.0)) - alongAxis * sinAngle;
    return closest > clusterSphere.w || alongAxis > clusterSphere.w + range || alongAxis < -clusterSphere.w;
}

uint lightSlices(LocalLightData light, TileBounds bounds, uint sliceCount)
{
    const float3 viewPosition = mul(float4(light.positionRange.xyz, 1.0), uniforms.view).xyz;
    const float radius = light.positionRange.w;
    uint mask = sliceMask(viewPosition, radius, sliceCount);
    if (mask == 0u || !overlapsTile(viewPosition, radius, bounds))
        return 0u;
    if (light.parameters.y > 1.5) // Spot
    {
        const float3 axis = normalize(mul(float4(light.directionOuterCos.xyz, 0.0), uniforms.view).xyz);
        const float cosAngle = light.directionOuterCos.w;
        const float sinAngle = sqrt(saturate(1.0 - cosAngle * cosAngle));
        for (uint remaining = mask; remaining != 0u; remaining &= remaining - 1u)
        {
            const uint slice = firstbitlow(remaining);
            if (spotMissesCluster(viewPosition, axis, cosAngle, sinAngle, radius, sliceSpheres[slice]))
                mask &= ~(1u << slice);
        }
    }
    return mask;
}

// A box is conservatively assigned through its bounding sphere.  The forward
// shader still evaluates the exact box/sphere volume and blend distance.
uint probeSlices(ReflectionProbeData probe, TileBounds bounds, uint sliceCount)
{
    if ((probe.flags & Probe_Enabled) == 0u) return 0u;
    const float3 viewPosition = mul(float4(probe.positionAndBlendDistance.xyz, 1.0), uniforms.view).xyz;
    const float radius = max(length(probe.extentsAndShape.xyz), 1e-4);
    const uint mask = sliceMask(viewPosition, radius, sliceCount);
    return mask != 0u && overlapsTile(viewPosition, radius, bounds) ? mask : 0u;
}

// Publishes this thread's batch hits, then lets thread `slice` append the
// hits of its slice in thread order. Lists therefore keep ascending light
// order and the same first-N overflow cut as a serial walk. Returns the
// slice's new count (only meaningful on threads below sliceCount).
uint appendBatch(uint thread, uint mask, uint sliceCount, uint batch, uint listStart, uint listCapacity,
                 uint count, uint3 grid, uint2 tile)
{
    // Thread `slice` both clears and later reads sliceHits[slice]; the two
    // barriers below order its clear against every thread's OR.
    if (thread < sliceCount)
    {
        sliceHits[thread][0] = 0u;
        sliceHits[thread][1] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint remaining = mask; remaining != 0u; remaining &= remaining - 1u)
        InterlockedOr(sliceHits[firstbitlow(remaining)][thread / 32u], 1u << (thread & 31u));
    GroupMemoryBarrierWithGroupSync();
    if (thread < sliceCount)
    {
        const uint offset = ((thread * grid.y + tile.y) * grid.x + tile.x) * MaxLightsPerCluster + listStart;
        [unroll]
        for (uint word = 0u; word < ClusterCullThreads / 32u; ++word)
        {
            for (uint bits = sliceHits[thread][word]; bits != 0u && count < listCapacity; bits &= bits - 1u)
            {
                clusterLightIndices[offset + count] = batch + word * 32u + firstbitlow(bits);
                ++count;
            }
        }
    }
    return count;
}

[numthreads(ClusterCullThreads, 1, 1)]
[shader("compute")]
void main(uint3 groupId : SV_GroupID, uint thread : SV_GroupThreadID)
{
    const uint3 grid = uniforms.gridAndLightCount.xyz;
    const uint2 tile = groupId.xy;
    if (any(tile >= grid.xy)) return;
    const uint sliceCount = min(grid.z, MaxClusterSlices);
    const TileBounds bounds = makeTileBounds(tile);
    if (thread <= sliceCount)
        slicePlanes[thread] = slicePlaneDepth(thread);
    GroupMemoryBarrierWithGroupSync();
    if (thread < sliceCount)
        sliceSpheres[thread] = clusterBoundingSphere(bounds, slicePlanes[thread], slicePlanes[thread + 1u]);
    GroupMemoryBarrierWithGroupSync();

    // Thread `slice` owns that cluster's counts.
    const uint localLightCount = uniforms.gridAndLightCount.w;
    uint lightCount = 0u;
    for (uint batch = 0u; batch < localLightCount; batch += ClusterCullThreads)
    {
        const uint lightIndex = batch + thread;
        const uint mask = lightIndex < localLightCount ? lightSlices(localLights[lightIndex], bounds, sliceCount) : 0u;
        lightCount = appendBatch(thread, mask, sliceCount, batch, 0u, MaxLocalLightsPerCluster, lightCount, grid, tile);
    }
    uint probeCount = 0u;
    for (uint batch = 0u; batch < frame.reflectionProbeCount; batch += ClusterCullThreads)
    {
        const uint probeIndex = batch + thread;
        const uint mask =
            probeIndex < frame.reflectionProbeCount ? probeSlices(reflectionProbes[probeIndex], bounds, sliceCount) : 0u;
        probeCount = appendBatch(thread, mask, sliceCount, batch, MaxLocalLightsPerCluster, MaxProbesPerCluster,
                                 probeCount, grid, tile);
    }
    if (thread < sliceCount)
    {
        const uint clusterIndex = (thread * grid.y + tile.y) * grid.x + tile.x;
        const uint offset = clusterIndex * MaxLightsPerCluster;
        ClusterLightRange range;
        range.lightOffset = offset;
        range.lightCount = lightCount;
        range.probeOffset = offset + MaxLocalLightsPerCluster;
        range.probeCount = probeCount;
        clusterRanges[clusterIndex] = range;
    }
}
