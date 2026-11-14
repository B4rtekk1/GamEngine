// Stochastic local lighting (MegaLights-style), shading step. The shadow
// tracer sampled four lights per 2x2 quad in proportion to their unshadowed
// irradiance and traced one ray to each (Shadow/local_light_shadow.slang,
// mode 1). Every pixel evaluates the full BRDF for the samples of its own
// quad and the three quads towards it, weighted by surface similarity:
//   L ~ 1/4 * sum f(sample) * V(sample) * W(sample),  W = sum(p^) / p^(sample)
// an unbiased estimate of all lights' contribution for a cost independent
// of the light count. megalights_temporal.slang accumulates it.
import Common.SceneData;
import Common.PbrLighting;
import Common.DeferredLocalLighting;

[[vk::binding(0, 0)]] Sampler2D depthTexture;
[[vk::binding(1, 0), vk::image_format("rgba32ui")]] RWTexture2D<uint4> surfaceData;
// Noisy diffuse lighting (without the diffuse colour) and specular radiance.
[[vk::binding(2, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> noisyDiffuse;
[[vk::binding(3, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(4, 0)]] StructuredBuffer<LocalLightData> localLights;
[[vk::binding(5, 0), vk::image_format("rg32ui")]] RWTexture2D<uint2> lightSamples;
[[vk::binding(6, 0), vk::image_format("rg32ui")]] RWTexture2D<uint2> lightSampleWeights;
[[vk::binding(7, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> noisySpecular;

// The pixel each quad traced this frame (as local_light_shadow.slang).
static const int2 QuadOrder[4] = { int2(0, 0), int2(1, 1), int2(1, 0), int2(0, 1) };

float3 reconstructViewPosition(int2 pixel, float depth, int2 extent)
{
    const float2 uv = (float2(pixel) + 0.5) / float2(extent);
    const float4 position = mul(float4(uv * 2.0 - 1.0, depth, 1.0), frame.inverseProjection);
    return position.xyz / position.w;
}

[numthreads(8, 8, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height, quadWidth, quadHeight;
    noisyDiffuse.GetDimensions(width, height);
    lightSamples.GetDimensions(quadWidth, quadHeight);
    const int2 extent = int2(width, height);
    const int2 pixel = int2(id.xy);
    if (any(pixel >= extent))
        return;
    DeferredLocalSurface surface;
    if (!unpackDeferredLocalSurface(surfaceData[pixel], surface))
    {
        noisyDiffuse[pixel] = 0.0;
        noisySpecular[pixel] = 0.0;
        return;
    }
    const float depth = depthTexture.Load(int3(pixel, 0)).x;
    const float3 viewPosition = reconstructViewPosition(pixel, depth, extent);
    const float viewDepth = -viewPosition.z;
    const float3 worldPosition = mul(float4(viewPosition, 1.0), frame.inverseView).xyz;
    const float3 viewDirection = normalize(frame.cameraPosition.xyz - worldPosition);
    const float roughness = max(surface.roughness, 0.045);
    const float nDotV = max(dot(surface.normal, viewDirection), 0.0);
    const float geometryView = geometrySchlickGGX(nDotV, roughness);
    const float3 energyCompensation = 1.0 + surface.f0 * surface.energyTerm;

    const int2 quad = pixel >> 1;
    const int2 toward = select((pixel & 1) != 0, int2(1, 1), int2(-1, -1));
    const int2 offsets[4] = { int2(0, 0), int2(toward.x, 0), int2(0, toward.y), toward };
    const int2 traced = QuadOrder[frame.temporalSampleIndex & 3u];
    float3 diffuse = 0.0;
    float3 specular = 0.0;
    float weightSum = 0.0;
    [unroll] for (uint q = 0u; q < 4u; ++q)
    {
        const int2 sampleQuad = quad + offsets[q];
        if (any(sampleQuad < 0) || any(sampleQuad >= int2(quadWidth, quadHeight)))
            continue;
        // The quad's samples were drawn for the surface at its traced pixel;
        // reuse them only on the same surface.
        const int2 tracedPixel = min(sampleQuad * 2 + traced, extent - 1);
        DeferredLocalSurface tracedSurface;
        if (!unpackDeferredLocalSurface(surfaceData[tracedPixel], tracedSurface))
            continue;
        const float tracedDepth = -reconstructViewPosition(tracedPixel, depthTexture.Load(int3(tracedPixel, 0)).x,
                                                            extent).z;
        const float depthWeight = saturate(1.0 - abs(tracedDepth - viewDepth) / (0.03 * viewDepth + 0.02));
        float normalWeight = saturate(dot(tracedSurface.normal, surface.normal));
        normalWeight *= normalWeight;
        normalWeight *= normalWeight;
        const float weight = depthWeight * normalWeight * (q == 0u ? 1.0 : 0.5);
        if (weight <= 1e-3)
            continue;
        const uint2 slots = lightSamples[sampleQuad];
        const uint2 packedWeights = lightSampleWeights[sampleQuad];
        float3 quadDiffuse = 0.0;
        float3 quadSpecular = 0.0;
        [unroll] for (uint k = 0u; k < 4u; ++k)
        {
            const uint slot = ((k < 2u ? slots.x : slots.y) >> ((k & 1u) * 16u)) & 0xffffu;
            if ((slot & 0x8000u) == 0u || (slot & 0x4000u) != 0u)
                continue; // no sample, or its ray was blocked
            const uint lightIndex = slot & 0x3ffu;
            if (lightIndex >= frame.localLightCount)
                continue;
            const float sampleWeight = f16tof32(((k < 2u ? packedWeights.x : packedWeights.y) >> ((k & 1u) * 16u)) &
                                                0xffffu);
            LocalLightContribution contribution;
            if (!evaluateLocalLight(surface, worldPosition, viewDirection, nDotV, geometryView, energyCompensation,
                                    localLights[lightIndex], contribution))
                continue;
            quadDiffuse += contribution.diffuse * sampleWeight;
            quadSpecular += contribution.specular * sampleWeight;
        }
        diffuse += quadDiffuse * (0.25 * weight);
        specular += quadSpecular * (0.25 * weight);
        weightSum += weight;
    }
    const float normalization = weightSum > 0.0 ? 1.0 / weightSum : 0.0;
    // Alpha: whether any sample covered this pixel (the temporal step keeps
    // its history where none did).
    noisyDiffuse[pixel] = float4(min(diffuse * normalization, 65000.0), weightSum > 0.0 ? 1.0 : 0.0);
    noisySpecular[pixel] = float4(min(specular * normalization, 65000.0), 0.0);
}
