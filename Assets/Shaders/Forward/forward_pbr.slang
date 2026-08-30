import Common.PbrLighting;
import Common.SceneData;
import Common.ShadowSampling;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;

struct ForwardVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR0;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(3)]] float3 normal : NORMAL;
    [[vk::location(4)]] float4 positionMaterial : INSTANCE_POSITION_MATERIAL;
    [[vk::location(5)]] float4 instanceRotation : INSTANCE_ROTATION;
    [[vk::location(6)]] float4 scaleBase : INSTANCE_SCALE_BASE;
    [[vk::location(8)]] uint materialIndex : MATERIAL_INDEX;
    [[vk::location(9)]] float4 tangent : TANGENT;
    [[vk::location(13)]] float4 grassDeformation : GRASS_DEFORMATION;
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
ForwardVertexOutput vertexMain(ForwardVertexInput input, uint instanceIndex : SV_InstanceID)
{
    float3 localPosition = input.position;
    float3 localNormal = input.normal;
    float normalizedHeight = clamp((input.position.y - input.scaleBase.w) *
                                   input.grassDeformation.w, 0.0, 1.0);
    float bendWeight = normalizedHeight * normalizedHeight * input.grassDeformation.z;
    float grassHeight = input.grassDeformation.w > 0.0 ? 1.0 / input.grassDeformation.w : 0.0;
    localPosition.xz += input.grassDeformation.xy * bendWeight * grassHeight * 0.8;
    localPosition.y -= bendWeight * normalizedHeight * grassHeight * 0.55;
    localNormal = normalize(localNormal + float3(-input.grassDeformation.x * bendWeight,
                                                0.0,
                                               -input.grassDeformation.y * bendWeight));

    float4 q = input.instanceRotation;
    float3x3 rotationMatrix = float3x3(
        1.0 - 2.0 * (q.y * q.y + q.z * q.z), 2.0 * (q.x * q.y + q.z * q.w), 2.0 * (q.x * q.z - q.y * q.w),
        2.0 * (q.x * q.y - q.z * q.w), 1.0 - 2.0 * (q.x * q.x + q.z * q.z), 2.0 * (q.y * q.z + q.x * q.w),
        2.0 * (q.x * q.z + q.y * q.w), 2.0 * (q.y * q.z - q.x * q.w), 1.0 - 2.0 * (q.x * q.x + q.y * q.y));
    float3 worldPosition3 = mul(localPosition * input.scaleBase.xyz, rotationMatrix) + input.positionMaterial.xyz;
    float4 worldPosition = float4(worldPosition3, 1.0);
    float3x3 normalMatrix = float3x3(rotationMatrix[0] / input.scaleBase.x,
                                     rotationMatrix[1] / input.scaleBase.y,
                                     rotationMatrix[2] / input.scaleBase.z);

    ForwardVertexOutput output;
    output.position = mul(mul(worldPosition, frame.view), frame.projection);
    output.color = input.color;
    output.worldPosition = worldPosition.xyz;
    output.normal = mul(localNormal, normalMatrix);
    output.materialIndex = asuint(input.positionMaterial.w) + input.materialIndex;
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
    if (material.textureIndices.x >= 0)
        baseColor *= materialTextures[material.textureIndices.x].Sample(input.texCoord);

    const bool doubleSided = (material.textureIndices.w & 1) != 0;
    const bool alphaBlend = (material.textureIndices.w & 2) != 0;
    if (alphaBlend)
    {
        if (baseColor.a < material.roughnessAmbientOcclusion.z)
            discard;
        baseColor.a = 1.0;
    }
    else if (baseColor.a <= 0.01)
    {
        discard;
    }

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
    if (doubleSided && !input.isFrontFace)
        normal = -normal;
    if (material.textureIndices.z >= 0 && abs(material.roughnessAmbientOcclusion.w) > 1e-6)
    {
        float3 tangent = normalize(input.tangent.xyz - normal * dot(normal, input.tangent.xyz));
        float3 bitangent = normalize(cross(normal, tangent)) * input.tangent.w;
        float3 mappedNormal =
            materialTextures[material.textureIndices.z].Sample(input.texCoord).xyz * 2.0 - 1.0;
        mappedNormal.xy *= material.roughnessAmbientOcclusion.w;
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
    float3 color = 0.9 * albedo *
                   clamp(material.roughnessAmbientOcclusion.y, 0.0, 1.0) + direct;

    ForwardFragmentOutput output;
    output.color = float4(color, baseColor.a);
    return output;
}
