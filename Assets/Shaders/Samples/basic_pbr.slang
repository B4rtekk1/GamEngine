import Common.PbrLighting;
import Common.ShadowSampling;

struct FrameData
{
    float4x4 view;
    float4x4 projection;
    float4x4 lightSpace;
    float4 cameraPosition;
    float4 lightDirectionIntensity;
};

struct PushConstants
{
    float4x4 model;
    float4 baseColorMetallic;
    float4 roughnessAo;
};

[[vk::binding(1, 0)]] ConstantBuffer<FrameData> frame;
[[vk::push_constant]] ConstantBuffer<PushConstants> pushConstants;

struct BasicPbrVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR0;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(3)]] float3 normal : NORMAL;
};

struct BasicPbrVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 color;
    [[vk::location(1)]] float4 lightSpacePosition;
    [[vk::location(2)]] float3 worldPosition;
    [[vk::location(3)]] float3 normal;
};

float3x3 inverseTranspose(float3x3 matrix)
{
    float3 cofactor0 = cross(matrix[1], matrix[2]);
    float3 cofactor1 = cross(matrix[2], matrix[0]);
    float3 cofactor2 = cross(matrix[0], matrix[1]);
    float determinant = dot(matrix[0], cofactor0);
    return float3x3(cofactor0, cofactor1, cofactor2) / determinant;
}

[shader("vertex")]
BasicPbrVertexOutput vertexMain(BasicPbrVertexInput input)
{
    float4 worldPosition = mul(float4(input.position, 1.0), pushConstants.model);
    float3x3 normalMatrix = inverseTranspose((float3x3)pushConstants.model);

    BasicPbrVertexOutput output;
    output.position = mul(mul(worldPosition, frame.view), frame.projection);
    output.color = input.color;
    output.worldPosition = worldPosition.xyz;
    output.normal = mul(input.normal, normalMatrix);
    output.lightSpacePosition = mul(worldPosition, frame.lightSpace);
    return output;
}

[[vk::binding(0, 0)]] Sampler2D shadowMap;

struct BasicPbrFragmentInput
{
    [[vk::location(0)]] float3 color;
    [[vk::location(1)]] float4 lightSpacePosition;
    [[vk::location(2)]] float3 worldPosition;
    [[vk::location(3)]] float3 normal;
};

struct BasicPbrFragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target;
};

[shader("fragment")]
BasicPbrFragmentOutput fragmentMain(BasicPbrFragmentInput input)
{
    float3 albedo = pow(input.color * pushConstants.baseColorMetallic.rgb, float3(2.2));
    float metallic = clamp(pushConstants.baseColorMetallic.a, 0.0, 1.0);
    float roughness = clamp(pushConstants.roughnessAo.x, 0.045, 1.0);
    float ao = clamp(pushConstants.roughnessAo.y, 0.0, 1.0);
    float3 normal = normalize(input.normal);
    float3 viewDirection = normalize(frame.cameraPosition.xyz - input.worldPosition);
    float3 lightDirection = normalize(-frame.lightDirectionIntensity.xyz);
    float3 halfway = normalize(viewDirection + lightDirection);
    float3 f0 = lerp(float3(0.04), albedo, metallic);
    float3 fresnel = fresnelSchlick(max(dot(halfway, viewDirection), 0.0), f0);
    float ndf = distributionGGX(normal, halfway, roughness);
    float geometry = geometrySmith(normal, viewDirection, lightDirection, roughness);
    float3 specular = (ndf * geometry * fresnel) /
        max(4.0 * max(dot(normal, viewDirection), 0.0) *
            max(dot(normal, lightDirection), 0.0), 0.0001);
    float3 diffuseFactor = (1.0 - fresnel) * (1.0 - metallic);
    float nDotL = max(dot(normal, lightDirection), 0.0);
    float3 radiance = frame.lightDirectionIntensity.w;
    float shadow = samplePcfShadow(shadowMap, input.lightSpacePosition);
    float3 direct = (diffuseFactor * albedo / PBR_PI + specular) *
                    radiance * nDotL * (1.0 - shadow);
    float3 ambient = 0.035 * albedo * ao;
    float3 color = ambient + direct;

    BasicPbrFragmentOutput output;
    output.color = float4(pow(color / (color + 1.0), float3(1.0 / 2.2)), 1.0);
    return output;
}
