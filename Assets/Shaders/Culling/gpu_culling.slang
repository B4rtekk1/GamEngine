import Culling.CullingTypes;

[[vk::binding(0, 0)]] StructuredBuffer<CullingObjectData> objects;
[[vk::binding(1, 0)]] RWStructuredBuffer<IndexedDrawCommand> commands;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> drawCounts;
[[vk::binding(3, 0)]] Sampler2D hiZTexture;
[[vk::binding(4, 0)]] ConstantBuffer<CullingUniformData> culling;

void appendDraw(CullingObjectData objectData)
{
    uint destinationIndex;
    InterlockedAdd(drawCounts[0], 1, destinationIndex);
    if (destinationIndex >= culling.maxDrawCount)
        return;

    IndexedDrawCommand command;
    command.indexCount = objectData.indexCount;
    command.instanceCount = objectData.instanceCount;
    command.firstIndex = objectData.firstIndex;
    command.vertexOffset = int(objectData.vertexOffset);
    command.firstInstance = objectData.firstInstance;
    commands[destinationIndex] = command;
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
    if (culling.enableFrustumCulling == 0)
    {
        appendDraw(objectData);
        return;
    }

    float3 localMinimum = objectData.localAabbMin.xyz - culling.aabbExpansion;
    float3 localMaximum = objectData.localAabbMax.xyz + culling.aabbExpansion;
    float4 clipPositions[8];
    for (uint i = 0; i < 8; ++i)
    {
        float3 localPosition = lerp(localMinimum, localMaximum, UNIT_AABB_CORNERS[i]);
        float4 worldPosition = mul(float4(localPosition, 1.0), objectData.model);
        clipPositions[i] = mul(worldPosition, culling.viewProjection);
    }

    if (outsideFrustum(clipPositions))
        return;

    bool occluded = false;
    if (culling.enableOcclusionCulling != 0 && culling.cameraCut == 0)
        occluded = occlusionTest(clipPositions);
    if (!occluded)
        appendDraw(objectData);
}
