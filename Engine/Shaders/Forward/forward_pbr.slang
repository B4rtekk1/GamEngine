import Common.PbrLighting;
import Common.ReflectionFallback;
import Common.SceneData;
import Common.ShadowSampling;

// GENERATED_SHADER_GRAPH_DECLARATIONS

#ifndef MATERIAL_VARIANT
#define MATERIAL_VARIANT 0
#endif

#ifndef FORWARD_OUTPUT_VELOCITY
#define FORWARD_OUTPUT_VELOCITY 1
#endif

static const uint PBR_DEBUG_FINAL_LIGHTING = 0u;
static const uint PBR_DEBUG_NO_SPECULAR_IBL = 1u;
static const uint PBR_DEBUG_NO_DIRECT_SPECULAR = 2u;
static const uint PBR_DEBUG_NO_GTAO = 3u;
static const uint PBR_DEBUG_GEOMETRIC_DIRECT_DIFFUSE = 4u;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(5, 0)]] StructuredBuffer<RendererInstanceData> visibleInstances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleInstanceIndices;
#if FORWARD_OUTPUT_VELOCITY
[[vk::binding(8, 0)]] StructuredBuffer<RendererPreviousTransformData> previousTransforms;
#endif
// Dynamic GTAO is deliberately sampled independently of material AO.  It is
// full resolution after a depth-aware upsample and therefore needs no final
// HDR darkening pass.
[[vk::binding(16, 0)]] Sampler2D gtaoVisibility;

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
#if FORWARD_OUTPUT_VELOCITY
    [[vk::location(7)]] noperspective float2 currentNdc;
    [[vk::location(8)]] float4 previousClip;
#endif
};

[shader("vertex")]
ForwardVertexOutput vertexMain(ForwardVertexInput input, uint instanceIndex : SV_InstanceID,
                               uint baseInstance : SV_StartInstanceLocation)
{
    const uint visibleIndex = visibleInstanceIndices[baseInstance + instanceIndex];
    RendererInstanceData instance = visibleInstances[visibleIndex];
    const float3 localPosition = input.position;
    const float3 localNormal = input.normal;

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
    float3 previousWorldPosition = mul(input.position * previousTransform.previousScale.xyz, previousRotationMatrix) +
                                   previousTransform.previousPosition.xyz;
#endif
    output.position = currentClip;
    output.color = input.color;
    output.worldPosition = worldPosition.xyz;
    output.normal = mul(localNormal, normalMatrix);
    output.materialIndex = asuint(instance.positionMaterial.w) + input.materialIndex;
    output.texCoord = input.texCoord;
    output.texCoord1 = input.texCoord1;
    output.tangent = float4(mul(input.tangent.xyz, normalMatrix), input.tangent.w);
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
[[vk::binding(13, 0)]] Sampler2D materialTextures[];
[[vk::binding(4, 0)]] StructuredBuffer<uint4> shadowPageTable;
[[vk::binding(3, 0)]] StructuredBuffer<ClusterLightRange> clusterRanges;
[[vk::binding(7, 0)]] StructuredBuffer<uint> clusterLightIndices;
// A fixed-capacity table avoids a CPU probe choice per instance. Only
// frame.reflectionProbeCount entries are valid and may be read.
struct ReflectionProbeData
{
    float4 positionAndBlendDistance;
    float4 extentsAndShape;
    uint textureIndex;
    int priority;
    uint flags;
    uint padding;
};
[[vk::binding(14, 0)]] StructuredBuffer<ReflectionProbeData> reflectionProbes;
// Fixed capacity deliberately avoids descriptor indexing/count changes while
// probes are baked. Entry zero is the global prefiltered environment, so an
// unbaked probe is safe to sample immediately.
[[vk::binding(15, 0)]] SamplerCube reflectionProbeTextures[256];

static const uint Probe_Box = 1u << 0u;
static const uint Probe_BoxProject = 1u << 1u;
static const uint Probe_Enabled = 1u << 2u;

float probeInfluence(ReflectionProbeData probe, float3 worldPosition)
{
    if ((probe.flags & Probe_Enabled) == 0u) return 0.0;
    float3 delta = abs(worldPosition - probe.positionAndBlendDistance.xyz);
    float boundary;
    if ((probe.flags & Probe_Box) != 0u)
        boundary = min(min(probe.extentsAndShape.x - delta.x, probe.extentsAndShape.y - delta.y),
                       probe.extentsAndShape.z - delta.z);
    else
        boundary = probe.extentsAndShape.x - length(delta);
    return saturate(boundary / max(probe.positionAndBlendDistance.w, 1e-4));
}

float3 boxProjectedReflection(ReflectionProbeData probe, float3 worldPosition, float3 direction)
{
    if ((probe.flags & (Probe_Box | Probe_BoxProject)) != (Probe_Box | Probe_BoxProject)) return direction;
    float3 local = worldPosition - probe.positionAndBlendDistance.xyz;
    float3 safeDirection = direction + sign(direction) * 1e-5;
    float3 positive = (probe.extentsAndShape.xyz - local) / safeDirection;
    float3 negative = (-probe.extentsAndShape.xyz - local) / safeDirection;
    float3 distances = float3(safeDirection.x > 0.0 ? positive.x : negative.x,
                              safeDirection.y > 0.0 ? positive.y : negative.y,
                              safeDirection.z > 0.0 ? positive.z : negative.z);
    float hitDistance = min(min(distances.x, distances.y), distances.z);
    return normalize(local + direction * max(hitDistance, 0.0));
}

ReflectionCandidate sampleLocalProbes(float3 worldPosition, float3 reflection, float roughness,
                                      uint prefilteredMipCount, ClusterLightRange clusterRange)
{
    if (frame.reflectionProbeCount == 0u)
    {
        ReflectionCandidate none = {float3(0.0), 0.0};
        return none;
    }

    int bestPriority = -2147483647;
    int secondPriority = -2147483647;
    float bestWeight = 0.0;
    float secondWeight = 0.0;
    ReflectionProbeData best;
    ReflectionProbeData second;
    // The compute pass writes probe IDs with its high bit set into the same
    // compact Forward+ list as local lights.  This keeps selection GPU-driven
    // and bounds fragment work by MaxLightsPerCluster, not MaxProbes.
    [loop] for (uint candidate = 0u; candidate < clusterRange.count; ++candidate)
    {
        const uint encodedIndex = clusterLightIndices[clusterRange.offset + candidate];
        if ((encodedIndex & 0x80000000u) == 0u) continue;
        const uint index = encodedIndex & 0x7fffffffu;
        if (index >= frame.reflectionProbeCount) continue;
        ReflectionProbeData probe = reflectionProbes[index];
        float weight = probeInfluence(probe, worldPosition);
        if (weight <= 0.0) continue;
        if (probe.priority > bestPriority || (probe.priority == bestPriority && weight > bestWeight))
        {
            second = best; secondPriority = bestPriority; secondWeight = bestWeight;
            best = probe; bestPriority = probe.priority; bestWeight = weight;
        }
        else if (probe.priority > secondPriority || (probe.priority == secondPriority && weight > secondWeight))
        {
            second = probe; secondPriority = probe.priority; secondWeight = weight;
        }
    }
    float total = bestWeight + secondWeight;
    if (total <= 0.0) { ReflectionCandidate none = {float3(0.0), 0.0}; return none; }
    float mip = roughness * float(max(prefilteredMipCount, 1u) - 1u);
    const uint bestTexture = min(best.textureIndex, 255u);
    float3 value = reflectionProbeTextures[NonUniformResourceIndex(bestTexture)].SampleLevel(
        boxProjectedReflection(best, worldPosition, reflection), mip).rgb * bestWeight;
    if (secondWeight > 0.0)
    {
        const uint secondTexture = min(second.textureIndex, 255u);
        value += reflectionProbeTextures[NonUniformResourceIndex(secondTexture)].SampleLevel(
            boxProjectedReflection(second, worldPosition, reflection), mip).rgb * secondWeight;
    }
    ReflectionCandidate result = {value / total, saturate(total)};
    return result;
}

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

[shader("fragment")]
ForwardFragmentOutput fragmentMain(ForwardFragmentInput input)
{
    MaterialData material = materials[input.materialIndex];
    float3 vertexColor = input.color;
    if (input.color.r < 0.0)
    {
        float cellCount = max(input.color.g, 1.0);
        int2 cell = int2(clamp(floor(input.texCoord * cellCount),
                              float2(0.0), float2(cellCount - 1.0)));
        float shade = ((cell.x + cell.y) & 1) == 0 ? 0.82 : 0.52;
        vertexColor = shade;
    }

    // Quixel foliage uses COLOR_0 for wind/mask data, not pigment.  Preserve
    // glTF's usual vertex-colour modulation for every other material.
    float3 albedoMultiplier = (material.textureIndices.w & 32) != 0 ? 1.0 : vertexColor;
    float4 baseColor = float4(albedoMultiplier * material.baseColorMetallic.rgb,
                              material.roughnessAmbientOcclusion.w);
// Unlit materials never use terrain layering.  Keeping this out of their
// module is materially different from a runtime branch: the four descriptor
// samples and their live values disappear before register allocation.
#if MATERIAL_VARIANT != 1
    if ((material.textureIndices.w & 8) != 0)
    {
        baseColor = float4(material.baseColorMetallic.rgb, material.roughnessAmbientOcclusion.w);
        float3 weights = saturate(input.color);
        float fourthWeight = saturate(1.0 - weights.x - weights.y - weights.z);
        float4 layers = float4(weights.x, weights.y, weights.z, fourthWeight);
        // Terrain UVs cover the complete heightmap. Repeat each layer so an
        // albedo map reads as ground detail rather than one giant decal.
        // A denser repeat keeps brush-painted material layers at a usable
        // ground-detail scale instead of spreading one oversized texture
        // across a large section of the terrain.
        float2 terrainUv = input.texCoord * 16.0;
        int2 cell = int2(floor(input.texCoord * 32.0));
        float checkerShade = ((cell.x + cell.y) & 1) == 0 ? 0.82 : 0.52;
        float4 checker = float4(checkerShade * material.baseColorMetallic.rgb, 1.0);
        baseColor = 0.0;
        float appliedWeight = 0.0;
        if (material.terrainLayerTextures.x >= 0) { baseColor += materialTextures[NonUniformResourceIndex(material.terrainLayerTextures.x)].Sample(terrainUv) * layers.x; appliedWeight += layers.x; }
        if (material.terrainLayerTextures.y >= 0) { baseColor += materialTextures[NonUniformResourceIndex(material.terrainLayerTextures.y)].Sample(terrainUv) * layers.y; appliedWeight += layers.y; }
        if (material.terrainLayerTextures.z >= 0) { baseColor += materialTextures[NonUniformResourceIndex(material.terrainLayerTextures.z)].Sample(terrainUv) * layers.z; appliedWeight += layers.z; }
        if (material.terrainLayerTextures.w >= 0) { baseColor += materialTextures[NonUniformResourceIndex(material.terrainLayerTextures.w)].Sample(terrainUv) * layers.w; appliedWeight += layers.w; }
        // Blend the unpainted part into the grid instead of leaving it with
        // a zero alpha/colour, which exposed the blue clear colour at edges.
        baseColor = baseColor * float4(material.baseColorMetallic.rgb, 1.0) +
                    checker * (1.0 - saturate(appliedWeight));
        baseColor.a = 1.0;
    }
    else
#endif
    if (material.textureIndices.x >= 0)
        baseColor *= materialTextures[NonUniformResourceIndex(material.textureIndices.x)].Sample(materialTextureUv(material, 0, input.texCoord, input.texCoord1));

    // Cooker may pack this map into base-color alpha; retain the separate
    // path for uncooked glTF/Quixel assets.
    if (material.auxiliaryTextureIndices.y >= 0)
        baseColor.a *= materialTextures[NonUniformResourceIndex(material.auxiliaryTextureIndices.y)].Sample(materialTextureUv(material, 4, input.texCoord, input.texCoord1)).r;

    const bool doubleSided = (material.textureIndices.w & 1) != 0;
    const uint alphaMode = uint((material.textureIndices.w >> 1) & 3);
    // The current two-sided foliage stream is a depth-writing cutout stream.
    // Importers commonly mark vegetation cards as BLEND even though their
    // opacity map is binary. Treat those cards as cutouts until they are
    // routed through a dedicated, sorted transparent stream.
    if (alphaMode == 1)
    {
        if (baseColor.a < material.roughnessAmbientOcclusion.z)
            discard;
        baseColor.a = 1.0;
    }
    else if (alphaMode == 2)
    {
        if (baseColor.a < 0.30)
            discard;
        baseColor.a = 1.0;
    }
    else if (alphaMode == 0) baseColor.a = 1.0;

#if MATERIAL_VARIANT == 1
    // This return deliberately precedes metallic/roughness, normal mapping,
    // VSM, clusters, probes, GTAO and IBL.  The unlit PSO is consequently a
    // real compile-time specialization rather than a late colour override.
    float3 unlitEmissive = material.emissiveColorIntensity.rgb * material.emissiveColorIntensity.w;
    if (material.extensionTextureIndices.x >= 0)
        unlitEmissive *= materialTextures[NonUniformResourceIndex(material.extensionTextureIndices.x)]
            .Sample(materialTextureUv(material, 7, input.texCoord, input.texCoord1)).rgb;
    ForwardFragmentOutput unlitOutput;
    unlitOutput.color = float4(baseColor.rgb + unlitEmissive, baseColor.a);
#if FORWARD_OUTPUT_VELOCITY
    const float2 unlitPreviousNdc = input.previousClip.xy / input.previousClip.w;
    unlitOutput.velocity = (input.currentNdc - unlitPreviousNdc) * 0.5;
#endif
    return unlitOutput;
#endif

    float3 albedo = baseColor.rgb;
    float metallic = material.baseColorMetallic.a;
    float roughness = material.roughnessAmbientOcclusion.x;
    if (material.textureIndices.y >= 0)
    {
        float4 metallicRoughness =
            materialTextures[NonUniformResourceIndex(material.textureIndices.y)].Sample(materialTextureUv(material, 1, input.texCoord, input.texCoord1));
        metallic *= metallicRoughness.b;
        roughness *= metallicRoughness.g;
    }
    metallic = clamp(metallic, 0.0, 1.0);
    roughness = clamp(roughness, 0.045, 1.0);

    // Keep the unperturbed, geometric normal.  It is required to prevent a
    // normal map from reflecting the environment through the surface.
    float3 geometricNormal = normalize(input.normal);
    float faceSign = doubleSided && !input.isFrontFace ? -1.0 : 1.0;
    geometricNormal *= faceSign;
    float3 normal = geometricNormal;
    if (material.textureIndices.z >= 0 && abs(material.extensionScalars.x) > 1e-6)
    {
        float3 tangent = normalize(input.tangent.xyz - normal * dot(normal, input.tangent.xyz)) * faceSign;
        float3 bitangent = normalize(cross(normal, tangent)) * input.tangent.w;
        float3 mappedNormal =
            materialTextures[NonUniformResourceIndex(material.textureIndices.z)].Sample(materialTextureUv(material, 2, input.texCoord, input.texCoord1)).xyz * 2.0 - 1.0;
        mappedNormal.xy *= material.extensionScalars.x;
        float3x3 tangentFrame = float3x3(tangent, bitangent, normal);
        normal = normalize(mul(mappedNormal, tangentFrame));
    }

#if MATERIAL_VARIANT == 2
    // Hologram keeps its authored albedo and normal-map silhouette but does
    // not retain the full PBR/VSM/cluster/probe working set.
    const float3 hologramView = normalize(frame.cameraPosition.xyz - input.worldPosition);
    const float scanline = 0.55 + 0.45 * sin(input.worldPosition.y * 90.0 + frame.windGustFrequencyTime.w * 8.0);
    const float hologramFresnel = pow(1.0 - saturate(dot(normal, hologramView)), 3.0);
    const float3 hologramEmission = material.emissiveColorIntensity.rgb * material.emissiveColorIntensity.w;
    ForwardFragmentOutput hologramOutput;
    hologramOutput.color = float4(baseColor.rgb * (0.25 + scanline * 0.75) + hologramEmission +
                                       float3(0.05, 0.65, 1.0) * hologramFresnel, baseColor.a);
#if FORWARD_OUTPUT_VELOCITY
    const float2 hologramPreviousNdc = input.previousClip.xy / input.previousClip.w;
    hologramOutput.velocity = (input.currentNdc - hologramPreviousNdc) * 0.5;
#endif
    return hologramOutput;
#endif

    // This isolates the invariant baseline: no normal map, Fresnel, IBL,
    // specular, AO, shadows or local lights may affect it. It is deliberately
    // evaluated before material-graph normal overrides and specular AA.
    if (frame.pbrDebugView == PBR_DEBUG_GEOMETRIC_DIRECT_DIFFUSE)
    {
        ForwardFragmentOutput diagnosticOutput;
        const float3 diagnosticLightDirection = normalize(-frame.lightDirectionIntensity.xyz);
        diagnosticOutput.color = float4(albedo * max(dot(geometricNormal, diagnosticLightDirection), 0.0), baseColor.a);
#if FORWARD_OUTPUT_VELOCITY
        float2 previousNdc = input.previousClip.xy / input.previousClip.w;
        diagnosticOutput.velocity = (input.currentNdc - previousNdc) * 0.5;
#endif
        return diagnosticOutput;
    }

    float3 viewDirection = normalize(frame.cameraPosition.xyz - input.worldPosition);
#ifdef ENGINE_SHADER_GRAPH
    SurfaceInput graphInput;
    graphInput.worldPosition = input.worldPosition;
    graphInput.worldNormal = normal;
    graphInput.viewDirection = viewDirection;
    graphInput.uv0 = input.texCoord;
    graphInput.uv1 = input.texCoord1;
    graphInput.tangent = normalize(input.tangent.xyz);
    graphInput.bitangent = normalize(cross(normal, graphInput.tangent)) * input.tangent.w;
    graphInput.time = frame.windGustFrequencyTime.w;
    MaterialSurface graphSurface = evaluateMaterial(graphInput);
    baseColor = float4(graphSurface.baseColor, graphSurface.alpha);
    albedo = graphSurface.baseColor;
    metallic = clamp(graphSurface.metallic, 0.0, 1.0);
    roughness = clamp(graphSurface.roughness, 0.045, 1.0);
    normal = normalize(graphSurface.normal);
#endif
    // Specular anti-aliasing: high-frequency normal maps otherwise steer
    // adjacent pixels to unrelated parts of the environment, especially at
    // grazing angles. Derivative-based normal-to-roughness broadens the GGX
    // lobe before both direct-light and IBL BRDF evaluation.
    const float3 normalDx = ddx(normal);
    const float3 normalDy = ddy(normal);
    const float normalVariation = max(length(normalDx), length(normalDy));
    const float aaRoughness = saturate(normalVariation * 0.5);
    roughness = max(roughness, aaRoughness);

    float3 lightDirection = normalize(-frame.lightDirectionIntensity.xyz);
    float3 halfway = normalize(viewDirection + lightDirection);
    float3 dielectricF0 = float3(0.04);
    // KHR_materials_specular: strength is in the alpha channel of
    // specularTexture, while specularColorTexture provides its RGB tint.
    // Do not reinterpret non-glTF/legacy named specular maps as this
    // extension; they have different authoring conventions.
    if ((material.textureIndices.w & 64) != 0)
    {
        float specularStrength = saturate(material.extensionScalars.w);
        float3 specularColor = saturate(material.specularColor.rgb);
        if (material.extensionTextureIndices.y >= 0)
            specularStrength *= materialTextures[NonUniformResourceIndex(material.extensionTextureIndices.y)]
                .Sample(materialTextureUv(material, 8, input.texCoord, input.texCoord1)).a;
        if (material.extensionTextureIndices.z >= 0)
            specularColor *= materialTextures[NonUniformResourceIndex(material.extensionTextureIndices.z)]
                .Sample(materialTextureUv(material, 9, input.texCoord, input.texCoord1)).rgb;
        dielectricF0 *= specularStrength * specularColor;
    }
    float3 f0 = lerp(dielectricF0, albedo, metallic);
    float3 fresnel = fresnelSchlick(max(dot(halfway, viewDirection), 0.0), f0);
    float nDotV = max(dot(normal, viewDirection), 0.0);
    // Thin double-sided cards (leaves, ferns and grass-like glTF meshes) do
    // not have an opaque "back".  Their source winding is often arbitrary,
    // so use the incident-light magnitude on either face instead of letting
    // the camera-facing normal make the entire card black.
    float nDotL = doubleSided ? abs(dot(normal, lightDirection))
                              : max(dot(normal, lightDirection), 0.0);
    float geometry = geometrySchlickGGX(nDotV, roughness) *
                     geometrySchlickGGX(nDotL, roughness);
    float3 specular = distributionGGX(normal, halfway, roughness) * geometry * fresnel /
                      max(4.0 * nDotV * nDotL, 0.0001);
    const bool disableDirectSpecular = frame.pbrDebugView == PBR_DEBUG_NO_DIRECT_SPECULAR;
    if (disableDirectSpecular)
        specular = 0.0;
    // The diagnostic removes specular and restores its diffuse energy.  This
    // prevents view-dependent Fresnel from masquerading as a diffuse change.
    float3 diffuse = (1.0 - (disableDirectSpecular ? 0.0 : fresnel)) *
                     (1.0 - metallic) * albedo / PBR_PI;
    ShadowSampleDebug shadowDebug = {0.0, 0.0, 0u, 0u, uint2(0, 0)};
    float shadow = frame.shadowEnabled != 0
        ? sampleVirtualShadow(shadowMap, linearShadowMap, shadowDepthMap, input.worldPosition, normal, lightDirection, frame.shadowClipMatrices,
                              shadowPageTable, frame.shadowQuality, uint2(input.position.xy),
                              frame.temporalSampleIndex, frame.temporalShadowJitter != 0, shadowDebug) : 0.0;
    if (frame.shadowDebugView != SHADOW_DEBUG_OFF)
    {
        ForwardFragmentOutput debugOutput;
        debugOutput.color = float4(shadowDebugColor(frame.shadowDebugView, shadowDebug), baseColor.a);
#if FORWARD_OUTPUT_VELOCITY
        float2 previousNdc = input.previousClip.xy / input.previousClip.w;
        debugOutput.velocity = (input.currentNdc - previousNdc) * 0.5;
#endif
        return debugOutput;
    }
    if (frame.gtaoDebugView != 0u && frame.screenSpaceEffects != 0u)
    {
        ForwardFragmentOutput debugOutput;
        const float visibility = gtaoVisibility.Sample(input.position.xy / frame.clusteredViewportNearFar.xy).r;
        debugOutput.color = float4(visibility.xxx, baseColor.a);
#if FORWARD_OUTPUT_VELOCITY
        float2 previousNdc = input.previousClip.xy / input.previousClip.w;
        debugOutput.velocity = (input.currentNdc - previousNdc) * 0.5;
#endif
        return debugOutput;
    }
    float3 direct = (diffuse + specular) * frame.lightDirectionIntensity.w *
                    frame.lightColor.rgb * nDotL * (1.0 - shadow);
    const uint2 grid = uint2((uint(frame.clusteredViewportNearFar.x) + 63u) / 64u,
                             (uint(frame.clusteredViewportNearFar.y) + 63u) / 64u);
    const float nearPlane = frame.clusteredViewportNearFar.z;
    const float farPlane = frame.clusteredViewportNearFar.w;
    const float viewDepth = max(-mul(float4(input.worldPosition, 1.0), frame.view).z, nearPlane);
    const uint slice = min(uint(log(viewDepth / nearPlane) / log(farPlane / nearPlane) * 24.0), 23u);
    const uint2 tile = min(uint2(input.position.xy) / 64u, grid - 1u);
    const uint clusterIndex = (slice * grid.y + tile.y) * grid.x + tile.x;
    const ClusterLightRange lightRange = clusterRanges[clusterIndex];
    for (uint i = 0u; i < lightRange.count; ++i)
    {
        const uint lightIndex = clusterLightIndices[lightRange.offset + i];
        if ((lightIndex & 0x80000000u) != 0u) continue;
        if (lightIndex >= frame.localLightCount) continue;
        LocalLightData local = frame.localLights[lightIndex];
        float3 toLight = local.positionRange.xyz - input.worldPosition;
        float distanceToLight = length(toLight);
        if (distanceToLight <= 1e-4 || distanceToLight >= local.positionRange.w)
            continue;
        float3 localDirection = toLight / distanceToLight;
        float rangeFactor = saturate(1.0 - distanceToLight / local.positionRange.w);
        float attenuation = rangeFactor * rangeFactor /
                            max(distanceToLight * distanceToLight, 0.01);
        if (local.parameters.y > 1.5) // Spot
        {
            float cone = dot(normalize(-local.directionOuterCos.xyz), localDirection);
            attenuation *= smoothstep(local.directionOuterCos.w, local.parameters.x, cone);
        }
        float localNDotL = doubleSided ? abs(dot(normal, localDirection))
                                       : max(dot(normal, localDirection), 0.0);
        float3 localHalfway = normalize(viewDirection + localDirection);
        float3 localFresnel = fresnelSchlick(max(dot(localHalfway, viewDirection), 0.0), f0);
        float localGeometry = geometrySchlickGGX(nDotV, roughness) *
                              geometrySchlickGGX(localNDotL, roughness);
        float3 localSpecular = distributionGGX(normal, localHalfway, roughness) * localGeometry *
                               localFresnel / max(4.0 * nDotV * localNDotL, 0.0001);
        if (disableDirectSpecular)
            localSpecular = 0.0;
        float3 localDiffuse = (1.0 - (disableDirectSpecular ? 0.0 : localFresnel)) *
                              (1.0 - metallic) * albedo / PBR_PI;
        direct += (localDiffuse + localSpecular) * local.colorIntensity.rgb *
                  local.colorIntensity.w * localNDotL * attenuation;
    }
    float ao = 1.0;
    if (material.auxiliaryTextureIndices.x >= 0)
        ao = lerp(1.0, materialTextures[NonUniformResourceIndex(material.auxiliaryTextureIndices.x)].Sample(materialTextureUv(material, 3, input.texCoord, input.texCoord1)).r,
                  clamp(material.roughnessAmbientOcclusion.y, 0.0, 1.0));
#ifdef ENGINE_SHADER_GRAPH
    ao = graphSurface.ao;
#endif
    float3 emissive = material.emissiveColorIntensity.rgb * material.emissiveColorIntensity.w;
    if (material.extensionTextureIndices.x >= 0)
    {
        emissive *= materialTextures[NonUniformResourceIndex(material.extensionTextureIndices.x)]
            .Sample(materialTextureUv(material, 7, input.texCoord, input.texCoord1)).rgb;
    }
#ifdef ENGINE_SHADER_GRAPH
    emissive = graphSurface.emission;
#endif
    float3 fresnelIbl = fresnelSchlickRoughness(nDotV, f0, roughness);
    const bool disableSpecularIbl = frame.pbrDebugView == PBR_DEBUG_NO_SPECULAR_IBL;
    float3 kS = disableSpecularIbl ? 0.0 : fresnelIbl;
    float3 kD = (1.0 - kS) * (1.0 - metallic);
    float3 diffuseIbl = irradianceMap.Sample(normal).rgb * albedo * kD;
    float3 reflection = reflect(-viewDirection, normal);
    uint prefilteredWidth;
    uint prefilteredHeight;
    uint prefilteredMipCount;
    prefilteredEnvironmentMap.GetDimensions(0, prefilteredWidth, prefilteredHeight, prefilteredMipCount);
    float3 prefiltered = prefilteredEnvironmentMap.SampleLevel(
        reflection, roughness * float(max(prefilteredMipCount, 1u) - 1u)).rgb;
    // The current forward pass provides the global sky.  Local-probe, SSR and
    // RT passes populate the preceding candidates as their resources become
    // available; this explicit resolver keeps the material-side priority
    // fixed as RT -> SSR -> probe -> sky.
    ReflectionCandidate skyReflection = {prefiltered, 1.0};
    ReflectionCandidate localProbeReflection = sampleLocalProbes(input.worldPosition, reflection, roughness,
                                                                  prefilteredMipCount, lightRange);
    ReflectionCandidate screenSpaceReflection = {0.0, 0.0};
    ReflectionCandidate rayTracedReflection = {0.0, 0.0};
    prefiltered = resolveReflection(rayTracedReflection, screenSpaceReflection,
                                    localProbeReflection, skyReflection);
    float2 brdf = brdfIntegrationLut.Sample(float2(nDotV, roughness)).rg;
    float3 specularIbl = prefiltered * (fresnelIbl * brdf.x + brdf.y);
    // Horizon specular occlusion (Filament-style): if normal mapping turns
    // the reflection vector below the geometric surface, no environment
    // radiance is physically visible along that direction.
    const float horizon = saturate(1.0 + dot(reflection, geometricNormal));
    specularIbl *= horizon * horizon;
    if (disableSpecularIbl)
        specularIbl = 0.0;
    // Scene View has an independent render target and no matching GTAO
    // texture yet. Sampling the Game View result there mixes two unrelated
    // screen spaces and produces hard viewport-aligned seams.
    const float dynamicAo = frame.screenSpaceEffects != 0u && frame.pbrDebugView != PBR_DEBUG_NO_GTAO
        ? gtaoVisibility.Sample(input.position.xy /
            float2(frame.clusteredViewportNearFar.x, frame.clusteredViewportNearFar.y)).r
        : 1.0;
    // Material and screen-space visibility both describe indirect-light
    // accessibility, so neither is allowed to brighten the other.  The
    // GTAO visibility is used at full strength after its bilateral upsample.
    const float indirectVisibility = min(ao, dynamicAo);
    const float diffuseAO = indirectVisibility;
    // Lagarde's visibility-based specular-occlusion approximation.  It
    // suppresses sky/probe reflections in contact cavities without needing a
    // bent normal; rough surfaces converge to the diffuse visibility while
    // smooth ones retain the expected view-angle dependence.
    const float specularOcclusion = saturate(
        pow(saturate(nDotV) + indirectVisibility,
            exp2(-16.0 * roughness - 1.0)) - 1.0 + indirectVisibility);
    float3 ambient = diffuseIbl * diffuseAO + specularIbl * specularOcclusion;
    float3 color = ambient + direct + emissive;
    // Foliage has a cheap, stable back-lighting term.  The flag is deliberately
    // material data rather than an asset-name heuristic at runtime.
    if ((material.textureIndices.w & 16) != 0 && material.auxiliaryTextureIndices.z >= 0) {
        float mask = materialTextures[NonUniformResourceIndex(material.auxiliaryTextureIndices.z)].Sample(materialTextureUv(material, 5, input.texCoord, input.texCoord1)).r;
        float backLight = saturate(dot(-normal, lightDirection));
        color += albedo * frame.lightColor.rgb * frame.lightDirectionIntensity.w *
                 backLight * mask * material.extensionScalars.y;
    }

    ForwardFragmentOutput output;
    output.color = float4(color, baseColor.a);
#if FORWARD_OUTPUT_VELOCITY
    float2 previousNdc = input.previousClip.xy / input.previousClip.w;
    output.velocity = (input.currentNdc - previousNdc) * 0.5;
#endif
    return output;
}
