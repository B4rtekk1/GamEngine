import Common.PbrLighting;
import Common.ReflectionFallback;
import Common.ReflectionProbes;
import Common.SceneData;
import Common.ShadowSampling;
import Common.Displacement;

#ifndef FORWARD_OUTPUT_VELOCITY
#define FORWARD_OUTPUT_VELOCITY 1
#endif
#ifndef FORWARD_RESOLVED_VSM
#define FORWARD_RESOLVED_VSM 0
#endif
#ifndef FORWARD_MESHLET_INDEXED
#define FORWARD_MESHLET_INDEXED 0
#endif

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(5, 0)]] StructuredBuffer<RendererInstanceData> visibleInstances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleInstanceIndices;
#if FORWARD_OUTPUT_VELOCITY
[[vk::binding(8, 0)]] StructuredBuffer<RendererPreviousTransformData> previousTransforms;
#endif
struct ForwardVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR0;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(4)]] float2 texCoord1 : TEXCOORD1;
    [[vk::location(3)]] float3 normal : NORMAL;
    [[vk::location(8)]] uint materialIndex : MATERIAL_INDEX;
    [[vk::location(9)]] float4 tangent : TANGENT;
};

struct ForwardVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 color;
    [[vk::location(1)]] float3 worldPosition;
    [[vk::location(2)]] float3 normal;
    [[vk::location(3)]] nointerpolation uint materialIndex;
    [[vk::location(4)]] float2 texCoord;
    [[vk::location(5)]] float4 tangent;
    [[vk::location(6)]] float2 texCoord1;
    [[vk::location(9)]] float3 baseWorldPosition;
    [[vk::location(10)]] float3 worldDisplacementDirection;
    [[vk::location(11)]] nointerpolation uint crackSlot;
    [[vk::location(12)]] float2 crackPosition;
#if FORWARD_OUTPUT_VELOCITY
    [[vk::location(7)]] noperspective float2 currentNdc;
    [[vk::location(8)]] float4 previousClip;
#endif
};

[shader("vertex")]
ForwardVertexOutput vertexMain(ForwardVertexInput input, uint instanceIndex : SV_InstanceID,
                               uint baseInstance : SV_StartInstanceLocation)
{
#if FORWARD_MESHLET_INDEXED
    const uint visibleIndex = baseInstance + instanceIndex;
#else
    const uint visibleIndex = visibleInstanceIndices[baseInstance + instanceIndex];
#endif
    RendererInstanceData instance = visibleInstances[visibleIndex];
    const uint materialIndex = asuint(instance.positionMaterial.w) + input.materialIndex;
    MaterialData material = materials[materialIndex];

    const DisplacedVertex displaced = displaceMaterialVertex(
        material, input.position, input.normal, input.tangent,
        input.texCoord, input.texCoord1);
    const float3 localPosition = displaced.position;

    float4 q = instance.rotation;
    float3x3 rotationMatrix = float3x3(
        1.0 - 2.0 * (q.y * q.y + q.z * q.z), 2.0 * (q.x * q.y + q.z * q.w), 2.0 * (q.x * q.z - q.y * q.w),
        2.0 * (q.x * q.y - q.z * q.w), 1.0 - 2.0 * (q.x * q.x + q.z * q.z), 2.0 * (q.y * q.z + q.x * q.w),
        2.0 * (q.x * q.z + q.y * q.w), 2.0 * (q.y * q.z - q.x * q.w), 1.0 - 2.0 * (q.x * q.x + q.y * q.y));
    float3 worldPosition3 = mul(localPosition * instance.scaleBase.xyz, rotationMatrix) + instance.positionMaterial.xyz;
    float4 worldPosition = float4(worldPosition3, 1.0);
    float3x3 normalMatrix = float3x3(rotationMatrix[0] / instance.scaleBase.x,
                                     rotationMatrix[1] / instance.scaleBase.y,
                                     rotationMatrix[2] / instance.scaleBase.z);

    ForwardVertexOutput output;
    float4 currentClip = mul(mul(worldPosition, frame.view), frame.projection);
#if FORWARD_OUTPUT_VELOCITY
    RendererPreviousTransformData previousTransform = previousTransforms[visibleIndex];
    float4 previousQ = previousTransform.previousRotation;
    float3x3 previousRotationMatrix = float3x3(
        1.0 - 2.0 * (previousQ.y * previousQ.y + previousQ.z * previousQ.z), 2.0 * (previousQ.x * previousQ.y + previousQ.z * previousQ.w), 2.0 * (previousQ.x * previousQ.z - previousQ.y * previousQ.w),
        2.0 * (previousQ.x * previousQ.y - previousQ.z * previousQ.w), 1.0 - 2.0 * (previousQ.x * previousQ.x + previousQ.z * previousQ.z), 2.0 * (previousQ.y * previousQ.z + previousQ.x * previousQ.w),
        2.0 * (previousQ.x * previousQ.z + previousQ.y * previousQ.w), 2.0 * (previousQ.y * previousQ.z - previousQ.x * previousQ.w), 1.0 - 2.0 * (previousQ.x * previousQ.x + previousQ.y * previousQ.y));
    float3 previousWorldPosition = mul(localPosition * previousTransform.previousScale.xyz, previousRotationMatrix) +
                                   previousTransform.previousPosition.xyz;
#endif
    output.position = currentClip;
    output.color = input.color;
    output.worldPosition = worldPosition.xyz;
    output.normal = mul(displaced.normal, normalMatrix);
    output.baseWorldPosition = mul(input.position * instance.scaleBase.xyz, rotationMatrix) +
                               instance.positionMaterial.xyz;
    output.worldDisplacementDirection = mul(normalize(input.normal) * instance.scaleBase.xyz,
                                            rotationMatrix);
    output.materialIndex = materialIndex;
    output.crackSlot = asuint(instance.scaleBase.w);
    output.crackPosition = input.position.xy;
    output.texCoord = input.texCoord;
    output.texCoord1 = input.texCoord1;
    output.tangent = float4(mul(displaced.tangent.xyz * instance.scaleBase.xyz, rotationMatrix),
                            displaced.tangent.w);
#if FORWARD_OUTPUT_VELOCITY
    output.currentNdc = currentClip.xy / currentClip.w;
    output.previousClip = mul(mul(float4(previousWorldPosition, 1.0), frame.previousView), frame.previousProjection);
#endif
    return output;
}

[[vk::binding(0, 0)]] Sampler2DShadow shadowMap;
[[vk::binding(9, 0)]] Sampler2D shadowDepthMap;
[[vk::binding(17, 0)]] Sampler2DShadow linearShadowMap;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(10, 0)]] SamplerCube irradianceMap;
[[vk::binding(11, 0)]] SamplerCube prefilteredEnvironmentMap;
[[vk::binding(12, 0)]] Sampler2D brdfIntegrationLut;
[[vk::binding(4, 0)]] StructuredBuffer<uint4> shadowPageTable;
[[vk::binding(3, 0)]] StructuredBuffer<ClusterLightRange> clusterRanges;
[[vk::binding(7, 0)]] StructuredBuffer<uint> clusterLightIndices;
struct ForwardFragmentInput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 color;
    [[vk::location(1)]] float3 worldPosition;
    [[vk::location(2)]] float3 normal;
    [[vk::location(3)]] nointerpolation uint materialIndex;
    [[vk::location(4)]] float2 texCoord;
    [[vk::location(5)]] float4 tangent;
    [[vk::location(6)]] float2 texCoord1;
    [[vk::location(9)]] float3 baseWorldPosition;
    [[vk::location(10)]] float3 worldDisplacementDirection;
    [[vk::location(11)]] nointerpolation uint crackSlot;
    [[vk::location(12)]] float2 crackPosition;
#if FORWARD_OUTPUT_VELOCITY
    [[vk::location(7)]] noperspective float2 currentNdc;
    [[vk::location(8)]] float4 previousClip;
#endif
    bool isFrontFace : SV_IsFrontFace;
};

struct ForwardFragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target;
#if FORWARD_OUTPUT_VELOCITY
    [[vk::location(1)]] float2 velocity : SV_Target1;
#endif
};

// Immutable opaque snapshot: never sample the HDR attachment being written.
[[vk::binding(0, 1)]] Sampler2D opaqueSceneColor;
[[vk::binding(1, 1)]] Sampler2D opaqueSceneDepth;
[[vk::binding(2, 1)]] Sampler2DArray glassCrackAtlas;
[[vk::binding(4, 1)]] StructuredBuffer<float4> glassCrackMetadata;

float2 glassUv(MaterialData m, uint slot, float2 uv0, float2 uv1)
{
    float2 uv = m.glassTextureCoordinateSets[slot] == 1 ? uv1 : uv0;
    return float2(dot(uv, m.glassTextureTransforms[slot].xy),
                  dot(uv, m.glassTextureRows1[slot].xy)) + m.glassTextureTransforms[slot].zw;
}

float3 viewPosition(float2 uv, float depth)
{
    float4 p = mul(float4(uv * 2.0 - 1.0, depth, 1.0), frame.inverseProjection);
    return p.xyz / max(p.w, 1e-6);
}

bool projectView(float3 p, out float2 uv)
{
    float4 clip = mul(float4(p, 1.0), frame.projection);
    uv = clip.xy / max(clip.w, 1e-6) * 0.5 + 0.5;
    return clip.w > 0.0 && all(uv > 0.0) && all(uv < 1.0);
}

// Start from the glass surface, not the opaque normal/depth behind it.
// A bounded view-space march falls back to parallax-corrected probes and sky.
ReflectionCandidate glassSsr(float3 worldPosition, float3 N, float3 R, float roughness)
{
    ReflectionCandidate miss = {float3(0.0), 0.0};
    float3 origin = mul(float4(worldPosition + N * 0.002, 1.0), frame.view).xyz;
    float3 ray = normalize(mul(float4(R, 0.0), frame.view).xyz);
    float previousT = 0.0;
    bool wasInFront = false;
    [loop] for (uint i = 0; i < 24; ++i)
    {
        float t = 0.025 * (pow(1.32, float(i + 1)) - 1.0);
        float3 p = origin + ray * t;
        float2 uv;
        if (!projectView(p, uv)) return miss;
        float depth = opaqueSceneDepth.SampleLevel(uv, 0.0).r;
        if (depth >= 0.99999) { previousT = t; wasInFront = true; continue; }
        float sceneZ = viewPosition(uv, depth).z;
        float delta = sceneZ - p.z;
        if (delta >= 0.0 && wasInFront)
        {
            float lo = previousT, hi = t;
            [unroll] for (uint j = 0; j < 5; ++j)
            {
                float mid = (lo + hi) * 0.5;
                float3 q = origin + ray * mid;
                if (!projectView(q, uv)) return miss;
                float d = opaqueSceneDepth.SampleLevel(uv, 0.0).r;
                float z = viewPosition(uv, d).z;
                if (z - q.z >= 0.0) hi = mid; else lo = mid;
            }
            float3 hit = origin + ray * hi;
            if (!projectView(hit, uv)) return miss;
            float hitDepth = opaqueSceneDepth.SampleLevel(uv, 0.0).r;
            float hitZ = viewPosition(uv, hitDepth).z;
            if (hitDepth >= 0.99999 || abs(hitZ - hit.z) > max(0.03, abs(hitZ) * 0.002)) return miss;
            float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
            ReflectionCandidate result = {opaqueSceneColor.SampleLevel(uv, 0.0).rgb,
                smoothstep(0.0, 0.06, edge) * (1.0 - smoothstep(0.15, 0.5, roughness))};
            return result;
        }
        wasInFront = delta < 0.0;
        previousT = t;
    }
    return miss;
}

float3 glassBrdf(float3 N, float3 V, float3 L, float roughness, float3 f0, float3 diffuseAlbedo)
{
    float nv = max(dot(N, V), 1e-4), nl = saturate(dot(N, L));
    if (nl <= 0.0) return 0.0;
    float3 H = normalize(V + L);
    float3 F = fresnelSchlick(saturate(dot(H, V)), f0);
    float alpha2 = pow(roughness, 4.0);
    float nh = saturate(dot(N, H));
    float denom = nh * nh * (alpha2 - 1.0) + 1.0;
    float D = alpha2 / max(PBR_PI * denom * denom, 1e-10);
    float lambdaV = nl * sqrt(nv * nv * (1.0 - alpha2) + alpha2);
    float lambdaL = nv * sqrt(nl * nl * (1.0 - alpha2) + alpha2);
    float visibility = 0.5 / max(lambdaV + lambdaL, 1e-6);
    return (F * D * visibility + (1.0 - F) * diffuseAlbedo / PBR_PI) * nl;
}

float3 sampleTransmission(float2 uv, float glassDepth, float radius)
{
    float3 center = opaqueSceneColor.SampleLevel(uv, 0.0).rgb;
    if (radius < 0.75) return center;
    // Bounded footprint approximation until a shared HDR mip pyramid exists.
    float3 sum = center * 4.0;
    float weight = 4.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        if (x == 0 && y == 0) continue;
        float2 tap = uv + float2(x, y) * radius / frame.clusteredViewportNearFar.xy;
        if (any(tap <= 0.0) || any(tap >= 1.0)) continue;
        if (opaqueSceneDepth.SampleLevel(tap, 0.0).r <= glassDepth) continue;
        float w = x == 0 || y == 0 ? 2.0 : 1.0;
        sum += opaqueSceneColor.SampleLevel(tap, 0.0).rgb * w;
        weight += w;
    }
    return sum / weight;
}

[shader("fragment")]
ForwardFragmentOutput fragmentMain(ForwardFragmentInput input)
{
    MaterialData m = materials[input.materialIndex];
    float2 screenUv = input.position.xy / frame.clusteredViewportNearFar.xy;
    float4 base = float4(m.baseColorMetallic.rgb, m.roughnessAmbientOcclusion.w);
    if (m.textureIndices.x >= 0)
        base *= materialTextures[NonUniformResourceIndex(m.textureIndices.x)]
            .Sample(materialTextureUv(m, 0, input.texCoord, input.texCoord1));
    uint alphaMode = (uint(m.textureIndices.w) >> 1u) & 3u;
    if (alphaMode == 1u && base.a < m.roughnessAmbientOcclusion.z) discard;
    float roughness = m.roughnessAmbientOcclusion.x;
    float metallic = m.baseColorMetallic.w;
    if (m.textureIndices.y >= 0)
    {
        float4 mr = materialTextures[NonUniformResourceIndex(m.textureIndices.y)]
            .Sample(materialTextureUv(m, 1, input.texCoord, input.texCoord1));
        roughness *= mr.g; metallic *= mr.b;
    }
    float transmission = saturate(m.glassParams.x) * (1.0 - saturate(metallic));
    float thickness = max(m.glassParams.z, 0.0);
    if (m.glassTextureIndices.x >= 0)
        transmission *= materialTextures[NonUniformResourceIndex(m.glassTextureIndices.x)]
            .Sample(glassUv(m, 0, input.texCoord, input.texCoord1)).r;
    if (m.glassTextureIndices.y >= 0)
        thickness *= materialTextures[NonUniformResourceIndex(m.glassTextureIndices.y)]
            .Sample(glassUv(m, 1, input.texCoord, input.texCoord1)).g;
    float dirt = saturate(m.glassParams.w);
    if (m.glassTextureIndices.z >= 0)
        dirt *= materialTextures[NonUniformResourceIndex(m.glassTextureIndices.z)]
            .Sample(glassUv(m, 2, input.texCoord, input.texCoord1)).r;
    roughness = clamp(lerp(roughness, max(roughness, 0.25), dirt), 0.02, 1.0);
    transmission *= 1.0 - dirt * 0.15;
    float3 V = normalize(frame.cameraPosition.xyz - input.worldPosition);
    float3 geometricNormal = normalize(input.normal);
    if (dot(geometricNormal, V) < 0.0) geometricNormal = -geometricNormal;
    float3 N = geometricNormal;
    if (m.textureIndices.z >= 0 && abs(m.extensionScalars.x) > 1e-6)
    {
        float3 tangent = normalize(input.tangent.xyz - N * dot(N, input.tangent.xyz));
        float3 bitangent = cross(N, tangent) * input.tangent.w;
        float2 xy = materialTextures[NonUniformResourceIndex(m.textureIndices.z)]
            .Sample(materialTextureUv(m, 2, input.texCoord, input.texCoord1)).rg * 2.0 - 1.0;
        if ((m.textureIndices.w & 128) != 0) xy.y = -xy.y;
        float z = sqrt(saturate(1.0 - dot(xy, xy)));
        N = normalize(tangent * xy.x * m.extensionScalars.x +
                      bitangent * xy.y * m.extensionScalars.x + N * z);
        if (dot(N, V) < 0.0) N = geometricNormal;
    }
    // Localized fracture roughness and normal perturbation; the remaining pane
    // retains its original optical properties. Slots are shared by its shards.
    if (input.crackSlot != 0)
    {
        uint slot = input.crackSlot - 1;
        // Packed side vertices carry edge/fragment indices. Detached shards have
        // all bits set; attached cells expose only outer or broken boundaries.
        if (input.texCoord1.y > 0.5 && input.texCoord1.x > 0.5) {
            uint fragment = uint(input.texCoord1.y + 0.5) - 1;
            uint edge = uint(input.texCoord1.x + 0.5) - 1;
            uint4 exposed = asuint(glassCrackMetadata[slot * 129 + 1 + fragment]);
            if (edge < 128 && (exposed[edge / 32] & (1u << (edge % 32))) == 0) discard;
        }
        float2 size = max(glassCrackMetadata[slot * 129].xy, float2(0.001));
        float2 uv = input.crackPosition / size + 0.5;
        float active = glassCrackMetadata[slot * 129].w;
        float crack = glassCrackAtlas.SampleLevel(float3(uv, slot), 0.0).r * active;
        uint atlasWidth, atlasHeight, atlasLayers;
        glassCrackAtlas.GetDimensions(atlasWidth, atlasHeight, atlasLayers);
        float2 step = 1.0 / float2(atlasWidth, atlasHeight);
        float2 gradient = float2(
            glassCrackAtlas.SampleLevel(float3(uv + float2(step.x, 0), slot), 0.0).r -
            glassCrackAtlas.SampleLevel(float3(uv - float2(step.x, 0), slot), 0.0).r,
            glassCrackAtlas.SampleLevel(float3(uv + float2(0, step.y), slot), 0.0).r -
            glassCrackAtlas.SampleLevel(float3(uv - float2(0, step.y), slot), 0.0).r);
        gradient *= active;
        float3 tangent = normalize(input.tangent.xyz - N * dot(N, input.tangent.xyz));
        float3 bitangent = cross(N, tangent) * input.tangent.w;
        N = normalize(N + (tangent * gradient.x + bitangent * gradient.y) * 0.18);
        roughness = lerp(roughness, max(roughness, 0.5), crack);
        transmission *= 1.0 - crack * 0.22;
    }
    float ior = max(m.glassParams.y, 1.0);
    float f0Scalar = pow((ior - 1.0) / (ior + 1.0), 2.0);
    float3 f0 = lerp(float3(f0Scalar), base.rgb, saturate(metallic));
    float3 F = fresnelSchlick(saturate(dot(N, V)), f0);
    float3 I = -V;
    float3 T = refract(I, N, 1.0 / ior);
    float travel = thickness / max(abs(dot(T, N)), 0.1);
    // Parallel air/glass/air interfaces: outgoing direction equals incident.
    // Project only the lateral displacement, which vanishes at IOR=1 or normal incidence.
    float3 offset = T * travel - I * (thickness / max(abs(dot(I, N)), 0.1));
    float2 refractedUv;
    float3 displacedView = mul(float4(input.worldPosition + offset, 1.0), frame.view).xyz;
    if (!projectView(displacedView, refractedUv) ||
        opaqueSceneDepth.SampleLevel(refractedUv, 0.0).r <= input.position.z)
        refractedUv = screenUv;
    float sceneDepth = opaqueSceneDepth.SampleLevel(refractedUv, 0.0).r;
    float3 background = viewPosition(refractedUv, sceneDepth);
    float3 surfaceView = mul(float4(input.worldPosition, 1.0), frame.view).xyz;
    float radius = roughness * roughness * length(background - surfaceView) *
        abs(frame.projection[1][1]) * frame.clusteredViewportNearFar.y * 0.5 / max(-background.z, 0.1);
    float3 transmitted = sampleTransmission(refractedUv, input.position.z, min(radius, 12.0));
    float3 absorption = float3(1.0);
    if (m.glassAttenuation.w > 0.0)
        absorption = exp(log(clamp(m.glassAttenuation.rgb, 1e-4, 1.0)) * travel / m.glassAttenuation.w);
    absorption *= lerp(float3(1.0), float3(0.65, 0.62, 0.56), dirt * 0.25);
    transmitted *= base.rgb * absorption;

    ClusterLightRange range = {0u, 0u, 0u, 0u};
    if (frame.localLightCount != 0u || frame.reflectionProbeCount != 0u)
    {
        uint2 grid = uint2((uint(frame.clusteredViewportNearFar.x) + 63u) / 64u,
                          (uint(frame.clusteredViewportNearFar.y) + 63u) / 64u);
        float depth = max(-surfaceView.z, frame.clusteredViewportNearFar.z);
        uint slice = min(uint(max(log2(depth) * frame.clusterZScaleBiasEnvironmentMipRtContact.x +
                                  frame.clusterZScaleBiasEnvironmentMipRtContact.y, 0.0)), 23u);
        uint2 tile = min(uint2(input.position.xy) / 64u, grid - 1u);
        range = clusterRanges[(slice * grid.y + tile.y) * grid.x + tile.x];
    }
    uint envWidth, envHeight, mipCount;
    prefilteredEnvironmentMap.GetDimensions(0, envWidth, envHeight, mipCount);
    float3 R = reflect(-V, N);
    if (dot(R, geometricNormal) < 0.0) R = reflect(-V, geometricNormal);
    ReflectionCandidate sky = {prefilteredEnvironmentMap.SampleLevel(R, roughness * float(max(mipCount, 1u) - 1u)).rgb, 1.0};
    ReflectionCandidate probe = sampleLocalProbes(frame, clusterLightIndices, input.worldPosition, R, roughness, mipCount, range);
    ReflectionCandidate ssr = glassSsr(input.worldPosition, geometricNormal, R, roughness);
    ReflectionCandidate noRt = {float3(0.0), 0.0};
    float3 reflection = resolveReflection(noRt, ssr, probe, sky);
    float3 diffuseAlbedo = base.rgb * (1.0 - transmission) * (1.0 - saturate(metallic));
    float3 L = normalize(frame.lightDirectionIntensity.xyz);
    ShadowSampleDebug shadowDebug = {0.0, 0.0, 0u, 0u, uint2(0, 0), 0u, 0u};
    float shadow = frame.shadowEnabled != 0
        ? sampleVirtualShadow(shadowMap, linearShadowMap, shadowDepthMap, input.worldPosition, geometricNormal, L,
                              frame.shadowClipMatrices, frame.shadowClipConstants, shadowPageTable,
                              frame.shadowQuality, uint2(input.position.xy), frame.temporalSampleIndex,
                              frame.temporalShadowJitter != 0, shadowDebug) : 0.0;
    float3 direct = glassBrdf(N, V, L, roughness, f0, diffuseAlbedo) *
        frame.lightColor.rgb * frame.lightDirectionIntensity.w * (1.0 - shadow);
    [loop] for (uint i = 0; i < range.lightCount; ++i)
    {
        uint index = clusterLightIndices[range.lightOffset + i];
        if (index >= frame.localLightCount) continue;
        LocalLightData light = frame.localLights[index];
        float3 delta = light.positionRange.xyz - input.worldPosition;
        float distance = length(delta);
        if (distance <= 1e-4 || distance >= light.positionRange.w) continue;
        float3 localL = delta / distance;
        float attenuation = pow(saturate(1.0 - distance / light.positionRange.w), 2.0) / max(distance * distance, 0.01);
        if (light.parameters.y > 1.5)
            attenuation *= smoothstep(light.directionOuterCos.w, light.parameters.x,
                                      dot(normalize(-light.directionOuterCos.xyz), localL));
        direct += glassBrdf(N, V, localL, roughness, f0, diffuseAlbedo) * light.colorIntensity.rgb *
                  light.colorIntensity.w * attenuation;
    }
    float3 color = F * reflection + (1.0 - F) * transmitted * transmission + direct +
        (1.0 - F) * irradianceMap.SampleLevel(N, 0.0).rgb * diffuseAlbedo;
    float3 emissive = m.emissiveColorIntensity.rgb * m.emissiveColorIntensity.w;
    if (m.extensionTextureIndices.x >= 0)
        emissive *= materialTextures[NonUniformResourceIndex(m.extensionTextureIndices.x)]
            .Sample(materialTextureUv(m, 7, input.texCoord, input.texCoord1)).rgb;
    color += emissive;
    if (alphaMode == 2u)
        color = lerp(opaqueSceneColor.SampleLevel(screenUv, 0.0).rgb, color, saturate(base.a));
    ForwardFragmentOutput output;
    // Negative HDR alpha is reserved for transmissive temporal reactivity.
    // Color is fully composed; blending remains disabled.
    output.color = float4(max(color, 0.0), -1.0);
#if FORWARD_OUTPUT_VELOCITY
    float2 previousNdc = input.previousClip.xy / max(input.previousClip.w, 1e-6);
    output.velocity = (input.currentNdc - previousNdc) * 0.5;
#endif
    return output;
}
