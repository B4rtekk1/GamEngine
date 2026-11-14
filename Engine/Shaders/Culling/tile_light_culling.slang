// Per-tile local light lists bounded by the depth buffer (Forward+). After
// the depth prepass, each 16x16 pixel tile takes the view-space box between
// its nearest and farthest opaque depth and keeps the lights whose range
// sphere reaches that box. A log-depth cluster spans a whole depth slice of
// a 64-pixel tile, so most of its lights could not reach any of its pixels;
// forward shading walked them all. Unordered lists: forward shading does
// not depend on the order.
import Common.SceneData;

[[vk::binding(0, 0)]] Sampler2D depthTexture;
[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2, 0)]] StructuredBuffer<LocalLightData> localLights;
// Per tile, TileLightStride entries: the count, then the light indices.
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> tileLights;

groupshared uint tileMinDepth;
groupshared uint tileMaxDepth;
groupshared uint tileCount;
groupshared uint tileList[MaxTileLights];

float3 viewPositionAt(float2 uv, float depth)
{
    const float4 position = mul(float4(uv * 2.0 - 1.0, depth, 1.0), frame.inverseProjection);
    return position.xyz / position.w;
}

[numthreads(TileLightPixelSize, TileLightPixelSize, 1)]
[shader("compute")]
void main(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex)
{
    if (groupIndex == 0u)
    {
        tileMinDepth = 0x7f7fffffu;
        tileMaxDepth = 0u;
        tileCount = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    uint width, height;
    depthTexture.GetDimensions(width, height);
    const int2 pixel = int2(groupId.xy * TileLightPixelSize + threadId.xy);
    if (pixel.x < int(width) && pixel.y < int(height))
    {
        // Positive floats order like their bit patterns.
        const float depth = depthTexture.Load(int3(pixel, 0)).x;
        if (depth < 0.999999)
        {
            InterlockedMin(tileMinDepth, asuint(depth));
            InterlockedMax(tileMaxDepth, asuint(depth));
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const uint tilesX = (width + TileLightPixelSize - 1u) / TileLightPixelSize;
    const uint tileBase = (groupId.y * tilesX + groupId.x) * TileLightStride;
    if (tileMinDepth > tileMaxDepth)
    {
        // Sky only: nothing is shaded here.
        if (groupIndex == 0u)
            tileLights[tileBase] = 0u;
        return;
    }

    // View-space box of the tile's pixels between its depth bounds.
    const float2 uvMin = float2(groupId.xy * TileLightPixelSize) / float2(width, height);
    const float2 uvMax = min(float2((groupId.xy + 1u) * TileLightPixelSize) / float2(width, height), 1.0);
    const float nearDepth = asfloat(tileMinDepth);
    const float farDepth = asfloat(tileMaxDepth);
    float3 boxMin = 1e30;
    float3 boxMax = -1e30;
    [unroll] for (uint corner = 0u; corner < 8u; ++corner)
    {
        const float2 uv = float2((corner & 1u) != 0u ? uvMax.x : uvMin.x, (corner & 2u) != 0u ? uvMax.y : uvMin.y);
        const float3 position = viewPositionAt(uv, (corner & 4u) != 0u ? farDepth : nearDepth);
        boxMin = min(boxMin, position);
        boxMax = max(boxMax, position);
    }

    for (uint lightIndex = groupIndex; lightIndex < frame.localLightCount;
         lightIndex += TileLightPixelSize * TileLightPixelSize)
    {
        const float4 positionRange = localLights[lightIndex].positionRange;
        const float3 center = mul(float4(positionRange.xyz, 1.0), frame.view).xyz;
        const float3 offset = clamp(center, boxMin, boxMax) - center;
        if (dot(offset, offset) >= positionRange.w * positionRange.w)
            continue;
        uint slot;
        InterlockedAdd(tileCount, 1u, slot);
        if (slot < MaxTileLights)
            tileList[slot] = lightIndex;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint count = min(tileCount, MaxTileLights);
    for (uint entry = groupIndex; entry < count; entry += TileLightPixelSize * TileLightPixelSize)
        tileLights[tileBase + 1u + entry] = tileList[entry];
    if (groupIndex == 0u)
        tileLights[tileBase] = count;
}
