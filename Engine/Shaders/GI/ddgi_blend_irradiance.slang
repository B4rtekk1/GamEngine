import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;

[[vk::binding(5, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> rayData;
[[vk::binding(6, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> irradianceAtlas;
[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(12, 1)]] StructuredBuffer<DDGIProbeUpdate> probeUpdateList;

groupshared uint rejectUpdate;

[numthreads(10, 10, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.z >= ddgiUpdateCount(ddgi)) return;
    const DDGIProbeUpdate update = probeUpdateList[id.z];
    const uint probeIndex = update.probeIndex;
    const uint2 atlasPixel = ddgiAtlasBase(probeIndex, DDGI_IRRADIANCE_INTERIOR) + id.xy;
    const float state = probeData[int2(ddgiProbePixel(probeIndex))].w;
    if (state < 0.5) {
        irradianceAtlas[int2(atlasPixel)] = float4(0.0);
        return;
    }
    const uint rayCount = uint(ddgi.scrollOffsetRays.w);
    if (id.x == 0u && id.y == 0u) {
        uint backfaceCount = 0u;
        for (uint rayIndex = 0u; rayIndex < rayCount; ++rayIndex)
            backfaceCount += rayData[int2(rayIndex, probeIndex)].a < 0.0 ? 1u : 0u;
        rejectUpdate = float(backfaceCount) / float(rayCount) >
            DDGI_RANDOM_BACKFACE_THRESHOLD ? 1u : 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (rejectUpdate != 0u) return;

    const uint2 texel = id.xy;
    const uint last = DDGI_IRRADIANCE_INTERIOR + 1u;
    const bool border = texel.x == 0u || texel.y == 0u ||
                        texel.x == last || texel.y == last;
    if (!border) {
        const float2 oct = (float2(texel - 1u) + 0.5) /
                           float(DDGI_IRRADIANCE_INTERIOR) * 2.0 - 1.0;
        const float3 direction = ddgiOctDecode(oct);
        float3 sum = 0.0;
        float weightSum = 0.0;
        const float4 rotation = ddgiRayRotation(uint(ddgi.scrollDeltaFrame.w), rayCount);
        for (uint rayIndex = 0u; rayIndex < rayCount; ++rayIndex) {
            const float4 sample = rayData[int2(rayIndex, probeIndex)];
            if (sample.a < 0.0) continue;
            const float3 rayDirection = ddgiRayDirection(rayIndex, rayCount, rotation);
            const float weight = max(dot(direction, rayDirection), 0.0);
            sum += sample.rgb * weight;
            weightSum += weight;
        }
        // The weighted mean is radiance. Its cosine-weighted hemisphere integral
        // is irradiance, so the atlas stores E for the forward Lambert term E / PI.
        const float3 irradiance = 3.14159265 * sum / max(weightSum, 1e-5);
        float3 blended = irradiance;
        // The first update has no valid history. The 0.97 weight is per frame,
        // and a large lighting change must converge faster.
        if (ddgi.distanceInitialized.y > 0.5 && state < 1.5) {
            const float3 previous = irradianceAtlas[int2(atlasPixel)].rgb;
            const float previousLuminance = dot(previous, float3(0.2126, 0.7152, 0.0722));
            const float change = dot(abs(irradiance - previous), float3(0.2126, 0.7152, 0.0722));
            float hysteresis = pow(0.97, float(update.framesSinceLastUpdate));
            if (change > max(0.5, previousLuminance * 0.75))
                hysteresis = min(hysteresis, 0.7);
            blended = lerp(irradiance, previous, hysteresis);
        }
        irradianceAtlas[int2(atlasPixel)] = float4(blended, 1.0);
    }

    AllMemoryBarrierWithGroupSync();
    if (border) {
        const uint2 source = ddgiAtlasBase(probeIndex, DDGI_IRRADIANCE_INTERIOR) +
                             ddgiBorderSource(texel, DDGI_IRRADIANCE_INTERIOR);
        irradianceAtlas[int2(atlasPixel)] = irradianceAtlas[int2(source)];
    }
}
