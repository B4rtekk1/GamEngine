import GI.ddgi_common;

float4 ddgiSampleProbeOct(Sampler2D atlas, uint probeIndex, float2 oct, uint interior) {
    const uint2 base = ddgiAtlasBase(probeIndex, interior);
    const float tileSize = float(interior + 2u);
    const float2 pixel = float2(base) + tileSize * 0.5 + oct * (float(interior) * 0.5);
    const float2 extent = float2(16u * (interior + 2u), 128u * (interior + 2u));
    return atlas.SampleLevel(pixel / extent, 0.0);
}

struct DDGIGatherResult {
    float3 irradiance;
    float blendWeight;
    float visibility;
    float activeProbeFraction;
    float preVisibilityWeight;
    float postVisibilityWeight;
};

float ddgiCellTransition(float3 cell) {
    if (any(cell < 0.0) || any(cell > float3(DDGI_COUNTS - 1u))) return 0.0;
    const float edge = min(min(cell.x, float(DDGI_COUNTS.x - 1u) - cell.x),
        min(min(cell.y, float(DDGI_COUNTS.y - 1u) - cell.y),
            min(cell.z, float(DDGI_COUNTS.z - 1u) - cell.z)));
    return smoothstep(0.0, 2.0, edge);
}

float ddgiCascadeTransition(float3 position, float3 origin, float spacing) {
    if (spacing <= 0.0) return 0.0;
    return ddgiCellTransition((position - origin) / spacing);
}

DDGIGatherResult ddgiGatherCascade(float3 position, float3 geometricNormal,
                         float3 irradianceNormal, float3 viewDirection, float3 origin,
                         float spacing, int3 scrollOffset,
                         Sampler2D irradianceTexture, Sampler2D distanceTexture,
                         Sampler2D probeTexture) {
    const float3 cell = (position - origin) / spacing;
    DDGIGatherResult result;
    result.irradiance = 0.0;
    result.blendWeight = 0.0;
    result.visibility = 0.0;
    result.activeProbeFraction = 0.0;
    result.preVisibilityWeight = 0.0;
    result.postVisibilityWeight = 0.0;
    if (any(cell < 0.0) || any(cell > float3(DDGI_COUNTS - 1u))) return result;
    const uint3 lower = uint3(min(floor(cell), float3(DDGI_COUNTS - 2u)));
    const float3 fractional = saturate(cell - float3(lower));
    // Move the visibility query off the surface; keep the original position
    // for cell selection so the trilinear weights remain spatially stable.
    const float normalBias = min(spacing * 0.0375, 0.10);
    const float viewBias = min(spacing * 0.075, 0.20);
    const float3 biasedPosition = position + geometricNormal * normalBias +
                                  viewDirection * viewBias;
    const float2 irradianceOct = ddgiOctEncode(irradianceNormal);
    float3 irradiance = 0.0;
    float weightedVisibility = 0.0;
    float validWeight = 0.0;
    uint activeProbeCount = 0u;
    for (uint pair = 0u; pair < 8u; pair += 2u) {
        const uint3 offset0 = uint3(0u, (pair >> 1u) & 1u, (pair >> 2u) & 1u);
        const uint3 grid0 = lower + offset0;
        const uint index0 = ddgiPhysicalIndex(grid0, scrollOffset);
        const uint index1 = ddgiPhysicalIndex(grid0 + uint3(1u, 0u, 0u), scrollOffset);
        // Keep only two probe records live while both independent loads are in flight.
        const float4 probe0 = probeTexture.Load(int3(ddgiProbePixel(index0), 0));
        const float4 probe1 = probeTexture.Load(int3(ddgiProbePixel(index1), 0));
        [unroll] for (uint lane = 0u; lane < 2u; ++lane) {
            const uint3 offset = offset0 + uint3(lane, 0u, 0u);
            const uint3 grid = lower + offset;
            const uint index = lane == 0u ? index0 : index1;
            const float4 probe = lane == 0u ? probe0 : probe1;
            // Pending probes have no valid irradiance at their current position.
            // In particular, a relocated probe must not reuse its old atlas here.
            if (probe.w < 0.5 || probe.w > 1.5) continue;
            ++activeProbeCount;
            const float3 cornerWeight = lerp(1.0 - fractional, fractional, float3(offset));
            const float trilinearWeight = cornerWeight.x * cornerWeight.y * cornerWeight.z;
            // A zero-weight corner still counts for diagnostics, but its atlas
            // texels cannot contribute to irradiance, visibility, or coverage.
            if (trilinearWeight <= 0.0) continue;
            const float3 probePosition = origin + float3(grid) * spacing + probe.xyz;
            const float3 toSurface = biasedPosition - probePosition;
            const float distance = length(toSurface);
            const float3 direction = distance > 1e-4 ? toSurface / distance : geometricNormal;
            const float4 encodedProbeIrradiance = ddgiSampleProbeOct(
                irradianceTexture, index, irradianceOct, DDGI_IRRADIANCE_INTERIOR);
            const float3 probeIrradiance = ddgiDecodeIrradiance(encodedProbeIrradiance.rgb);
            const float reactivation = saturate((1.5 - probe.w) * 2.0);
            const float confidence = saturate(encodedProbeIrradiance.a) * reactivation;
            if (confidence <= 0.0) continue;
            const float2 visibilityOct = ddgiOctEncode(direction);
            const float2 moments = ddgiSampleProbeOct(distanceTexture, index, visibilityOct,
                                                      DDGI_DISTANCE_INTERIOR).rg;
            // Missing or pending probes reduce the cascade's valid coverage.
            validWeight += trilinearWeight * confidence;
            const float3 surfaceToProbe = -direction;
            const float wrap = (dot(geometricNormal, surfaceToProbe) + 1.0) * 0.5;
            float weight = (wrap * wrap + 0.2) * confidence;
            const float weightBeforeVisibility = weight * trilinearWeight;
            result.preVisibilityWeight += weightBeforeVisibility;
            const float variance = max(moments.y - moments.x * moments.x, 0.0001);
            const float delta = max(distance - moments.x, 0.0);
            const float chebyshev = saturate(variance / (variance + delta * delta));
            weightedVisibility += chebyshev * weightBeforeVisibility;
            weight *= max(0.05, chebyshev * chebyshev * chebyshev);
            // Keep a fallback when every probe is occluded, while suppressing
            // light leaking from probes with only a small visibility weight.
            weight = max(weight, 0.000001);
            if (weight < 0.2) weight *= weight * weight / (0.2 * 0.2);
            weight *= trilinearWeight;
            irradiance += probeIrradiance.rgb * weight;
            result.postVisibilityWeight += weight;
        }
    }
    const float transition = ddgiCellTransition(cell);
    result.irradiance = result.postVisibilityWeight > 0.0
        ? irradiance / result.postVisibilityWeight : float3(0.0);
    result.visibility = weightedVisibility / max(result.preVisibilityWeight, 0.0001);
    result.activeProbeFraction = float(activeProbeCount) / 8.0;
    // Eight fully valid trilinear weights can sum just below one in FP32.
    // Treat that rounding error as full coverage to avoid another cascade.
    const float coverage = validWeight >= 0.99999 ? 1.0 : saturate(validWeight);
    result.blendWeight = coverage * transition;
    return result;
}
