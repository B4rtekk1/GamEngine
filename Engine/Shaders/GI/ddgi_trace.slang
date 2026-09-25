import Common.SceneData;
import Common.Displacement;
import Common.RayHitGeometry;
import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(11, 0)]] SamplerCube skyEnvironment;
[[vk::binding(0, 1)]] RaytracingAccelerationStructure sceneTlas;
[[vk::binding(5, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> rayData;
[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(12, 1)]] StructuredBuffer<uint> probeUpdateList;

float3 skyRadiance(float3 direction) {
    return skyEnvironment.SampleLevel(direction, 0.0).rgb;
}

float3 evaluateSunDiffuse(float3 normal, float3 toSun, float3 baseColor,
                          float3 lightRadiance, float visibility) {
    const float nDotL = max(dot(normal, toSun), 0.0);
    return baseColor * (1.0 / 3.14159265) * lightRadiance * nDotL * visibility;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(ddgi.scrollOffsetRays.w) || id.y >= ddgiUpdateCount(ddgi)) return;
    const uint probeIndex = probeUpdateList[id.y];
    const float4 probe = probeData[int2(ddgiProbePixel(probeIndex))];
    if (probe.w < 0.5) return;
    const float3 direction = ddgiRayDirection(id.x, uint(ddgi.scrollDeltaFrame.w), uint(ddgi.scrollOffsetRays.w));
    RayDesc ray;
    ray.Origin = ddgiPosition(probeIndex, ddgi.originSpacing.xyz, ddgi.originSpacing.w, ddgi.scrollOffsetRays.xyz) + probe.xyz;
    ray.Direction = direction;
    ray.TMin = 0.02;
    ray.TMax = ddgi.distanceInitialized.x;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(sceneTlas, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
                         0x02, ray);
    while (query.Proceed()) {}
    float3 radiance = skyRadiance(direction);
    float distance = ddgi.distanceInitialized.x;
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        distance = query.CommittedRayT();
        const RayHitSurface hit = loadRayHitSurface(
            query.CommittedInstanceID(), query.CommittedPrimitiveIndex(),
            query.CommittedTriangleBarycentrics());
        const MaterialData material = materials[hit.materialIndex];
        const float3 vertexColor = (material.textureIndices.w & 32) != 0 ? 1.0 : hit.color;
        float3 albedo = material.baseColorMetallic.rgb * vertexColor *
                        (1.0 - material.baseColorMetallic.a);
        if (material.textureIndices.x >= 0)
            albedo *= materialTextures[NonUniformResourceIndex(material.textureIndices.x)]
                .SampleLevel(materialTextureUv(material, 0u, hit.uv0, hit.uv1), 0.0).rgb;
        float3 emissive = material.emissiveColorIntensity.rgb * material.emissiveColorIntensity.w;
        if (material.extensionTextureIndices.x >= 0)
            emissive *= materialTextures[NonUniformResourceIndex(material.extensionTextureIndices.x)]
                .SampleLevel(materialTextureUv(material, 7u, hit.uv0, hit.uv1), 0.0).rgb;
        radiance = emissive;
        if (frame.lightDirectionIntensity.w > 0.0) {
            const float3 toSun = normalize(-frame.lightDirectionIntensity.xyz);
            const float nDotL = max(dot(hit.normal, toSun), 0.0);
            if (nDotL > 0.0) {
                RayDesc shadow;
                shadow.Origin = hit.position + hit.normal * 0.025;
                shadow.Direction = toSun;
                shadow.TMin = 0.02;
                shadow.TMax = 10000.0;
                RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                         RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> sunQuery;
                sunQuery.TraceRayInline(sceneTlas,
                    RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                    RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0x02, shadow);
                while (sunQuery.Proceed()) {}
                const float visibility = sunQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
                radiance += evaluateSunDiffuse(hit.normal, toSun, albedo,
                    frame.lightColor.rgb * frame.lightDirectionIntensity.w, visibility);
            }
        }
        if (!query.CommittedTriangleFrontFace()) distance = -distance;
    }
    rayData[int2(id.x, probeIndex)] = float4(radiance, distance);
}
