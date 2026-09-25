import GI.ddgi_common;

float4 ddgiSampleProbe(Sampler2D atlas, uint probeIndex, float3 direction, uint interior) {
    const uint2 base = ddgiAtlasBase(probeIndex, interior);
    const float2 local = ddgiOctEncode(direction) * 0.5 + 0.5;
    const float2 pixel = float2(base) + 1.5 + local * float(interior - 1u);
    const float2 extent = float2(16u * (interior + 2u), 128u * (interior + 2u));
    return atlas.SampleLevel(pixel / extent, 0.0);
}

float4 ddgiGatherCascade(float3 position, float3 geometricNormal,
                         float3 viewDirection, float3 origin,
                         float spacing, int3 scrollOffset,
                         Sampler2D irradianceTexture, Sampler2D distanceTexture,
                         Sampler2D probeTexture) {
    const float3 cell = (position - origin) / spacing;
    if (any(cell < 0.0) || any(cell > float3(DDGI_COUNTS - 1u))) return float4(0.0);
    const uint3 lower = uint3(min(floor(cell), float3(DDGI_COUNTS - 2u)));
    const float3 fractional = saturate(cell - float3(lower));
    // Move the visibility query off the surface; keep the original position
    // for cell selection so the trilinear weights remain spatially stable.
    const float normalBias = min(spacing * 0.0375, 0.10);
    const float viewBias = min(spacing * 0.075, 0.20);
    const float3 biasedPosition = position + geometricNormal * normalBias +
                                  viewDirection * viewBias;
    float3 irradiance = 0.0;
    float totalWeight = 0.0;
    float validWeight = 0.0;
    for (uint corner = 0u; corner < 8u; ++corner) {
        const uint3 offset = uint3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
        const uint3 grid = lower + offset;
        const uint index = ddgiPhysicalIndex(grid, scrollOffset);
        const float4 probe = probeTexture.Load(int3(ddgiProbePixel(index), 0));
        if (probe.w < 0.5) continue;
        const float3 probePosition = origin + float3(grid) * spacing + probe.xyz;
        const float3 toSurface = biasedPosition - probePosition;
        const float distance = length(toSurface);
        const float3 direction = distance > 1e-4 ? toSurface / distance : geometricNormal;
        const float3 cornerWeight = lerp(1.0 - fractional, fractional, float3(offset));
        const float trilinearWeight = cornerWeight.x * cornerWeight.y * cornerWeight.z;
        const float4 probeIrradiance = ddgiSampleProbe(irradianceTexture, index, geometricNormal, DDGI_IRRADIANCE_INTERIOR);
        const float confidence = saturate(probeIrradiance.a);
        if (confidence <= 0.0) continue;
        // Volume coverage controls cascade blending independently of the
        // distance visibility used to interpolate the probe irradiance.
        validWeight += trilinearWeight * confidence;
        float weight = max(dot(geometricNormal, -direction) * 0.5 + 0.5, 0.05) * confidence;
        const float2 moments = ddgiSampleProbe(distanceTexture, index, direction, DDGI_DISTANCE_INTERIOR).rg;
        const float variance = max(moments.y - moments.x * moments.x, 0.0001);
        const float delta = max(distance - moments.x, 0.0);
        const float chebyshev = saturate(variance / (variance + delta * delta));
        weight *= max(0.05, chebyshev * chebyshev * chebyshev);
        // Keep a fallback when every probe is occluded, while suppressing
        // light leaking from probes with only a small visibility weight.
        weight = max(weight, 0.000001);
        if (weight < 0.2) weight *= weight * weight / (0.2 * 0.2);
        weight *= trilinearWeight;
        irradiance += probeIrradiance.rgb * weight;
        totalWeight += weight;
    }
    const float edge = min(min(cell.x, float(DDGI_COUNTS.x - 1u) - cell.x),
        min(min(cell.y, float(DDGI_COUNTS.y - 1u) - cell.y),
            min(cell.z, float(DDGI_COUNTS.z - 1u) - cell.z)));
    const float transition = smoothstep(0.0, 2.0, edge);
    return float4(totalWeight > 0.0 ? irradiance / totalWeight : float3(0.0),
                  saturate(validWeight * 4.0) * transition);
}
