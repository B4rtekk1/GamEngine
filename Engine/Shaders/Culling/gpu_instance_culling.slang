import Culling.CullingTypes;

struct GPUVisibilityInstanceRecord
{
    float4 worldCenterRadius;
    uint4 idsAndFlags;
};

[[vk::binding(0, 0)]] StructuredBuffer<GPUVisibilityInstanceRecord> instances;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> visibleInstances;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> visibleCount;
[[vk::binding(3, 0)]] ConstantBuffer<CullingUniformData> culling;

bool outsideFrustum(GPUVisibilityInstanceRecord instance)
{
    for (uint planeIndex = 0; planeIndex < 6; ++planeIndex) {
        const float4 plane = culling.frustumPlanes[planeIndex];
        if (dot(plane.xyz, instance.worldCenterRadius.xyz) + plane.w < -instance.worldCenterRadius.w)
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
        const GPUVisibilityInstanceRecord instance = instances[instanceId];
        // Bit 0 is the renderer visibility/alive bit; inactive slots never enter the list.
        visible = (instance.idsAndFlags.w & 1U) != 0U;
        // Terrain grass is already submitted in terrain chunks.  Until the
        // GPU-scene bounds path owns those chunk bounds, do not apply the
        // per-blade frustum test here: a mismatched extracted bound must never
        // make an entire grass field disappear in the game view.
        const bool isGrass = (instance.idsAndFlags.w & 4u) != 0u;
        if (visible && !isGrass && culling.enableFrustumCulling != 0u)
            visible = !outsideFrustum(instance);
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
