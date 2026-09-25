import Common.SceneData;
import Common.Displacement;
import Common.RayHitGeometry;
import Common.ShadowLod;
import Common.ShadowSampling;
import GI.ddgi_common;
import GI.ddgi_gather_core;
[[vk::push_constant]] DDGIConstants ddgi;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(0, 0)]] Sampler2DShadow shadowMap;
[[vk::binding(4, 0)]] StructuredBuffer<uint4> shadowPageTable;
[[vk::binding(10, 0)]] SamplerCube diffuseIrradianceMap;
[[vk::binding(11, 0)]] SamplerCube skyEnvironment;
[[vk::binding(0, 1)]] RaytracingAccelerationStructure sceneTlas;
[[vk::binding(5, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> rayData;
[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(12, 1)]] StructuredBuffer<DDGIProbeUpdate> probeUpdateList;
[[vk::binding(18, 1)]] StructuredBuffer<float4> rayDirections;
// These views read the previous atlas contents while the DDGI update keeps
// the images in GENERAL for storage writes later in the command buffer.
[[vk::binding(13, 1)]] Sampler2D previousIrradiance;
[[vk::binding(14, 1)]] Sampler2D previousDistance;
[[vk::binding(15, 1)]] Sampler2D previousProbeData;

float3 skyRadiance(float3 direction) {
    return skyEnvironment.SampleLevel(direction, 0.0).rgb;
}

float3 evaluateSunDiffuse(float3 normal, float3 toSun, float3 baseColor,
                          float3 lightRadiance, float visibility) {
    const float nDotL = max(dot(normal, toSun), 0.0);
    return baseColor * (1.0 / 3.14159265) * lightRadiance * nDotL * visibility;
}

float ddgiSunVisibility(float3 position, float3 normal, float3 toSun) {
    if (frame.shadowEnabled == 0u) return 1.0;
    [loop]
    for (uint level = 0u; level < SHADOW_CLIP_LEVELS; ++level) {
        const ShadowClipConstants clip = frame.shadowClipConstants[level];
        const float3 receiver = position + normal *
            (clip.worldTexelSize * GEOMETRIC_NORMAL_OFFSET_TEXELS);
        const float4 projected = mul(float4(receiver, 1.0), frame.shadowClipMatrices[level]);
        if (abs(projected.w) < 1.0e-6) continue;
        const float3 ndc = projected.xyz / projected.w;
        const float2 uv = ndc.xy * 0.5 + 0.5;
        if (any(uv < 0.0) || any(uv >= 1.0) || ndc.z <= 0.0 || ndc.z >= 1.0) continue;
        float2 atlasUv;
        if (!virtualShadowAtlasUv(uv * VIRTUAL_RESOLUTION, level, shadowPageTable, atlasUv))
            continue;
        const float noL = saturate(abs(dot(normalize(normal), toSun)));
        const float slope = sqrt(saturate(1.0 - noL * noL)) / max(noL, 0.05);
        const float bias = max(MIN_SHADOW_BIAS,
            clip.worldTexelSize * clip.depthScale * (1.0 + min(slope, 8.0)));
        return shadowMap.SampleCmpLevelZero(atlasUv, ndc.z - bias);
    }
    // VSM pages are allocated for raster receivers; unallocated GI hits are lit.
    return 1.0;
}

float localLightFactor(LocalLightData light, float3 position, float3 normal) {
    const float3 toLight = light.positionRange.xyz - position;
    const float distanceToLight = length(toLight);
    if (distanceToLight <= 1e-4 || distanceToLight >= light.positionRange.w) return 0.0;
    const float3 direction = toLight / distanceToLight;
    const float nDotL = max(dot(normal, direction), 0.0);
    const float rangeFactor = saturate(1.0 - distanceToLight / light.positionRange.w);
    float attenuation = rangeFactor * rangeFactor / max(distanceToLight * distanceToLight, 0.01);
    if (light.parameters.y > 1.5) {
        const float cone = dot(normalize(-light.directionOuterCos.xyz), direction);
        attenuation *= smoothstep(light.directionOuterCos.w, light.parameters.x, cone);
    }
    return nDotL * attenuation;
}

float3 evaluateLocalDiffuse(float3 position, float3 normal, float3 geometricNormal,
                            float3 albedo, uint4 lightIndices) {
    float3 result = 0.0;
    for (uint rank = 0u; rank < 4u; ++rank) {
        const uint lightIndex = lightIndices[rank];
        if (lightIndex == 0xffffffffu) break;
        const LocalLightData light = frame.localLights[lightIndex];
        const float3 toLight = light.positionRange.xyz - position;
        if (dot(geometricNormal, toLight) <= 0.0) continue;
        const float factor = localLightFactor(light, position, normal);
        if (factor <= 0.0) continue;
        const float distanceToLight = length(toLight);
        float visibility = 1.0;
        if (distanceToLight > 0.05) {
            RayDesc shadow;
            shadow.Origin = position + geometricNormal * 0.025;
            // Aim at the light from the displaced origin, including the endpoint.
            const float3 shadowToLight = light.positionRange.xyz - shadow.Origin;
            const float shadowDistance = length(shadowToLight);
            shadow.Direction = shadowToLight / shadowDistance;
            shadow.TMin = min(0.02, shadowDistance * 0.25);
            shadow.TMax = shadowDistance - shadow.TMin;
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                     RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> lightQuery;
            // Bit 0 selects shadow casters; bit 1 is for GI surface hits.
            lightQuery.TraceRayInline(sceneTlas,
                RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0x01, shadow);
            lightQuery.Proceed();
            visibility = lightQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
        }
        result += light.colorIntensity.rgb * light.colorIntensity.w *
                  factor * visibility * albedo / 3.14159265;
    }
    return result;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.y >= ddgiUpdateCount(ddgi)) return;
    const DDGIProbeUpdate update = probeUpdateList[id.y];
    const uint rayCount = ddgiRandomRayCount(ddgi, update.relocated);
    if (id.x >= rayCount) return;
    const uint probeIndex = update.probeIndex;
    if (probeIndex >= DDGI_PROBES) return;
    const float4 probe = probeData[int2(ddgiProbePixel(probeIndex))];
    if (probe.w < 0.5) return;
    const uint directionIndex = ddgiRandomDirectionIndex(
        id.x, rayCount, ddgiRandomRayCount(ddgi, 0u));
    const float3 direction = rayDirections[directionIndex].xyz;
    RayDesc ray;
    ray.Origin = ddgiPosition(probeIndex, ddgi.originSpacing.xyz, ddgi.originSpacing.w, ddgi.scrollOffsetRays.xyz) + probe.xyz;
    ray.Direction = direction;
    ray.TMin = 0.02;
    ray.TMax = ddgi.distanceInitialized.x;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(sceneTlas, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
                         0x02, ray);
    query.Proceed();
    float3 radiance = skyRadiance(direction);
    float distance = ddgi.distanceInitialized.x;
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        distance = query.CommittedRayT();
        const RayHitSurface hit = loadRayHitSurface(
            query.CommittedInstanceID(), query.CommittedPrimitiveIndex(),
            query.CommittedTriangleBarycentrics());
        const MaterialData material = materials[hit.materialIndex];
        const bool doubleSided = (material.textureIndices.w & 1) != 0;
        const bool realBackface = !query.CommittedTriangleFrontFace() && !doubleSided;
        if (realBackface) {
            // Random-ray distance moments use a shortened backface hit so
            // visibility gives this probe less weight near thin geometry.
            // Fixed validation rays retain the full distance for relocation.
            rayData[int2(id.x, id.y)] = float4(0.0, 0.0, 0.0, -distance * 0.2);
            return;
        }
        // Orient the actual triangle plane toward the incoming ray. This also
        // handles mirrored instances and the back side of double-sided materials.
        const float3 geometricNormal = dot(hit.geometricNormal, direction) > 0.0
            ? -hit.geometricNormal : hit.geometricNormal;
        float3 hitNormal = !query.CommittedTriangleFrontFace() && doubleSided
            ? -hit.normal : hit.normal;
        if (dot(hitNormal, geometricNormal) < 0.0) hitNormal = -hitNormal;
        const float3 vertexColor = (material.textureIndices.w & 32) != 0 ? 1.0 : hit.color;
        float3 albedo = material.baseColorMetallic.rgb * vertexColor;
        if (material.textureIndices.x >= 0)
            albedo *= materialTextures[NonUniformResourceIndex(material.textureIndices.x)]
                .SampleLevel(materialTextureUv(material, 0u, hit.uv0, hit.uv1), 0.0).rgb;
        float metallic = material.baseColorMetallic.a;
        if (material.textureIndices.y >= 0)
            metallic *= materialTextures[NonUniformResourceIndex(material.textureIndices.y)]
                .SampleLevel(materialTextureUv(material, 1u, hit.uv0, hit.uv1), 0.0).b;
        albedo *= 1.0 - saturate(metallic);
        float3 emissive = material.emissiveColorIntensity.rgb * material.emissiveColorIntensity.w;
        if (material.extensionTextureIndices.x >= 0)
            emissive *= materialTextures[NonUniformResourceIndex(material.extensionTextureIndices.x)]
                .SampleLevel(materialTextureUv(material, 7u, hit.uv0, hit.uv1), 0.0).rgb;
        float3 indirectIrradiance = diffuseIrradianceMap.SampleLevel(hitNormal, 0.0).rgb * 3.14159265;
        if (ddgi.distanceInitialized.y > 0.5) {
            const DDGIGatherResult recursive = ddgiGatherCascade(
                hit.position, geometricNormal, hitNormal, -direction, ddgi.originSpacing.xyz,
                ddgi.originSpacing.w, ddgi.scrollOffsetRays.xyz,
                previousIrradiance, previousDistance, previousProbeData);
            indirectIrradiance = lerp(indirectIrradiance, recursive.irradiance, recursive.blendWeight);
        }
        radiance = emissive + indirectIrradiance * albedo / 3.14159265;
        if (frame.lightDirectionIntensity.w > 0.0) {
            const float3 toSun = normalize(-frame.lightDirectionIntensity.xyz);
            const float nDotL = max(dot(hitNormal, toSun), 0.0);
            if (nDotL > 0.0 && dot(geometricNormal, toSun) > 0.0) {
                const float visibility = ddgiSunVisibility(hit.position, geometricNormal, toSun);
                radiance += evaluateSunDiffuse(hitNormal, toSun, albedo,
                    frame.lightColor.rgb * frame.lightDirectionIntensity.w, visibility);
            }
        }
    radiance += evaluateLocalDiffuse(hit.position, hitNormal, geometricNormal, albedo,
                                    probeUpdateList[id.y].localLightIndices);
    }
    rayData[int2(id.x, id.y)] = float4(radiance, distance);
}
