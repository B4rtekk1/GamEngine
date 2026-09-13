// Water is intentionally a separate shader module and pipeline family.  It
// currently shares the forward scene/material descriptor contract; future
// water-only inputs (scene colour/depth, SSR, FFT and foam) belong here, not
// in Forward/forward_pbr.slang.
import Common.SceneData;

#ifndef FORWARD_OUTPUT_VELOCITY
#define FORWARD_OUTPUT_VELOCITY 1
#endif

// Bindings 1, 2, 5, 6 and 8 deliberately mirror the forward material
// contract so Water can use the same GPU-driven indirect draw stream today.
[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(5, 0)]] StructuredBuffer<RendererInstanceData> visibleInstances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleInstanceIndices;
#if FORWARD_OUTPUT_VELOCITY
[[vk::binding(8, 0)]] StructuredBuffer<RendererPreviousTransformData> previousTransforms;
#endif

struct GerstnerWave { float2 direction; float amplitude; float wavelength; float speed; float steepness; };
static const GerstnerWave WaterWaves[8] = {
    {{ 1.00,  0.10}, 0.60, 22.0, 1.20, 0.32}, {{ 0.65,  0.76}, 0.18, 5.0, 2.00, 0.25},
    {{-0.34,  0.94}, 0.10, 3.1, 2.60, 0.22}, {{ 0.91, -0.41}, 0.07, 1.8, 3.30, 0.18},
    {{-0.82, -0.57}, 0.03, 0.7, 4.20, 0.12}, {{ 0.18, -0.98}, 0.025, 0.52, 4.80, 0.10},
    {{-0.96,  0.27}, 0.04, 1.1, 3.80, 0.14}, {{ 0.48,  0.88}, 0.12, 7.5, 1.70, 0.20},
};

void evaluateGerstnerWaves(float2 positionXZ, float time, out float3 displacement,
                           out float3 tangent, out float3 binormal) {
    displacement = 0.0; tangent = float3(1.0, 0.0, 0.0); binormal = float3(0.0, 0.0, 1.0);
    [unroll] for (uint index = 0; index < 8; ++index) {
        GerstnerWave wave = WaterWaves[index];
        float2 direction = normalize(wave.direction);
        float k = 6.28318530718 / wave.wavelength;
        float phase = k * dot(direction, positionXZ) + wave.speed * time;
        float sine = sin(phase); float cosine = cos(phase); float horizontal = wave.steepness * wave.amplitude;
        displacement += float3(horizontal * direction.x * cosine, wave.amplitude * sine, horizontal * direction.y * cosine);
        tangent += float3(-horizontal * direction.x * direction.x * k * sine, wave.amplitude * direction.x * k * cosine, -horizontal * direction.x * direction.y * k * sine);
        binormal += float3(-horizontal * direction.x * direction.y * k * sine, wave.amplitude * direction.y * k * cosine, -horizontal * direction.y * direction.y * k * sine);
    }
}

struct WaterVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR0;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(4)]] float2 texCoord1 : TEXCOORD1;
    [[vk::location(3)]] float3 normal : NORMAL;
    [[vk::location(8)]] uint materialIndex : MATERIAL_INDEX;
    [[vk::location(9)]] float4 tangent : TANGENT;
};

struct WaterVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPosition;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] nointerpolation uint materialIndex;
#if FORWARD_OUTPUT_VELOCITY
    [[vk::location(3)]] noperspective float2 currentNdc;
    [[vk::location(4)]] float4 previousClip;
#endif
};

[shader("vertex")]
WaterVertexOutput vertexMain(WaterVertexInput input, uint instanceIndex : SV_InstanceID,
                             uint baseInstance : SV_StartInstanceLocation)
{
    const uint visibleIndex = visibleInstanceIndices[baseInstance + instanceIndex];
    RendererInstanceData instance = visibleInstances[visibleIndex];
    float4 q = instance.rotation;
    float3x3 rotationMatrix = float3x3(
        1.0 - 2.0 * (q.y * q.y + q.z * q.z), 2.0 * (q.x * q.y + q.z * q.w), 2.0 * (q.x * q.z - q.y * q.w),
        2.0 * (q.x * q.y - q.z * q.w), 1.0 - 2.0 * (q.x * q.x + q.z * q.z), 2.0 * (q.y * q.z + q.x * q.w),
        2.0 * (q.x * q.z + q.y * q.w), 2.0 * (q.y * q.z - q.x * q.w), 1.0 - 2.0 * (q.x * q.x + q.y * q.y));
    float3 displacement; float3 tangent; float3 binormal;
    evaluateGerstnerWaves(input.position.xz, frame.windGustFrequencyTime.z, displacement, tangent, binormal);
    float3 worldPosition = mul((input.position + displacement) * instance.scaleBase.xyz, rotationMatrix) + instance.positionMaterial.xyz;
    WaterVertexOutput output;
    float4 currentClip = mul(mul(float4(worldPosition, 1.0), frame.view), frame.projection);
    output.position = currentClip;
    output.worldPosition = worldPosition;
    output.normal = normalize(mul(normalize(cross(binormal, tangent)), rotationMatrix));
    output.materialIndex = asuint(instance.positionMaterial.w) + input.materialIndex;
#if FORWARD_OUTPUT_VELOCITY
    RendererPreviousTransformData previous = previousTransforms[visibleIndex];
    float4 previousQ = previous.previousRotation;
    float3x3 previousRotation = float3x3(
        1.0 - 2.0 * (previousQ.y * previousQ.y + previousQ.z * previousQ.z), 2.0 * (previousQ.x * previousQ.y + previousQ.z * previousQ.w), 2.0 * (previousQ.x * previousQ.z - previousQ.y * previousQ.w),
        2.0 * (previousQ.x * previousQ.y - previousQ.z * previousQ.w), 1.0 - 2.0 * (previousQ.x * previousQ.x + previousQ.z * previousQ.z), 2.0 * (previousQ.y * previousQ.z + previousQ.x * previousQ.w),
        2.0 * (previousQ.x * previousQ.z + previousQ.y * previousQ.w), 2.0 * (previousQ.y * previousQ.z - previousQ.x * previousQ.w), 1.0 - 2.0 * (previousQ.x * previousQ.x + previousQ.y * previousQ.y));
    evaluateGerstnerWaves(input.position.xz, frame.windGustFrequencyTime.w, displacement, tangent, binormal);
    float3 previousWorldPosition = mul((input.position + displacement) * previous.previousScale.xyz, previousRotation) + previous.previousPosition.xyz;
    output.currentNdc = currentClip.xy / currentClip.w;
    output.previousClip = mul(mul(float4(previousWorldPosition, 1.0), frame.previousView), frame.previousProjection);
#endif
    return output;
}

struct WaterFragmentInput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPosition;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] nointerpolation uint materialIndex;
#if FORWARD_OUTPUT_VELOCITY
    [[vk::location(3)]] noperspective float2 currentNdc;
    [[vk::location(4)]] float4 previousClip;
#endif
    bool isFrontFace : SV_IsFrontFace;
};

struct WaterFragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target;
#if FORWARD_OUTPUT_VELOCITY
    [[vk::location(1)]] float2 velocity : SV_Target1;
#endif
};

[shader("fragment")]
WaterFragmentOutput fragmentMain(WaterFragmentInput input)
{
    MaterialData material = materials[input.materialIndex];
    float3 normal = normalize(input.normal);
    if ((material.textureIndices.w & 1) != 0 && !input.isFrontFace)
        normal = -normal;

    float3 viewDirection = normalize(frame.cameraPosition.xyz - input.worldPosition);
    float3 lightDirection = normalize(-frame.lightDirectionIntensity.xyz);
    float nDotL = max(dot(normal, lightDirection), 0.0);
    float fresnel = pow(1.0 - saturate(dot(normal, viewDirection)), 4.0);

    // Compatibility shading only: the dedicated module retains the existing
    // visual until WaterMaterial and its absorption/scattering coefficients
    // are introduced.  Do not add new water resources to the PBR module.
    float3 shallowColor = material.baseColorMetallic.rgb * (0.08 + 0.35 * nDotL);
    float3 color = lerp(shallowColor, float3(0.25, 0.55, 0.80), fresnel) +
                   material.emissiveColorIntensity.rgb * material.emissiveColorIntensity.w;

    WaterFragmentOutput output;
    output.color = float4(color, 1.0);
#if FORWARD_OUTPUT_VELOCITY
    float2 previousNdc = input.previousClip.xy / input.previousClip.w;
    output.velocity = (input.currentNdc - previousNdc) * 0.5;
#endif
    return output;
}
