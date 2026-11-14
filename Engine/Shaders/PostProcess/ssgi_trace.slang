// Screen-space indirect diffuse lighting with a visibility bitmask (Therrien,
// Levesque and Gilet, "Screen Space Indirect Lighting with Visibility
// Bitmask", 2023). Each slice of the hemisphere is split into 32 angular
// sectors. A depth sample occludes the sectors between its front face and an
// assumed back face `thickness` metres behind it, so thin geometry no longer
// hides everything behind it as with GTAO's infinite-thickness horizons. Only
// sectors a sample newly occludes add its radiance, which makes the estimate
// visibility-correct without tracing rays. Radiance is the previous frame's
// lit colour, reprojected with the camera; light therefore accumulates one
// extra bounce per frame.
import PostProcess.SsgiCommon;

// x: linear view depth bits, y: packed octahedral view normal, at trace resolution.
[[vk::binding(0,0), vk::image_format("rg32ui")]] RWTexture2D<uint2> geometry;
[[vk::binding(1,0)]] Sampler2D previousRadiance;
// rgb: irradiance in the units of the IBL irradiance map, a: linear view depth.
[[vk::binding(2,0)]] RWTexture2D<float4> rawGi;
struct Settings {
    // Current view space to the clip space the radiance history was rendered in.
    float4x4 viewToPreviousClip;
    float4 depthToView;
    float2 projectionScale;
    float radiusView, thickness;
    uint frameIndex, sliceCount, stepCount;
    float maxRadiusPixels;
    uint radianceValid;
    // Scalars, not a float3: std430 would align a vector to 16 bytes past 128.
    float padding0, padding1, padding2;
};
[[vk::push_constant]] ConstantBuffer<Settings> settings;

static const uint SectorCount = 32u;

uint occludedSectors(float2 horizonAngles, float normalAngle) {
    // Map both horizon angles into [0, 1] across the hemisphere centred on the
    // projected normal, then set every sector the [front, back] span touches.
    float2 normalized = saturate((horizonAngles - normalAngle + 0.5 * SsgiPi) / SsgiPi);
    float low = min(normalized.x, normalized.y), high = max(normalized.x, normalized.y);
    uint start = min(uint(low * float(SectorCount)), SectorCount - 1u);
    uint count = min(uint(ceil((high - low) * float(SectorCount))), SectorCount - start);
    return count == 0u ? 0u : (0xFFFFFFFFu >> (SectorCount - count)) << start;
}

[numthreads(8,8,1)] [shader("compute")]
void main(uint3 id:SV_DispatchThreadID){
    uint w, h;
    rawGi.GetDimensions(w, h);
    if (any(id.xy >= uint2(w, h))) return;
    const float2 halfSize = float2(w, h);
    const int2 maxPixel = int2(w, h) - 1;

    const float2 uv = (float2(id.xy) + 0.5) / halfSize;
    const uint2 centerGeometry = geometry[id.xy];
    const float depth = asfloat(centerGeometry.x);
    if (depth >= SsgiBackgroundDepth || settings.radianceValid == 0u) {
        rawGi[id.xy] = float4(0.0, 0.0, 0.0, depth);
        return;
    }
    const float3 center = ssgiViewPosition(uv, depth, settings.projectionScale);
    const float3 normal = ssgiUnpackNormal(centerGeometry.y);
    const float3 viewDir = ssgiSafeNormalize(-center);

    float2 radiusPixels = 0.5 * settings.radiusView * abs(settings.projectionScale) / depth * halfSize;
    const float largestRadius = max(radiusPixels.x, radiusPixels.y);
    // Close to the camera the world-space radius spans much of the screen;
    // capping it keeps the taps cache-coherent at the cost of a shorter reach.
    radiusPixels *= min(1.0, settings.maxRadiusPixels / max(largestRadius, 1e-4));
    if (largestRadius < 1.0) {
        rawGi[id.xy] = float4(0.0, 0.0, 0.0, depth);
        return;
    }
    const float minSampleDistance = 1.3 / max(max(radiusPixels.x, radiusPixels.y), 1e-4);
    const float falloffStart = 0.7 * settings.radiusView;
    const float falloffScale = 1.0 / max(settings.radiusView - falloffStart, 1e-4);

    const float2 noise = ssgiNoise(id.xy, settings.frameIndex);
    // The step loop is bound by the quarter-rate XU pipe (Nsight). Keep its
    // divisions and int-to-float conversions out of it: view positions come
    // from pixel coordinates by one multiply-add.
    const float2 invHalfSize = 1.0 / halfSize;
    const float2 pixelToView = 2.0 * invHalfSize / settings.projectionScale;
    const float2 viewOrigin = -1.0 / settings.projectionScale;
    const float invStepCount = 1.0 / float(settings.stepCount);
    float3 irradiance = 0.0;
    for (uint slice = 0u; slice < settings.sliceCount; ++slice) {
        float sinPhi, cosPhi;
        sincos((float(slice) + noise.x) * SsgiPi / float(settings.sliceCount), sinPhi, cosPhi);
        const float2 ray = float2(cosPhi, -sinPhi);
        const float2 rayUv = ray * radiusPixels * invHalfSize;
        const float sliceNoise = noise.y + float(slice * settings.stepCount) * 0.61803398875;
        // The view-space direction a step along `ray` moves in, whatever the
        // sign convention of the projection's y axis.
        const float3 screenTangent = ssgiSafeNormalize(float3(ray / (settings.projectionScale * halfSize), 0.0));
        const float3 bitangent = ssgiSafeNormalize(cross(viewDir, screenTangent));
        const float3 tangent = cross(bitangent, viewDir);
        float3 projectedNormal = normal - bitangent * dot(normal, bitangent);
        const float projectedLength = length(projectedNormal);
        if (projectedLength < 1e-4) continue;
        projectedNormal /= projectedLength;
        const float normalAngle =
            sign(dot(tangent, projectedNormal)) * ssgiFastAcos(saturate(dot(projectedNormal, viewDir)));

        uint occluded = 0u;
        float3 sliceLight = 0.0;
        for (uint side = 0u; side < 2u; ++side) {
            const float direction = side == 0u ? 1.0 : -1.0;
            float stepIndex = 0.0;
            for (uint step = 0u; step < settings.stepCount; ++step, stepIndex += 1.0) {
                const float stepNoise = frac(sliceNoise + stepIndex * 0.61803398875);
                const float x = (stepIndex + stepNoise) * invStepCount;
                const float2 sampleUv = uv + direction * rayUv * (x * x + minSampleDistance);
                if (any(sampleUv < 0.0) || any(sampleUv > 1.0)) break;
                const int2 samplePixel = min(int2(sampleUv * halfSize), maxPixel);
                const uint2 sampleGeometry = geometry[samplePixel];
                const float sampleDepth = asfloat(sampleGeometry.x);
                if (sampleDepth >= SsgiBackgroundDepth) continue;
                const float3 samplePosition =
                    float3((float2(samplePixel) + 0.5) * pixelToView + viewOrigin, -1.0) * sampleDepth;
                const float3 delta = samplePosition - center;
                const float distanceSquared = dot(delta, delta);
                if (distanceSquared < 1e-8) continue;
                const float inverseDistance = rsqrt(distanceSquared);
                const float distance = distanceSquared * inverseDistance;
                const float3 sampleDir = delta * inverseDistance;
                const float3 backDelta = delta + ssgiSafeNormalize(samplePosition) * settings.thickness;
                const float2 horizonCos = clamp(float2(dot(sampleDir, viewDir), dot(ssgiSafeNormalize(backDelta), viewDir)),
                                                -1.0, 1.0);
                const float2 horizonAngles = direction * float2(ssgiFastAcos(horizonCos.x), ssgiFastAcos(horizonCos.y));
                const uint sectors = occludedSectors(horizonAngles, normalAngle);
                const uint newlyOccluded = sectors & ~occluded;
                occluded |= sectors;
                if (newlyOccluded == 0u) continue;

                const float receiver = saturate(dot(normal, sampleDir));
                // Surfaces facing away from the receiver cannot light it; their
                // screen colour belongs to the other side.
                const float3 sampleNormal = ssgiUnpackNormal(sampleGeometry.y);
                const float emitter = smoothstep(0.0, 0.2, dot(sampleNormal, -sampleDir));
                const float falloff = saturate(1.0 - (distance - falloffStart) * falloffScale);
                const float weight = float(countbits(newlyOccluded)) * receiver * emitter * falloff;
                if (weight <= 0.0) continue;
                const float4 previousClip = mul(float4(samplePosition, 1.0), settings.viewToPreviousClip);
                if (previousClip.w <= 1e-4) continue;
                const float2 previousUv = previousClip.xy / previousClip.w * 0.5 + 0.5;
                if (any(previousUv < 0.0) || any(previousUv > 1.0)) continue;
                sliceLight += previousRadiance.SampleLevel(previousUv, 0.0).rgb * weight;
            }
        }
        irradiance += sliceLight;
    }
    // A fully occluded slice of unit radiance sums cos over 32 sectors, which is
    // 64/pi; the result is then averaged over slices to match the irradiance map.
    irradiance *= SsgiPi / (64.0 * float(settings.sliceCount));
    if (any(isnan(irradiance))) irradiance = 0.0;
    rawGi[id.xy] = float4(irradiance, depth);
}
