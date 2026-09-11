import Culling.CullingTypes;

struct GPUSceneInstanceRecord
{
    float4x4 worldMatrix;
    float4 localBoundsMin;
    float4 localBoundsMax;
    uint4 idsAndFlags;
};

[[vk::binding(0, 0)]] StructuredBuffer<GPUSceneInstanceRecord> instances;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> visibleInstances;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> visibleCount;
[[vk::binding(3, 0)]] ConstantBuffer<CullingUniformData> culling;

bool outsideFrustum(float4 clipPositions[8])
{
    bool outsideLeft = true, outsideRight = true, outsideBottom = true;
    bool outsideTop = true, outsideNear = true, outsideFar = true;
    for (uint i = 0; i < 8; ++i) {
        float4 clip = clipPositions[i];
        outsideLeft = outsideLeft && clip.x < -clip.w;
        outsideRight = outsideRight && clip.x > clip.w;
        outsideBottom = outsideBottom && clip.y < -clip.w;
        outsideTop = outsideTop && clip.y > clip.w;
        outsideNear = outsideNear && clip.z < 0.0;
        outsideFar = outsideFar && clip.z > clip.w;
    }
    return outsideLeft || outsideRight || outsideBottom || outsideTop || outsideNear || outsideFar;
}

bool worldAabbOutsideFrustum(GPUSceneInstanceRecord instance)
{
    const float3 localCenter = (instance.localBoundsMin.xyz + instance.localBoundsMax.xyz) * 0.5;
    const float3 localExtent = (instance.localBoundsMax.xyz - instance.localBoundsMin.xyz) * 0.5;
    const float3 worldCenter = mul(float4(localCenter, 1.0), instance.worldMatrix).xyz;
    const float3 worldExtent =
        abs(mul(float4(localExtent.x, 0.0, 0.0, 0.0), instance.worldMatrix).xyz) +
        abs(mul(float4(0.0, localExtent.y, 0.0, 0.0), instance.worldMatrix).xyz) +
        abs(mul(float4(0.0, 0.0, localExtent.z, 0.0), instance.worldMatrix).xyz);
    for (uint planeIndex = 0; planeIndex < 6; ++planeIndex) {
        const float4 plane = culling.frustumPlanes[planeIndex];
        if (dot(plane.xyz, worldCenter) + plane.w + dot(abs(plane.xyz), worldExtent) < 0.0)
            return true;
    }
    return false;
}

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint instanceId = dispatchThreadId.x;
    uint instanceCount, instanceStride;
    instances.GetDimensions(instanceCount, instanceStride);

    // All lanes must reach the wave operations below.  In particular, the
    // final, partially populated workgroup cannot return early before its
    // subgroup has collectively reserved output storage.
    bool visible = false;
    if (instanceId < instanceCount)
    {
        const GPUSceneInstanceRecord instance = instances[instanceId];
        // Bit 0 is the renderer visibility/alive bit; inactive slots never enter the list.
        visible = (instance.idsAndFlags.w & 1U) != 0U;
        // Terrain grass is already submitted in terrain chunks.  Until the
        // GPU-scene bounds path owns those chunk bounds, do not apply the
        // per-blade frustum test here: a mismatched extracted bound must never
        // make an entire grass field disappear in the game view.
        const bool isGrass = (instance.idsAndFlags.w & 4u) != 0u;
        if (visible && !isGrass && culling.enableFrustumCulling != 0u)
            visible = !worldAabbOutsideFrustum(instance);
    }

    // Compact visible IDs per implementation-defined subgroup. This uses one
    // global atomic per subgroup, without assuming a 32-lane NVIDIA warp.
    const uint visibleInWave = WaveActiveCountBits(visible);
    uint waveBase = 0;
    if (WaveIsFirstLane() && visibleInWave != 0)
        InterlockedAdd(visibleCount[0], visibleInWave, waveBase);
    waveBase = WaveReadLaneFirst(waveBase);

    if (!visible) return;

    const uint destination = waveBase + WavePrefixCountBits(visible);
    uint maxVisibleInstances, visibleStride;
    visibleInstances.GetDimensions(maxVisibleInstances, visibleStride);
    // Keep the GPU-scene ID here: the grass binning pass needs mesh/type flags
    // before it emits the final compact renderer-instance indices.
    if (destination < maxVisibleInstances) visibleInstances[destination] = instanceId;
}
