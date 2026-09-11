import Culling.CullingTypes;

[[vk::binding(0, 0)]] StructuredBuffer<CullingObjectData> objects;
[[vk::binding(1, 0)]] RWStructuredBuffer<IndexedDrawCommand> commands;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> drawCounts;
[[vk::binding(3, 0)]] Sampler2D hiZTexture;
[[vk::binding(4, 0)]] ConstantBuffer<CullingUniformData> culling;
// Shadow culling uses these bindings to build one compact caster list per
// clip level, then consumes the relevant list for each virtual page.
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> candidateObjectIndices;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> candidateCounts;
// Three uints per clip level, matching VkDispatchIndirectCommand.
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> candidateDispatchArguments;
struct ShadowPageWork
{
    float4x4 viewProjection;
    uint drawSlot;
    uint clipLevel;
    // These fields are deliberately carried through culling even though that
    // pass only needs the matrix. The graphics pass and the later page-table
    // commit consume the same GPU work-list entry, so a physical tile can
    // never be confused with a stale virtual mapping.
    uint physicalPage;
    uint virtualPage;
};
[[vk::binding(8, 0)]] StructuredBuffer<ShadowPageWork> shadowPageWork;

struct CullingPushConstants
{
    float4x4 viewProjectionOverride;
    uint drawSlot;
    uint sourceCount;
    uint candidateLevel;
    // 0: direct object input, 1: emit clip-level candidates,
    // 2: consume the candidate list for a virtual-shadow page.
    uint mode;
    uint shaderFilter;
};

[[vk::push_constant]] ConstantBuffer<CullingPushConstants> pushConstants;

void appendDraw(bool visible, CullingObjectData objectData, uint drawSlot)
{
    // Reserve one contiguous range per wave.  Calling this from every active
    // lane is important: wave operations must not be nested in divergent flow.
    const uint visibleInWave = WaveActiveCountBits(visible);
    uint waveBase = 0;
    if (WaveIsFirstLane() && visibleInWave != 0)
        InterlockedAdd(drawCounts[drawSlot], visibleInWave, waveBase);
    waveBase = WaveReadLaneFirst(waveBase);
    if (!visible)
        return;

    const uint destinationIndex = waveBase + WavePrefixCountBits(visible);
    if (destinationIndex >= culling.maxDrawCount)
        return;

    IndexedDrawCommand command;
    command.indexCount = objectData.indexCount;
    if (culling.shadowPass == 0 && objectData.lod1IndexCount != 0)
    {
        // Bounds are stored in object-local space, while the culling camera is
        // in world space. Compare positions in the same space so LOD choice is
        // independent of the object's transform.
        float3 localCenter = (objectData.localAabbMin.xyz + objectData.localAabbMax.xyz) * 0.5;
        float3 worldCenter = mul(float4(localCenter, 1.0), objectData.model).xyz;
        float distanceToCamera = length(worldCenter - culling.cameraPosition.xyz);
        if (distanceToCamera >= objectData.lod2Distance && objectData.lod2IndexCount != 0)
            command.indexCount = objectData.lod2IndexCount;
        else if (distanceToCamera >= objectData.lod1Distance)
            command.indexCount = objectData.lod1IndexCount;
    }
    command.instanceCount = objectData.instanceCount;
    command.firstIndex = objectData.firstIndex;
    command.vertexOffset = int(objectData.vertexOffset);
    command.firstInstance = objectData.firstInstance;
    commands[drawSlot * culling.maxDrawCount + destinationIndex] = command;
}

void appendBinnedDraw(bool visible, CullingObjectData objectData)
{
    // Visibility/LOD/Hi-Z are evaluated once per object.  Iterate bins only
    // for wave compaction: every lane enters each wave intrinsic uniformly.
    // This trades N complete scene scans for one scan plus cheap wave ballots.
    for (uint bin = 0; bin < 32; ++bin)
    {
        const bool inBin = visible && objectData.shader == bin;
        const uint visibleInWave = WaveActiveCountBits(inBin);
        uint waveBase = 0;
        if (WaveIsFirstLane() && visibleInWave != 0)
            InterlockedAdd(drawCounts[bin], visibleInWave, waveBase);
        waveBase = WaveReadLaneFirst(waveBase);
        if (!inBin)
            continue;

        const uint destinationIndex = waveBase + WavePrefixCountBits(inBin);
        if (destinationIndex >= culling.maxDrawCount)
            continue;

        IndexedDrawCommand command;
        command.indexCount = objectData.indexCount;
        if (culling.shadowPass == 0 && objectData.lod1IndexCount != 0)
        {
            float3 localCenter = (objectData.localAabbMin.xyz + objectData.localAabbMax.xyz) * 0.5;
            float3 worldCenter = mul(float4(localCenter, 1.0), objectData.model).xyz;
            float distanceToCamera = length(worldCenter - culling.cameraPosition.xyz);
            if (distanceToCamera >= objectData.lod2Distance && objectData.lod2IndexCount != 0)
                command.indexCount = objectData.lod2IndexCount;
            else if (distanceToCamera >= objectData.lod1Distance)
                command.indexCount = objectData.lod1IndexCount;
        }
        command.instanceCount = objectData.instanceCount;
        command.firstIndex = objectData.firstIndex;
        command.vertexOffset = int(objectData.vertexOffset);
        command.firstInstance = objectData.firstInstance;
        commands[bin * culling.maxDrawCount + destinationIndex] = command;
    }
}

void appendCandidate(bool visible, uint objectIndex)
{
    const uint visibleInWave = WaveActiveCountBits(visible);
    uint waveBase = 0;
    if (WaveIsFirstLane() && visibleInWave != 0)
        InterlockedAdd(candidateCounts[pushConstants.candidateLevel], visibleInWave, waveBase);
    waveBase = WaveReadLaneFirst(waveBase);
    if (!visible)
        return;

    const uint destinationIndex = waveBase + WavePrefixCountBits(visible);
    if (destinationIndex < culling.maxDrawCount)
        candidateObjectIndices[pushConstants.candidateLevel * culling.maxDrawCount + destinationIndex] = objectIndex;
}

bool outsideFrustum(float4 clipPositions[8])
{
    bool outsideLeft = true;
    bool outsideRight = true;
    bool outsideBottom = true;
    bool outsideTop = true;
    bool outsideNear = true;
    bool outsideFar = true;

    for (uint i = 0; i < 8; ++i)
    {
        float4 clip = clipPositions[i];
        // A bound intersecting the eye/near plane cannot be classified by
        // homogeneous clip comparisons alone. Keep it conservatively: the
        // rasterizer will perform the exact clipping.
        if (clip.w <= 0.00001 || clip.z <= 0.0)
            return false;
        outsideLeft = outsideLeft && clip.x < -clip.w;
        outsideRight = outsideRight && clip.x > clip.w;
        outsideBottom = outsideBottom && clip.y < -clip.w;
        outsideTop = outsideTop && clip.y > clip.w;
        outsideNear = outsideNear && clip.z < 0.0;
        outsideFar = outsideFar && clip.z > clip.w;
    }
    return outsideLeft || outsideRight || outsideBottom ||
           outsideTop || outsideNear || outsideFar;
}

// Converts a local AABB into a conservative world-space center/extent pair.
// Transforming the three local extent axes separately works for arbitrary
// affine transforms (including rotations and non-uniform scale).
bool worldAabbOutsideFrustum(CullingObjectData objectData, float boundsPadding)
{
    const float3 localMinimum = objectData.localAabbMin.xyz - boundsPadding;
    const float3 localMaximum = objectData.localAabbMax.xyz + boundsPadding;
    const float3 localCenter = (localMinimum + localMaximum) * 0.5;
    const float3 localExtent = (localMaximum - localMinimum) * 0.5;
    const float3 worldCenter = mul(float4(localCenter, 1.0), objectData.model).xyz;
    const float3 worldExtent =
        abs(mul(float4(localExtent.x, 0.0, 0.0, 0.0), objectData.model).xyz) +
        abs(mul(float4(0.0, localExtent.y, 0.0, 0.0), objectData.model).xyz) +
        abs(mul(float4(0.0, 0.0, localExtent.z, 0.0), objectData.model).xyz);
    for (uint planeIndex = 0; planeIndex < 6; ++planeIndex)
    {
        const float4 plane = culling.frustumPlanes[planeIndex];
        if (dot(plane.xyz, worldCenter) + plane.w + dot(abs(plane.xyz), worldExtent) < 0.0)
            return true;
    }
    return false;
}

bool occlusionTest(float4 clipPositions[8])
{
    float2 minimumNdc = 1.0;
    float2 maximumNdc = -1.0;
    float nearestObjectDepth = 1.0;

    for (uint i = 0; i < 8; ++i)
    {
        float4 clip = clipPositions[i];
        if (clip.w <= 0.00001 || clip.z <= 0.0)
            return false;

        float3 ndc = clip.xyz / clip.w;
        minimumNdc = min(minimumNdc, ndc.xy);
        maximumNdc = max(maximumNdc, ndc.xy);
        nearestObjectDepth = min(nearestObjectDepth, ndc.z);
    }

    minimumNdc = clamp(minimumNdc, -1.0, 1.0);
    maximumNdc = clamp(maximumNdc, -1.0, 1.0);
    float2 minimumUv = minimumNdc * 0.5 + 0.5;
    float2 maximumUv = maximumNdc * 0.5 + 0.5;
    minimumUv.y = 1.0 - minimumUv.y;
    maximumUv.y = 1.0 - maximumUv.y;
    float temporaryY = minimumUv.y;
    minimumUv.y = min(temporaryY, maximumUv.y);
    maximumUv.y = max(temporaryY, maximumUv.y);
    minimumUv = clamp(minimumUv, 0.0, 1.0);
    maximumUv = clamp(maximumUv, 0.0, 1.0);

    float2 rectanglePixels = (maximumUv - minimumUv) *
                             float2(culling.viewportWidth, culling.viewportHeight);
    float maximumExtent = max(rectanglePixels.x, rectanglePixels.y);
    float selectedMipFloat = ceil(log2(max(maximumExtent, 1.0)));
    uint selectedMip = uint(clamp(selectedMipFloat, 0.0,
                                  float(culling.hiZMipCount - 1)));

    uint mipWidth;
    uint mipHeight;
    uint mipCount;
    hiZTexture.GetDimensions(selectedMip, mipWidth, mipHeight, mipCount);
    int2 mipSize = int2(mipWidth, mipHeight);
    int2 minimumTexel = int2(floor(minimumUv * float2(mipSize)));
    int2 maximumTexel = int2(floor(maximumUv * float2(mipSize)));
    minimumTexel = clamp(minimumTexel, int2(0), mipSize - 1);
    maximumTexel = clamp(maximumTexel, int2(0), mipSize - 1);

    float regionDepth = 0.0;
    for (int y = minimumTexel.y; y <= maximumTexel.y; ++y)
        for (int x = minimumTexel.x; x <= maximumTexel.x; ++x)
            regionDepth = max(regionDepth,
                              hiZTexture.Load(int3(x, y, int(selectedMip))).r);

    return nearestObjectDepth > regionDepth + culling.depthBias;
}

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (pushConstants.mode == 4)
    {
        if (dispatchThreadId.x == 0)
        {
            const uint argumentOffset = pushConstants.candidateLevel * 3;
            candidateDispatchArguments[argumentOffset] =
                (candidateCounts[pushConstants.candidateLevel] + 63) / 64;
            candidateDispatchArguments[argumentOffset + 1] = 1;
            candidateDispatchArguments[argumentOffset + 2] = 1;
        }
        return;
    }
    const bool batchedShadowPages = pushConstants.mode == 5;
    // Binding 8 always contains at least one inert entry for non-shadow uses.
    const ShadowPageWork pageWork = shadowPageWork[batchedShadowPages ? dispatchThreadId.y : 0];
    const uint sourceIndex = dispatchThreadId.x;
    uint sourceCount = pushConstants.sourceCount;
    const uint candidateLevel = batchedShadowPages ? pageWork.clipLevel : pushConstants.candidateLevel;
    if (pushConstants.mode == 2 || batchedShadowPages)
        sourceCount = min(sourceCount, candidateCounts[candidateLevel]);
    bool visible = false;
    uint objectIndex = 0;
    CullingObjectData objectData = (CullingObjectData)0;

    if (sourceIndex < sourceCount) {
        objectIndex = (pushConstants.mode == 2 || batchedShadowPages)
            ? candidateObjectIndices[candidateLevel * culling.maxDrawCount + sourceIndex]
            : sourceIndex;
        if (objectIndex < culling.objectCount) {
            objectData = objects[objectIndex];
            visible = !(culling.shadowPass != 0 && objectData.castShadow == 0) &&
                      !(culling.drawCategory != 2 && objectData.twoSided != culling.drawCategory) &&
                      (pushConstants.shaderFilter == 0xffffffff ||
                       objectData.shader == pushConstants.shaderFilter);
        }
    }

    if (visible && culling.enableFrustumCulling != 0)
    {

    // A larger guard band for singleton draws makes their Hi-Z result robust
    // at shallow angles, while still allowing them to benefit from occlusion.
    const float boundsScale = objectData.instanceCount == 1 ? 0.05 : 0.02;
    const float boundsPadding = max(culling.aabbExpansion,
                                    length(objectData.localAabbMax.xyz - objectData.localAabbMin.xyz) * boundsScale);
    // Virtual-shadow candidate passes supply a per-clip/page matrix through
    // push constants. Its planes are not available in the uniform buffer, so
    // retain the homogeneous-corner test on that path.
    if (culling.shadowPass != 0)
    {
        const float3 localMinimum = objectData.localAabbMin.xyz - boundsPadding;
        const float3 localMaximum = objectData.localAabbMax.xyz + boundsPadding;
        float4 clipPositions[8];
        for (uint i = 0; i < 8; ++i)
        {
            const float3 localPosition = lerp(localMinimum, localMaximum, UNIT_AABB_CORNERS[i]);
            const float4 worldPosition = mul(float4(localPosition, 1.0), objectData.model);
            clipPositions[i] = mul(worldPosition, batchedShadowPages
                ? pageWork.viewProjection : pushConstants.viewProjectionOverride);
        }
        visible = !outsideFrustum(clipPositions);
    }
    else
    {
        visible = !worldAabbOutsideFrustum(objectData, boundsPadding);

        // Only the objects reaching Hi-Z need projected corners.
        if (visible && culling.enableOcclusionCulling != 0 && culling.cameraCut == 0)
        {
            const float3 localMinimum = objectData.localAabbMin.xyz - boundsPadding;
            const float3 localMaximum = objectData.localAabbMax.xyz + boundsPadding;
            float4 clipPositions[8];
            for (uint i = 0; i < 8; ++i)
            {
                const float3 localPosition = lerp(localMinimum, localMaximum, UNIT_AABB_CORNERS[i]);
                const float4 worldPosition = mul(float4(localPosition, 1.0), objectData.model);
                clipPositions[i] = mul(worldPosition, culling.viewProjection);
            }
            visible = !occlusionTest(clipPositions);
        }
    }
    }

    // mode is a dispatch-wide push constant, hence every active lane enters
    // exactly one compaction routine and its wave operations stay uniform.
    if (pushConstants.mode == 1)
        appendCandidate(visible, objectIndex);
    else if (pushConstants.mode == 3)
        appendBinnedDraw(visible, objectData);
    else
        appendDraw(visible, objectData,
                   batchedShadowPages ? pageWork.drawSlot : pushConstants.drawSlot);
}
