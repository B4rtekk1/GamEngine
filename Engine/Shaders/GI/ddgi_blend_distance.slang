import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;

[[vk::binding(5, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> rayData;
[[vk::binding(7, 1), vk::image_format("rg16f")]] RWTexture2D<float2> distanceAtlas;
[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(12, 1)]] StructuredBuffer<uint> probeUpdateList;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= 18u || id.y >= 18u || id.z >= ddgiUpdateCount(ddgi)) return;
    const uint probeIndex = probeUpdateList[id.z];
    const uint2 atlasPixel = ddgiAtlasBase(probeIndex, DDGI_DISTANCE_INTERIOR) + id.xy;
    const float state = probeData[int2(ddgiProbePixel(probeIndex))].w;
    if (state < 0.5) {
        distanceAtlas[int2(atlasPixel)] = float2(0.0);
        return;
    }
    const uint2 texel = id.xy;
    const float2 oct = (float2(clamp(int2(texel) - 1, int2(0, 0), int2(15, 15))) + 0.5) / 16.0 * 2.0 - 1.0;
    const float3 direction = ddgiOctDecode(oct);
    float mean = 0.0;
    float meanSquared = 0.0;
    float weightSum = 0.0;
    for (uint rayIndex = 0u; rayIndex < uint(ddgi.scrollOffsetRays.w); ++rayIndex) {
        const float3 rayDirection = ddgiRayDirection(rayIndex, uint(ddgi.scrollDeltaFrame.w), uint(ddgi.scrollOffsetRays.w));
        const float weight = pow(max(dot(direction, rayDirection), 0.0), 8.0);
        const float distance = abs(rayData[int2(rayIndex, probeIndex)].a);
        mean += distance * weight;
        meanSquared += distance * distance * weight;
        weightSum += weight;
    }
    const float2 result = float2(mean, meanSquared) / max(weightSum, 1e-5);
    float2 blended = result;
    if (ddgi.distanceInitialized.y > 0.5 && state < 1.5)
        blended = lerp(result, distanceAtlas[int2(atlasPixel)], 0.97);
    distanceAtlas[int2(atlasPixel)] = blended;
}
