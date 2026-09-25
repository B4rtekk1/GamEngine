import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;

[[vk::binding(5, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> rayData;
[[vk::binding(6, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> irradianceAtlas;
[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(12, 1)]] StructuredBuffer<uint> probeUpdateList;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= 10u || id.y >= 10u || id.z >= ddgiUpdateCount(ddgi)) return;
    const uint probeIndex = probeUpdateList[id.z];
    const uint2 atlasPixel = ddgiAtlasBase(probeIndex, DDGI_IRRADIANCE_INTERIOR) + id.xy;
    const float state = probeData[int2(ddgiProbePixel(probeIndex))].w;
    if (state < 0.5) {
        irradianceAtlas[int2(atlasPixel)] = float4(0.0);
        return;
    }
    const uint2 texel = id.xy;
    const float2 oct = (float2(clamp(int2(texel) - 1, int2(0, 0), int2(7, 7))) + 0.5) / 8.0 * 2.0 - 1.0;
    const float3 direction = ddgiOctDecode(oct);
    float3 sum = 0.0;
    float weightSum = 0.0;
    for (uint rayIndex = 0u; rayIndex < uint(ddgi.scrollOffsetRays.w); ++rayIndex) {
        const float3 rayDirection = ddgiRayDirection(rayIndex, uint(ddgi.scrollDeltaFrame.w), uint(ddgi.scrollOffsetRays.w));
        const float weight = max(dot(direction, rayDirection), 0.0);
        const float4 sample = rayData[int2(rayIndex, probeIndex)];
        sum += sample.rgb * weight;
        weightSum += weight;
    }
    // The weighted mean is radiance. Its cosine-weighted hemisphere integral
    // is irradiance, so the atlas stores E for the forward Lambert term E / PI.
    const float3 irradiance = 3.14159265 * sum / max(weightSum, 1e-5);
    float3 blended = irradiance;
    // The first update has no valid history. A large lighting change must
    // converge faster than the normal 0.97 temporal accumulation.
    if (ddgi.distanceInitialized.y > 0.5 && state < 1.5) {
        const float3 previous = irradianceAtlas[int2(atlasPixel)].rgb;
        const float previousLuminance = dot(previous, float3(0.2126, 0.7152, 0.0722));
        const float change = dot(abs(irradiance - previous), float3(0.2126, 0.7152, 0.0722));
        const float hysteresis = change > max(0.5, previousLuminance * 0.75) ? 0.7 : 0.97;
        blended = lerp(irradiance, previous, hysteresis);
    }
    irradianceAtlas[int2(atlasPixel)] = float4(blended, 1.0);
}
