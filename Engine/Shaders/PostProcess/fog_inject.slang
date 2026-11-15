// Volumetric fog, step 1: density and in-scattered light per froxel.
//
// Each froxel is evaluated at one point, jittered in depth every frame, and
// blended with its reprojected history, so a single sample per froxel
// converges to the integral over the cell. Light comes from:
//  - the sun, through the directional virtual shadow map (one tap);
//  - every local light of the froxel's light cluster (the clustered lists
//    Forward walks), with the same falloff, profile and spot cone;
//  - the sky irradiance, as isotropic ambient.
// Output: rgb in-scattered radiance times the scattering coefficient,
// a extinction (1/m).
import Common.SceneData;
import Common.ShadowSampling;
import Common.VolumetricFog;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(0, 0)]] Sampler2DShadow shadowMap;
[[vk::binding(9, 0)]] Sampler2D shadowDepthMap;
[[vk::binding(17, 0)]] Sampler2DShadow linearShadowMap;
[[vk::binding(4, 0)]] StructuredBuffer<uint4> shadowPageTable;
[[vk::binding(3, 0)]] StructuredBuffer<ClusterLightRange> clusterRanges;
[[vk::binding(7, 0)]] StructuredBuffer<uint> clusterLightIndices;
[[vk::binding(10, 0)]] SamplerCube irradianceMap;
[[vk::binding(24, 0)]] StructuredBuffer<LocalLightData> localLights;

[[vk::binding(0, 1), vk::image_format("rgba16f")]] RWTexture3D<float4> scattering;
[[vk::binding(1, 1)]] Sampler3D history;

// Smooth 3D value noise for drifting density variation.
float fogHash(float3 p)
{
    p = frac(p * 0.3183099 + 0.1);
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float fogValueNoise(float3 p)
{
    const float3 cell = floor(p);
    const float3 f = frac(p);
    const float3 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(fogHash(cell), fogHash(cell + float3(1, 0, 0)), u.x),
                     lerp(fogHash(cell + float3(0, 1, 0)), fogHash(cell + float3(1, 1, 0)), u.x), u.y),
                lerp(lerp(fogHash(cell + float3(0, 0, 1)), fogHash(cell + float3(1, 0, 1)), u.x),
                     lerp(fogHash(cell + float3(0, 1, 1)), fogHash(cell + float3(1, 1, 1)), u.x), u.y),
                u.z);
}

[numthreads(4, 4, 4)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint3 size = fogParams.size.xyz;
    if (any(id >= size))
        return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(size.xy);
    const float slice = (float(id.z) + fogParams.grid.w) / float(size.z);
    const float depth = fogDepthFromSlice(slice);
    const float3 viewPosition = fogViewPosition(uv, depth, frame.inverseProjection);
    const float3 worldPosition = mul(float4(viewPosition, 1.0), frame.inverseView).xyz;
    const float3 rayDirection = normalize(worldPosition - frame.cameraPosition.xyz);

    // Density: exponential height fog, varied by drifting noise.
    float extinction = fogParams.albedoDensity.w *
                       exp(-max(worldPosition.y - fogParams.height.x, 0.0) * fogParams.height.y);
    if (fogParams.height.w > 0.0)
    {
        const float time = frame.windGustFrequencyTime.z;
        const float3 drift = float3(0.35, 0.05, 0.2) * time;
        const float n = fogValueNoise(worldPosition * 0.18 + drift) * 0.65 +
                        fogValueNoise(worldPosition * 0.47 - drift * 1.7) * 0.35;
        extinction *= max(1.0 + fogParams.height.w * (n * 2.0 - 1.0) * 1.5, 0.0);
    }
    const float g = fogParams.height.z;

    float3 light = 0.0;
    if (extinction > 1.0e-6)
    {
        // Sky: uniform radiance E / pi, the phase function integrates to one.
        light += irradianceMap.SampleLevel(float3(0.0, 1.0, 0.0), 0.0).rgb * (fogParams.intensity.z / FOG_PI);

        // Sun.
        if (frame.lightDirectionIntensity.w > 0.0 && fogParams.intensity.x > 0.0)
        {
            const float3 lightDirection = normalize(-frame.lightDirectionIntensity.xyz);
            float visibility = 1.0;
            if (frame.shadowEnabled != 0u)
            {
                // Fog needs no texel-accurate level: the first clip level that
                // contains the point, no finer than its distance warrants.
                uint level = min(uint(max(log2(depth * 0.25), 0.0)), SHADOW_CLIP_LEVELS - 1u);
                [loop] for (; level < SHADOW_CLIP_LEVELS - 1u; ++level)
                {
                    const float4 clip = mul(float4(worldPosition, 1.0), frame.shadowClipMatrices[level]);
                    if (all(abs(clip.xy / clip.w) < 0.98))
                        break;
                }
                bool resident;
                float clipEdge, blockerCount, penumbraRadius;
                uint2 virtualPage;
                // Air has no surface: offset along the light instead of a normal.
                visibility = 1.0 - sampleVirtualShadowLevel(
                                       shadowMap, linearShadowMap, shadowDepthMap, worldPosition, lightDirection,
                                       lightDirection, frame.shadowClipMatrices[level],
                                       frame.shadowClipConstants[level], level, shadowPageTable, 0u, id.xy, 0u, false,
                                       resident, clipEdge, blockerCount, penumbraRadius, virtualPage, true);
                // Pages no surface requested are not resident: treat as lit.
                if (!resident)
                    visibility = 1.0;
            }
            light += frame.lightColor.rgb * frame.lightDirectionIntensity.w * visibility * fogParams.intensity.x *
                     fogHenyeyGreenstein(dot(rayDirection, lightDirection), g);
        }

        // Local lights of this froxel's light cluster.
        if (frame.localLightCount != 0u && fogParams.intensity.y > 0.0)
        {
            const uint2 grid = (uint2(frame.clusteredViewportNearFar.xy) + ClusterTilePixelSize - 1u) /
                               ClusterTilePixelSize;
            const uint clusterSlice =
                min(uint(max(log2(max(depth, frame.clusteredViewportNearFar.z)) *
                                 frame.clusterZScaleBiasEnvironmentMipRtContact.x +
                                 frame.clusterZScaleBiasEnvironmentMipRtContact.y,
                             0.0)),
                    23u);
            const uint2 tile = min(uint2(uv * frame.clusteredViewportNearFar.xy) / ClusterTilePixelSize, grid - 1u);
            const ClusterLightRange range = clusterRanges[(clusterSlice * grid.y + tile.y) * grid.x + tile.x];
            float3 local = 0.0;
            for (uint i = 0u; i < range.lightCount; ++i)
            {
                const uint lightIndex = clusterLightIndices[range.lightOffset + i];
                if (lightIndex >= frame.localLightCount)
                    continue;
                const LocalLightData lamp = localLights[lightIndex];
                const float3 toLight = lamp.positionRange.xyz - worldPosition;
                const float distanceSquared = dot(toLight, toLight);
                const float radius = lamp.positionRange.w;
                if (distanceSquared <= 1.0e-8 || distanceSquared >= radius * radius)
                    continue;
                const float inverseDistance = rsqrt(distanceSquared);
                const float3 lightDirection = toLight * inverseDistance;
                const float rangeFactor = saturate(1.0 - distanceSquared * inverseDistance / radius);
                float attenuation = rangeFactor * rangeFactor / max(distanceSquared, LocalLightSourceRadiusSquared);
                attenuation *= localLightProfile(lamp, -lightDirection);
                if (lamp.parameters.y > 1.5) // Spot
                    attenuation *= smoothstep(lamp.directionOuterCos.w, lamp.parameters.x,
                                              dot(normalize(-lamp.directionOuterCos.xyz), lightDirection));
                if (attenuation <= 0.0)
                    continue;
                local += lamp.colorIntensity.rgb * (lamp.colorIntensity.w * attenuation) *
                         fogHenyeyGreenstein(dot(rayDirection, lightDirection), g);
            }
            light += local * fogParams.intensity.y;
        }
    }

    float4 result = float4(light * fogParams.albedoDensity.rgb * extinction, extinction);

    // Temporal reprojection: the froxel's point in last frame's grid.
    if (fogParams.grid.z > 0.5)
    {
        const float3 previousView = mul(float4(worldPosition, 1.0), frame.previousView).xyz;
        const float4 previousClip = mul(float4(previousView, 1.0), frame.previousProjection);
        if (previousClip.w > 1.0e-4 && -previousView.z > 0.0)
        {
            const float3 previous = float3(previousClip.xy / previousClip.w * 0.5 + 0.5,
                                           fogSliceFromDepth(-previousView.z));
            if (all(previous.xy > 0.0) && all(previous.xy < 1.0))
                result = lerp(result, history.SampleLevel(previous, 0.0), fogParams.intensity.w);
        }
    }
    if (any(isnan(result)) || any(isinf(result)))
        result = float4(0.0, 0.0, 0.0, extinction);
    scattering[id] = result;
}
