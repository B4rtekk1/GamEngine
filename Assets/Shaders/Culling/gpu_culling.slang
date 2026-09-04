import Culling.CullingTypes;

[[vk::binding(0, 0)]] StructuredBuffer<CullingObjectData> objects;
[[vk::binding(1, 0)]] RWStructuredBuffer<IndexedDrawCommand> commands;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> drawCounts;
[[vk::binding(3, 0)]] Sampler2D hiZTexture;
[[vk::binding(4, 0)]] ConstantBuffer<CullingUniformData> culling;

struct CullingPushConstants
{
    float4x4 viewProjectionOverride;
    uint drawSlot;
};

[[vk::push_constant]] ConstantBuffer<CullingPushConstants> pushConstants;

void appendDraw(CullingObjectData objectData)
{
    uint destinationIndex;
    InterlockedAdd(drawCounts[pushConstants.drawSlot], 1, destinationIndex);
    if (destinationIndex >= culling.maxDrawCount)
        return;

    IndexedDrawCommand command;
    command.indexCount = objectData.indexCount;
    if (culling.shadowPass == 0 && objectData.lod1IndexCount != 0)
    {
        float3 center = (objectData.localAabbMin.xyz + objectData.localAabbMax.xyz) * 0.5;
        float distanceToCamera = length(center - culling.cameraPosition.xyz);
        if (distanceToCamera >= objectData.lod2Distance && objectData.lod2IndexCount != 0)
            command.indexCount = objectData.lod2IndexCount;
        else if (distanceToCamera >= objectData.lod1Distance)
            command.indexCount = objectData.lod1IndexCount;
    }
    command.instanceCount = objectData.instanceCount;
    command.firstIndex = objectData.firstIndex;
    command.vertexOffset = int(objectData.vertexOffset);
    command.firstInstance = objectData.firstInstance;
    commands[pushConstants.drawSlot * culling.maxDrawCount + destinationIndex] = command;
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
    uint objectIndex = dispatchThreadId.x;
    if (objectIndex >= culling.objectCount)
        return;

    CullingObjectData objectData = objects[objectIndex];
    if (culling.shadowPass != 0 && objectData.castShadow == 0)
        return;
    if (culling.drawCategory != 2 && objectData.twoSided != culling.drawCategory)
        return;
    if (culling.enableFrustumCulling == 0)
    {
        appendDraw(objectData);
        return;
    }

    // A fixed centimetre-scale padding is insufficient for large, rotated
    // meshes at the frustum edge (for example ramps).  Keep culling
    // conservative by scaling the guard band with the extracted AABB.
    const float boundsPadding = max(culling.aabbExpansion,
                                    length(objectData.localAabbMax.xyz - objectData.localAabbMin.xyz) * 0.02);
    float3 localMinimum = objectData.localAabbMin.xyz - boundsPadding;
    float3 localMaximum = objectData.localAabbMax.xyz + boundsPadding;
    float4 clipPositions[8];
    for (uint i = 0; i < 8; ++i)
    {
        float3 localPosition = lerp(localMinimum, localMaximum, UNIT_AABB_CORNERS[i]);
        float4 worldPosition = mul(float4(localPosition, 1.0), objectData.model);
        float4x4 viewProjection = culling.shadowPass != 0
            ? pushConstants.viewProjectionOverride : culling.viewProjection;
        clipPositions[i] = mul(worldPosition, viewProjection);
    }

    if (outsideFrustum(clipPositions))
        return;

    bool occluded = false;
    // Single-instance static meshes receive little benefit from Hi-Z, while
    // their small projected rectangles are the most susceptible to false
    // occlusion (notably ramps viewed at shallow angles).  Keep frustum
    // culling, but reserve Hi-Z for batched geometry.
    if (objectData.instanceCount > 1 && culling.enableOcclusionCulling != 0 && culling.cameraCut == 0)
        occluded = occlusionTest(clipPositions);
    if (!occluded)
        appendDraw(objectData);
}
