import Common.PbrLighting;
import Common.SceneData;
import Common.ShadowSampling;
import Common.GrassWind;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(5, 0)]] StructuredBuffer<RendererInstanceData> visibleGrassInstances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleGrassIndices;

struct ForwardVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR0;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
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
};

[shader("vertex")]
ForwardVertexOutput vertexMain(ForwardVertexInput input, uint instanceIndex : SV_InstanceID,
                               uint baseInstance : SV_StartInstanceLocation)
{
    RendererInstanceData instance = visibleGrassInstances[visibleGrassIndices[baseInstance + instanceIndex]];
    float3 localPosition = input.position;
    float3 localNormal = input.normal;
    float normalizedHeight = clamp((input.position.y - instance.scaleBase.w) *
                                   instance.grassDeformation.w, 0.0, 1.0);
    // Smooth S-shaped curvature keeps the root planted and the tip relaxed,
    // rather than making the blade look like a translated rigid card.
    float bendCurve = normalizedHeight * normalizedHeight * (3.0 - 2.0 * normalizedHeight);
    float bendWeight = bendCurve * instance.grassDeformation.z;
    float grassHeight = instance.grassDeformation.w > 0.0 ? 1.0 / instance.grassDeformation.w : 0.0;
    localPosition.xz += instance.grassDeformation.xy * bendWeight * grassHeight * 0.8;
    localPosition.y -= bendWeight * (0.25 + 0.30 * normalizedHeight) * grassHeight;
    // Wind is authored in world space. Convert it into the card's local
    // frame first, otherwise randomized blade yaw makes neighbouring grass
    // sway in unrelated directions.
    float4 inverseInstanceRotation = float4(-instance.rotation.xyz, instance.rotation.w);
    float3 windOffset = grassWindDisplacement(instance.positionMaterial.xz, inverseInstanceRotation,
                                              normalizedHeight, grassHeight,
                                              frame.windGustFrequencyTime.z,
                                              frame.windDirectionStrength,
                                              frame.windSourcePositionRange,
                                              frame.windGustFrequencyTime);
    localPosition += windOffset;
    float bendSlope = 1.6 * normalizedHeight * (1.0 - normalizedHeight) * instance.grassDeformation.z;
    localNormal = normalize(localNormal + float3(-instance.grassDeformation.x * bendSlope,
                                                0.0,
                                               -instance.grassDeformation.y * bendSlope));
    float windSlope = 1.8 * normalizedHeight / max(grassHeight, 1.0e-4);
    localNormal = normalize(localNormal + float3(-windOffset.x * windSlope, 0.0,
                                                  -windOffset.z * windSlope));

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
    output.position = mul(mul(worldPosition, frame.view), frame.projection);
    output.color = input.color;
    output.worldPosition = worldPosition.xyz;
    output.normal = mul(localNormal, normalMatrix);
    output.materialIndex = asuint(instance.positionMaterial.w) + input.materialIndex;
    output.texCoord = input.texCoord;
    output.tangent = float4(mul(input.tangent.xyz, normalMatrix), input.tangent.w);
    return output;
}

[[vk::binding(0, 0)]] Sampler2DShadow shadowMap;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(3, 0)]] Sampler2D materialTextures[16];
[[vk::binding(4, 0)]] StructuredBuffer<uint4> shadowPageTable;

struct ForwardFragmentInput
{
    [[vk::location(0)]] float3 color;
    [[vk::location(1)]] float3 worldPosition;
    [[vk::location(2)]] float3 normal;
    [[vk::location(3)]] nointerpolation uint materialIndex;
    [[vk::location(4)]] float2 texCoord;
    [[vk::location(5)]] float4 tangent;
    bool isFrontFace : SV_IsFrontFace;
};

struct ForwardFragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target;
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

    float4 baseColor = float4(vertexColor * material.baseColorMetallic.rgb, 1.0);
    if ((material.textureIndices.w & 8) != 0)
    {
        baseColor = float4(material.baseColorMetallic.rgb, 1.0);
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
        if (material.terrainLayerTextures.x >= 0) { baseColor += materialTextures[material.terrainLayerTextures.x].Sample(terrainUv) * layers.x; appliedWeight += layers.x; }
        if (material.terrainLayerTextures.y >= 0) { baseColor += materialTextures[material.terrainLayerTextures.y].Sample(terrainUv) * layers.y; appliedWeight += layers.y; }
        if (material.terrainLayerTextures.z >= 0) { baseColor += materialTextures[material.terrainLayerTextures.z].Sample(terrainUv) * layers.z; appliedWeight += layers.z; }
        if (material.terrainLayerTextures.w >= 0) { baseColor += materialTextures[material.terrainLayerTextures.w].Sample(terrainUv) * layers.w; appliedWeight += layers.w; }
        // Blend the unpainted part into the grid instead of leaving it with
        // a zero alpha/colour, which exposed the blue clear colour at edges.
        baseColor = baseColor * float4(material.baseColorMetallic.rgb, 1.0) +
                    checker * (1.0 - saturate(appliedWeight));
        baseColor.a = 1.0;
    }
    else if (material.textureIndices.x >= 0)
        baseColor *= materialTextures[material.textureIndices.x].Sample(input.texCoord);

    // Cooker may pack this map into base-color alpha; retain the separate
    // path for uncooked glTF/Quixel assets.
    if (material.auxiliaryTextureIndices.y >= 0)
        baseColor.a *= materialTextures[material.auxiliaryTextureIndices.y].Sample(input.texCoord).r;

    const bool doubleSided = (material.textureIndices.w & 1) != 0;
    const uint alphaMode = uint((material.textureIndices.w >> 1) & 3);
    // 0 = opaque, 1 = mask, 2 = blend.  Only MASK discards fragments.
    if (alphaMode == 1)
    {
        if (baseColor.a < material.roughnessAmbientOcclusion.z)
            discard;
        baseColor.a = 1.0;
    }
    else if (alphaMode == 0) baseColor.a = 1.0;

    float3 albedo = baseColor.rgb;
    float metallic = material.baseColorMetallic.a;
    float roughness = material.roughnessAmbientOcclusion.x;
    if (material.textureIndices.y >= 0)
    {
        float4 metallicRoughness =
            materialTextures[material.textureIndices.y].Sample(input.texCoord);
        metallic *= metallicRoughness.b;
        roughness *= metallicRoughness.g;
    }
    metallic = clamp(metallic, 0.0, 1.0);
    roughness = clamp(roughness, 0.045, 1.0);

    float3 normal = normalize(input.normal);
    float faceSign = doubleSided && !input.isFrontFace ? -1.0 : 1.0;
    normal *= faceSign;
    if (material.textureIndices.z >= 0 && abs(material.extensionScalars.x) > 1e-6)
    {
        float3 tangent = normalize(input.tangent.xyz - normal * dot(normal, input.tangent.xyz)) * faceSign;
        float3 bitangent = normalize(cross(normal, tangent)) * input.tangent.w;
        float3 mappedNormal =
            materialTextures[material.textureIndices.z].Sample(input.texCoord).xyz * 2.0 - 1.0;
        mappedNormal.xy *= material.extensionScalars.x;
        float3x3 tangentFrame = float3x3(tangent, bitangent, normal);
        normal = normalize(mul(mappedNormal, tangentFrame));
    }

    float3 viewDirection = normalize(frame.cameraPosition.xyz - input.worldPosition);
    float3 lightDirection = normalize(-frame.lightDirectionIntensity.xyz);
    float3 halfway = normalize(viewDirection + lightDirection);
    float3 f0 = lerp(float3(0.04), albedo, metallic);
    float3 fresnel = fresnelSchlick(max(dot(halfway, viewDirection), 0.0), f0);
    float nDotV = max(dot(normal, viewDirection), 0.0);
    float nDotL = max(dot(normal, lightDirection), 0.0);
    float geometry = geometrySchlickGGX(nDotV, roughness) *
                     geometrySchlickGGX(nDotL, roughness);
    float3 specular = distributionGGX(normal, halfway, roughness) * geometry * fresnel /
                      max(4.0 * nDotV * nDotL, 0.0001);
    float3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * albedo / PBR_PI;
    float shadow = frame.shadowEnabled != 0
        ? sampleVirtualShadow(shadowMap, input.worldPosition, frame.shadowClipMatrices,
                              shadowPageTable) : 0.0;
    float3 direct = (diffuse + specular) * frame.lightDirectionIntensity.w *
                    frame.lightColor.rgb * nDotL * (1.0 - shadow);
    for (uint lightIndex = 0; lightIndex < frame.localLightCount; ++lightIndex)
    {
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
        float localNDotL = max(dot(normal, localDirection), 0.0);
        float3 localHalfway = normalize(viewDirection + localDirection);
        float3 localFresnel = fresnelSchlick(max(dot(localHalfway, viewDirection), 0.0), f0);
        float localGeometry = geometrySchlickGGX(nDotV, roughness) *
                              geometrySchlickGGX(localNDotL, roughness);
        float3 localSpecular = distributionGGX(normal, localHalfway, roughness) * localGeometry *
                               localFresnel / max(4.0 * nDotV * localNDotL, 0.0001);
        float3 localDiffuse = (1.0 - localFresnel) * (1.0 - metallic) * albedo / PBR_PI;
        direct += (localDiffuse + localSpecular) * local.colorIntensity.rgb *
                  local.colorIntensity.w * localNDotL * attenuation;
    }
    float ao = 1.0;
    if (material.auxiliaryTextureIndices.x >= 0)
        ao = lerp(1.0, materialTextures[material.auxiliaryTextureIndices.x].Sample(input.texCoord).r,
                  clamp(material.roughnessAmbientOcclusion.y, 0.0, 1.0));
    float3 color = 0.9 * albedo * ao + direct;
    // Foliage has a cheap, stable back-lighting term.  The flag is deliberately
    // material data rather than an asset-name heuristic at runtime.
    if ((material.textureIndices.w & 16) != 0 && material.auxiliaryTextureIndices.z >= 0) {
        float mask = materialTextures[material.auxiliaryTextureIndices.z].Sample(input.texCoord).r;
        float backLight = saturate(dot(-normal, lightDirection));
        color += albedo * frame.lightColor.rgb * frame.lightDirectionIntensity.w *
                 backLight * mask * material.extensionScalars.y;
    }

    ForwardFragmentOutput output;
    output.color = float4(color, baseColor.a);
    return output;
}
