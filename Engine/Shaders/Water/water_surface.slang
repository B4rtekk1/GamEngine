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
[[vk::binding(11, 0)]] SamplerCube prefilteredEnvironmentMap;
[[vk::binding(13, 0)]] Sampler2D materialTextures[];
// Immutable copies made after opaque lighting.  These belong to Water's own
// descriptor set: updating them can therefore never mutate a forward set that
// is already referenced by this command buffer.
[[vk::binding(0, 1)]] Sampler2D opaqueSceneColor;
[[vk::binding(1, 1)]] Sampler2D opaqueSceneDepth;
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
    [[vk::location(6)]] float3 tangent;
    [[vk::location(7)]] float3 binormal;
    [[vk::location(2)]] nointerpolation uint materialIndex;
    [[vk::location(5)]] float2 texCoord;
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
    // Anchor wave phase in world space.  Ocean clipmap recentering now moves
    // only its sampling mesh, never the surface function itself.
    float3 baseWorldPosition = mul(input.position * instance.scaleBase.xyz, rotationMatrix) + instance.positionMaterial.xyz;
    evaluateGerstnerWaves(baseWorldPosition.xz, frame.windGustFrequencyTime.z, displacement, tangent, binormal);
    float3 worldPosition = baseWorldPosition + mul(displacement * instance.scaleBase.xyz, rotationMatrix);
    WaterVertexOutput output;
    float4 currentClip = mul(mul(float4(worldPosition, 1.0), frame.view), frame.projection);
    output.position = currentClip;
    output.worldPosition = worldPosition;
    output.tangent = normalize(mul(tangent, rotationMatrix));
    output.binormal = normalize(mul(binormal, rotationMatrix));
    output.normal = normalize(cross(output.binormal, output.tangent));
    output.materialIndex = asuint(instance.positionMaterial.w) + input.materialIndex;
    output.texCoord = input.texCoord;
#if FORWARD_OUTPUT_VELOCITY
    RendererPreviousTransformData previous = previousTransforms[visibleIndex];
    float4 previousQ = previous.previousRotation;
    float3x3 previousRotation = float3x3(
        1.0 - 2.0 * (previousQ.y * previousQ.y + previousQ.z * previousQ.z), 2.0 * (previousQ.x * previousQ.y + previousQ.z * previousQ.w), 2.0 * (previousQ.x * previousQ.z - previousQ.y * previousQ.w),
        2.0 * (previousQ.x * previousQ.y - previousQ.z * previousQ.w), 1.0 - 2.0 * (previousQ.x * previousQ.x + previousQ.z * previousQ.z), 2.0 * (previousQ.y * previousQ.z + previousQ.x * previousQ.w),
        2.0 * (previousQ.x * previousQ.z + previousQ.y * previousQ.w), 2.0 * (previousQ.y * previousQ.z - previousQ.x * previousQ.w), 1.0 - 2.0 * (previousQ.x * previousQ.x + previousQ.y * previousQ.y));
    float3 previousBaseWorldPosition = mul(input.position * previous.previousScale.xyz, previousRotation) + previous.previousPosition.xyz;
    evaluateGerstnerWaves(previousBaseWorldPosition.xz, frame.windGustFrequencyTime.w, displacement, tangent, binormal);
    float3 previousWorldPosition = previousBaseWorldPosition + mul(displacement * previous.previousScale.xyz, previousRotation);
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
    [[vk::location(6)]] float3 tangent;
    [[vk::location(7)]] float3 binormal;
    [[vk::location(2)]] nointerpolation uint materialIndex;
    [[vk::location(5)]] float2 texCoord;
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

float linearViewDepth(float depth)
{
    // Row-vector projection used by the renderer: clip.z = z*m22 + m32 and
    // clip.w = z. This avoids expanding the shared per-frame UBO.
    return abs(frame.projection[3][2] / max(abs(depth - frame.projection[2][2]), 1.0e-5));
}

[shader("fragment")]
WaterFragmentOutput fragmentMain(WaterFragmentInput input)
{
    MaterialData material = materials[input.materialIndex];
    float3 normal = normalize(input.normal);
    if ((material.textureIndices.w & 1) != 0 && !input.isFrontFace)
        normal = -normal;

    // Macro normal follows the displaced Gerstner geometry. A material-owned
    // normal map is strictly micro detail, never the water's main shape.
    if (material.waterTextureIndices.x >= 0) {
        float3 microTS = materialTextures[NonUniformResourceIndex(material.waterTextureIndices.x)]
            .Sample(input.texCoord * 6.0 + frame.windDirectionStrength.xz * frame.windGustFrequencyTime.z * 0.04).xyz * 2.0 - 1.0;
        float3 microWS = normalize(input.tangent * microTS.x + input.binormal * microTS.y + normal * microTS.z);
        normal = normalize(lerp(normal, microWS, saturate(material.waterFoam.z)));
    }

    float3 viewDirection = normalize(frame.cameraPosition.xyz - input.worldPosition);
    float3 lightDirection = normalize(-frame.lightDirectionIntensity.xyz);
    float nDotL = max(dot(normal, lightDirection), 0.0);
    float nDotV = saturate(dot(normal, viewDirection));
    float ior = max(material.waterDeepColorIor.w, 1.001);
    float f0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    float fresnel = f0 + (1.0 - f0) * pow(1.0 - nDotV, 5.0);

    float2 screenUv = input.position.xy / frame.clusteredViewportNearFar.xy;
    float3 normalVS = normalize(mul(normal, (float3x3)frame.view));
    float2 distortedUv = clamp(screenUv + normalVS.xy * material.waterAbsorptionRefraction.w,
                               float2(0.001, 0.001), float2(0.999, 0.999));
    float opaqueDepth = opaqueSceneDepth.SampleLevel(distortedUv, 0.0).r;
    const float waterDepth = input.position.z;
    // Never refract foreground geometry into the water surface.  Falling back
    // to the undistorted scene sample is stable at intersections.
    if (opaqueDepth <= waterDepth) {
        distortedUv = screenUv;
        opaqueDepth = opaqueSceneDepth.SampleLevel(distortedUv, 0.0).r;
    }
    float thickness = opaqueDepth >= 0.99999
        ? material.waterScatteringMaxDepth.w
        : max(0.0, linearViewDepth(opaqueDepth) - linearViewDepth(waterDepth));
    thickness = min(thickness, material.waterScatteringMaxDepth.w);
    float3 transmittance = exp(-material.waterAbsorptionRefraction.rgb * thickness);
    float3 refracted = opaqueSceneColor.SampleLevel(distortedUv, 0.0).rgb;
    float3 transmission = refracted * transmittance +
        material.waterScatteringMaxDepth.rgb * (1.0 - transmittance);

    uint reflectionWidth; uint reflectionHeight; uint reflectionMips;
    prefilteredEnvironmentMap.GetDimensions(0, reflectionWidth, reflectionHeight, reflectionMips);
    float3 reflected = prefilteredEnvironmentMap.SampleLevel(reflect(-viewDirection, normal),
        material.waterShallowColorRoughness.w * float(max(reflectionMips, 1u) - 1u)).rgb;
    // A compact GGX-like sun lobe retains the crucial sun glitter even before
    // the specialised ocean BRDF arrives in the FFT stage.
    float3 halfVector = normalize(lightDirection + viewDirection);
    float sunSpecular = pow(saturate(dot(normal, halfVector)),
        lerp(2048.0, 24.0, material.waterShallowColorRoughness.w)) * nDotL;
    float3 color = lerp(transmission, reflected, fresnel) +
        frame.lightColor.rgb * frame.lightDirectionIntensity.w * sunSpecular * fresnel;

    WaterFragmentOutput output;
    output.color = float4(color, 1.0);
#if FORWARD_OUTPUT_VELOCITY
    float2 previousNdc = input.previousClip.xy / input.previousClip.w;
    output.velocity = (input.currentNdc - previousNdc) * 0.5;
#endif
    return output;
}
