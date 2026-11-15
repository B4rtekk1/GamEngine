// Deferred local lighting for Game View. Forward shading of the PBR programs
// leaves its shading inputs (Common/DeferredLocalLighting) instead of walking
// the local lights itself; this pass evaluates the same BRDF once per pixel
// and adds it to the HDR target.
//
// Forward shading walked the light list for every rasterized fragment,
// including the helper lanes of partly covered quads, which Bistro's dense
// geometry made several times the pixel count; here every thread is one
// visible pixel, and a group shares one depth-bounded tile list.
import Common.SceneData;
import Common.PbrLighting;
import Common.DeferredLocalLighting;

[[vk::binding(0, 0)]] Sampler2D depthTexture;
[[vk::binding(1, 0), vk::image_format("rgba32ui")]] RWTexture2D<uint4> surfaceData;
[[vk::binding(2, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> hdrColor;
[[vk::binding(3, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(4, 0)]] StructuredBuffer<LocalLightData> localLights;
[[vk::binding(5, 0)]] StructuredBuffer<uint> tileLightIndices;
// Denoised ray-traced occlusion (Shadow/local_light_shadow_denoise.slang).
[[vk::binding(6, 0)]] Sampler2D<uint4> localShadowMask;

static const uint SCREEN_SPACE_EFFECTS_LOCAL_SHADOWS = 8u;

float3 reconstructViewPosition(int2 pixel, float depth, int2 extent)
{
    const float2 uv = (float2(pixel) + 0.5) / float2(extent);
    const float4 position = mul(float4(uv * 2.0 - 1.0, depth, 1.0), frame.inverseProjection);
    return position.xyz / position.w;
}

// The quad-resolution shadow texel for this pixel, matched by depth (as
// forward shading did before).
uint4 localShadowsAt(int2 pixel, float pixelDepth)
{
    const int2 quad = pixel >> 1;
    uint2 quads;
    localShadowMask.GetDimensions(quads.x, quads.y);
    uint4 shadows = localShadowMask.Load(int3(quad, 0));
    float depthError = abs(asfloat(shadows.w & ~0xffu) - pixelDepth);
    const float tolerance = 0.02 * pixelDepth + 0.02;
    if (depthError > tolerance)
    {
        const int2 toward = select((pixel & 1) != 0, int2(1, 1), int2(-1, -1));
        const int2 candidates[3] = { int2(toward.x, 0), int2(0, toward.y), toward };
        [unroll] for (uint c = 0u; c < 3u; ++c)
        {
            const int2 neighbour = clamp(quad + candidates[c], int2(0, 0), int2(quads) - 1);
            const uint4 candidate = localShadowMask.Load(int3(neighbour, 0));
            const float candidateError = abs(asfloat(candidate.w & ~0xffu) - pixelDepth);
            if (candidateError < depthError)
            {
                depthError = candidateError;
                shadows = candidate;
            }
        }
        if (depthError > 3.0 * tolerance)
            shadows = uint4(0u, 0u, 0u, 0u);
    }
    return shadows;
}

[numthreads(8, 8, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    hdrColor.GetDimensions(width, height);
    const int2 pixel = int2(id.xy);
    if (pixel.x >= int(width) || pixel.y >= int(height))
        return;
    // A tile no light reaches reads nothing else: the surface, depth and
    // shadow texels are most of this pass's memory traffic.
    const uint tilesX = (width + TileLightPixelSize - 1u) / TileLightPixelSize;
    const uint2 tile = uint2(pixel) / TileLightPixelSize;
    const uint tileBase = (tile.y * tilesX + tile.x) * TileLightStride;
    const uint lightCount = tileLightIndices[tileBase];
    if (lightCount == 0u)
        return;
    DeferredLocalSurface surface;
    if (!unpackDeferredLocalSurface(surfaceData[pixel], surface))
        return;
    const float depth = depthTexture.Load(int3(pixel, 0)).x;
    const float3 viewPosition = reconstructViewPosition(pixel, depth, int2(width, height));
    const float3 worldPosition = mul(float4(viewPosition, 1.0), frame.inverseView).xyz;
    const float3 viewDirection = normalize(frame.cameraPosition.xyz - worldPosition);
    const float3 normal = surface.normal;
    const float roughness = max(surface.roughness, 0.045);
    const float nDotV = max(dot(normal, viewDirection), 0.0);
    const float geometryView = geometrySchlickGGX(nDotV, roughness);
    const float3 energyCompensation = 1.0 + surface.f0 * surface.energyTerm;

    uint4 shadows = uint4(0u, 0u, 0u, 0u);
    if ((frame.screenSpaceEffects & SCREEN_SPACE_EFFECTS_LOCAL_SHADOWS) != 0u)
        shadows = localShadowsAt(pixel, -viewPosition.z);

    float3 lighting = 0.0;
    for (uint entry = 0u; entry < lightCount; ++entry)
    {
        const uint lightIndex = tileLightIndices[tileBase + 1u + entry];
        if (lightIndex >= frame.localLightCount) continue;
        float shadowVisibility = 1.0;
        if (shadows.z != 0u)
        {
            [unroll] for (uint s = 0u; s < 4u; ++s)
            {
                const uint shadowLight = ((s < 2u ? shadows.x : shadows.y) >> ((s & 1u) * 16u)) & 0xffffu;
                if (shadowLight == lightIndex)
                    shadowVisibility = 1.0 - float((shadows.z >> (s * 8u)) & 0xffu) / 255.0;
            }
        }
        if (shadowVisibility <= 0.0) continue;
        const LocalLightData local = localLights[lightIndex];
        const float3 toLight = local.positionRange.xyz - worldPosition;
        const float distanceSquared = dot(toLight, toLight);
        const float radius = local.positionRange.w;
        if (distanceSquared <= 1e-8 || distanceSquared >= radius * radius) continue;
        const float inverseDistance = rsqrt(distanceSquared);
        const float distanceToLight = distanceSquared * inverseDistance;
        const float3 lightDirection = toLight * inverseDistance;
        const float rangeFactor = saturate(1.0 - distanceToLight / radius);
        float attenuation = rangeFactor * rangeFactor / max(distanceSquared, LocalLightSourceRadiusSquared);
        const float nDotL = localNDotL(surface.nDotLMode, normal, lightDirection);
        // As in forward shading: lights near the end of their range or
        // outside their cone add almost nothing, so skip their GGX. The
        // profile (an acos and two table reads) and the cone only lower the
        // product, so most of those lights are rejected before them.
        const float reach = local.colorIntensity.w * nDotL * shadowVisibility;
        if (reach * attenuation < LocalLightCutoff) continue;
        attenuation *= localLightProfile(local, -lightDirection);
        if (local.parameters.y > 1.5) // Spot
        {
            const float cone = dot(normalize(-local.directionOuterCos.xyz), lightDirection);
            attenuation *= smoothstep(local.directionOuterCos.w, local.parameters.x, cone);
        }
        if (reach * attenuation < LocalLightCutoff) continue;
        const float3 halfway = normalize(viewDirection + lightDirection);
        const float3 fresnel = fresnelSchlick(max(dot(halfway, viewDirection), 0.0), surface.f0);
        const float geometry = geometryView * geometrySchlickGGX(nDotL, roughness);
        const float3 specular = distributionGGX(normal, halfway, roughness) * geometry * fresnel /
                                max(4.0 * nDotV * nDotL, 0.0001) * energyCompensation;
        const float3 diffuse = (1.0 - fresnel) * surface.diffuseColor;
        lighting += (diffuse + specular) * local.colorIntensity.rgb * local.colorIntensity.w * nDotL * attenuation *
                    shadowVisibility;
    }
    if (all(lighting <= 0.0))
        return;
    const float4 color = hdrColor[pixel];
    hdrColor[pixel] = float4(min(color.rgb + lighting, 65000.0), color.a);
}
