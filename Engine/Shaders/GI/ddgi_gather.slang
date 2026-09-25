import GI.ddgi_common;

[[vk::binding(20, 0)]] Sampler2D ddgiIrradiance;
[[vk::binding(21, 0)]] Sampler2D ddgiDistance;
[[vk::binding(22, 0)]] Sampler2D ddgiProbeData;
[[vk::binding(23, 0)]] Sampler2D ddgiIrradiance1;
[[vk::binding(24, 0)]] Sampler2D ddgiDistance1;
[[vk::binding(25, 0)]] Sampler2D ddgiProbeData1;
[[vk::binding(26, 0)]] Sampler2D ddgiIrradiance2;
[[vk::binding(27, 0)]] Sampler2D ddgiDistance2;
[[vk::binding(28, 0)]] Sampler2D ddgiProbeData2;

float4 ddgiSampleProbe(Sampler2D atlas, uint probeIndex, float3 direction, uint interior) {
    const uint2 base = ddgiAtlasBase(probeIndex, interior);
    const float2 local = ddgiOctEncode(direction) * 0.5 + 0.5;
    const float2 pixel = float2(base) + 1.5 + local * float(interior - 1u);
    const float2 extent = float2(16u * (interior + 2u), 128u * (interior + 2u));
    return atlas.SampleLevel(pixel / extent, 0.0);
}

float4 ddgiGatherCascade(float3 position, float3 normal, float3 origin,
                         float spacing, int3 scrollOffset,
                         Sampler2D irradianceTexture, Sampler2D distanceTexture,
                         Sampler2D probeTexture) {
    const float3 cell = (position - origin) / spacing;
    if (any(cell < 0.0) || any(cell > float3(DDGI_COUNTS - 1u))) return float4(0.0);
    const uint3 lower = uint3(min(floor(cell), float3(DDGI_COUNTS - 2u)));
    const float3 fractional = saturate(cell - float3(lower));
    float3 irradiance = 0.0;
    float totalWeight = 0.0;
    for (uint corner = 0u; corner < 8u; ++corner) {
        const uint3 offset = uint3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
        const uint3 grid = lower + offset;
        const uint index = ddgiPhysicalIndex(grid, scrollOffset);
        const float4 probe = probeTexture.Load(int3(ddgiProbePixel(index), 0));
        if (probe.w < 0.5) continue;
        const float3 probePosition = origin + float3(grid) * spacing + probe.xyz;
        const float3 toSurface = position - probePosition;
        const float distance = length(toSurface);
        const float3 direction = distance > 1e-4 ? toSurface / distance : normal;
        const float3 cornerWeight = lerp(1.0 - fractional, fractional, float3(offset));
        float weight = cornerWeight.x * cornerWeight.y * cornerWeight.z;
        weight *= max(dot(normal, -direction) * 0.5 + 0.5, 0.05);
        const float4 probeIrradiance = ddgiSampleProbe(irradianceTexture, index, normal, DDGI_IRRADIANCE_INTERIOR);
        weight *= saturate(probeIrradiance.a);
        const float2 moments = ddgiSampleProbe(distanceTexture, index, direction, DDGI_DISTANCE_INTERIOR).rg;
        const float variance = max(moments.y - moments.x * moments.x, 0.0001);
        const float delta = max(distance - moments.x, 0.0);
        const float visibility = saturate(variance / (variance + delta * delta));
        // Preserve occlusion: a nonzero visibility floor would let probes
        // contribute through a wall even when the distance moments reject them.
        weight *= visibility * visibility;
        irradiance += probeIrradiance.rgb * weight;
        totalWeight += weight;
    }
    const float edge = min(min(cell.x, float(DDGI_COUNTS.x - 1u) - cell.x),
        min(min(cell.y, float(DDGI_COUNTS.y - 1u) - cell.y),
            min(cell.z, float(DDGI_COUNTS.z - 1u) - cell.z)));
    const float transition = smoothstep(0.0, 2.0, edge);
    return float4(irradiance / max(totalWeight, 0.0001),
                  saturate(totalWeight * 4.0) * transition);
}
