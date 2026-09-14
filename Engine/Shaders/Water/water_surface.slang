// Water is intentionally a separate shader module and pipeline family.  It
// currently shares the forward scene/material descriptor contract; future
// water-only inputs (scene colour/depth, SSR, FFT and foam) belong here, not
// in Forward/forward_pbr.slang.
import Common.PbrLighting;
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

void evaluateGerstnerWaves(MaterialData material, float2 positionXZ, float time, float cellSize, out float3 displacement,
                           out float3 tangent, out float3 binormal) {
    displacement = 0.0; tangent = float3(1.0, 0.0, 0.0); binormal = float3(0.0, 0.0, 1.0);
    [unroll] for (uint index = 0; index < 8; ++index) {
        if (index >= material.waterWaveCount.x) break;
        float4 wave = material.waterWaves[index];
        // Clipmap vertices cannot represent wavelengths below their sampling
        // rate.  Authored lakes/rivers carry cellSize == 0 and retain their
        // complete wave set.  The smooth transition avoids a hard spectral
        // cutoff while the CPU matches it on shared ring edges.
        float waveLod = cellSize > 0.0 ? smoothstep(2.0, 6.0, wave.w / cellSize) : 1.0;
        if (waveLod <= 0.0) continue;
        float2 direction = normalize(wave.xy);
        float k = 6.28318530718 / max(wave.w, 0.001);
        float phase = k * dot(direction, positionXZ) + material.waterWaveMotion[index].x * time;
        float sine = sin(phase); float cosine = cos(phase); float horizontal = material.waterWaveMotion[index].y * wave.z;
        displacement += waveLod * float3(horizontal * direction.x * cosine, wave.z * sine, horizontal * direction.y * cosine);
        tangent += waveLod * float3(-horizontal * direction.x * direction.x * k * sine, wave.z * direction.x * k * cosine, -horizontal * direction.x * direction.y * k * sine);
        binormal += waveLod * float3(-horizontal * direction.x * direction.y * k * sine, wave.z * direction.y * k * cosine, -horizontal * direction.y * direction.y * k * sine);
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
    const uint materialIndex = asuint(instance.positionMaterial.w) + input.materialIndex;
    MaterialData material = materials[materialIndex];
    // Anchor wave phase in world space.  Ocean clipmap recentering now moves
    // only its sampling mesh, never the surface function itself.
    float3 baseWorldPosition = mul(input.position * instance.scaleBase.xyz, rotationMatrix) + instance.positionMaterial.xyz;
    evaluateGerstnerWaves(material, baseWorldPosition.xz, frame.windGustFrequencyTime.z, input.texCoord1.x, displacement, tangent, binormal);
    float3 worldPosition = baseWorldPosition + mul(displacement * instance.scaleBase.xyz, rotationMatrix);
    WaterVertexOutput output;
    float4 currentClip = mul(mul(float4(worldPosition, 1.0), frame.view), frame.projection);
    output.position = currentClip;
    output.worldPosition = worldPosition;
    output.tangent = normalize(mul(tangent, rotationMatrix));
    output.binormal = normalize(mul(binormal, rotationMatrix));
    output.normal = normalize(cross(output.binormal, output.tangent));
    output.materialIndex = materialIndex;
    output.texCoord = input.texCoord;
#if FORWARD_OUTPUT_VELOCITY
    RendererPreviousTransformData previous = previousTransforms[visibleIndex];
    float4 previousQ = previous.previousRotation;
    float3x3 previousRotation = float3x3(
        1.0 - 2.0 * (previousQ.y * previousQ.y + previousQ.z * previousQ.z), 2.0 * (previousQ.x * previousQ.y + previousQ.z * previousQ.w), 2.0 * (previousQ.x * previousQ.z - previousQ.y * previousQ.w),
        2.0 * (previousQ.x * previousQ.y - previousQ.z * previousQ.w), 1.0 - 2.0 * (previousQ.x * previousQ.x + previousQ.z * previousQ.z), 2.0 * (previousQ.y * previousQ.z + previousQ.x * previousQ.w),
        2.0 * (previousQ.x * previousQ.z + previousQ.y * previousQ.w), 2.0 * (previousQ.y * previousQ.z - previousQ.x * previousQ.w), 1.0 - 2.0 * (previousQ.x * previousQ.x + previousQ.y * previousQ.y));
    float3 previousBaseWorldPosition = mul(input.position * previous.previousScale.xyz, previousRotation) + previous.previousPosition.xyz;
    evaluateGerstnerWaves(material, previousBaseWorldPosition.xz, frame.windGustFrequencyTime.w, input.texCoord1.x, displacement, tangent, binormal);
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

float3 reconstructViewPosition(float depth, float2 uv)
{
    // The inverse jittered projection is the authoritative convention for
    // Vulkan RH_ZO depth and also gives the true oblique ray distance.
    float4 view = mul(float4(uv * 2.0 - 1.0, depth, 1.0), frame.inverseProjection);
    return view.xyz / max(abs(view.w), 1.0e-5);
}

// Schlick's inexpensive approximation of the Henyey-Greenstein phase
// function.  The incident ray travels along -lightDirection, while the
// scattered ray travels towards the camera along viewDirection.
float schlickPhase(float cosTheta, float g)
{
    float k = 1.55 * g - 0.55 * g * g * g;
    float denominator = 1.0 + k * cosTheta;
    return (1.0 - k * k) / max(4.0 * PBR_PI * denominator * denominator, 1.0e-4);
}

// Correlated Smith visibility for a GGX/Trowbridge-Reitz microfacet surface.
// Returned visibility already contains G / (4 * NdotL * NdotV).
float visibilitySmithGGXCorrelated(float nDotV, float nDotL, float roughness)
{
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float lambdaV = nDotL * sqrt(max((nDotV - nDotV * alpha2) * nDotV + alpha2, 0.0));
    float lambdaL = nDotV * sqrt(max((nDotL - nDotL * alpha2) * nDotL + alpha2, 0.0));
    return 0.5 / max(lambdaV + lambdaL, 1.0e-4);
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
        // Ocean detail must be continuous in world space: per-quad UVs make
        // normal-map frequency and phase change at every clipmap ring.
        const float normalMapScale = 0.15;
        float2 waterUv = input.worldPosition.xz * normalMapScale +
            frame.windDirectionStrength.xz * frame.windGustFrequencyTime.z * 0.04;
        float3 microTS = materialTextures[NonUniformResourceIndex(material.waterTextureIndices.x)]
            .Sample(waterUv).xyz * 2.0 - 1.0;
        float3 microWS = normalize(input.tangent * microTS.x + input.binormal * microTS.y + normal * microTS.z);
        normal = normalize(lerp(normal, microWS, saturate(material.waterFoam.z)));
    }

    float3 viewDirection = normalize(frame.cameraPosition.xyz - input.worldPosition);
    float3 lightDirection = normalize(-frame.lightDirectionIntensity.xyz);
    float nDotL = saturate(dot(normal, lightDirection));
    float nDotV = saturate(dot(normal, viewDirection));
    float ior = max(material.waterDeepColorIor.w, 1.001);
    float f0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    float3 surfaceF0 = float3(f0);
    float3 reflectionFresnel = fresnelSchlick(nDotV, surfaceF0);

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
    float thickness = opaqueDepth >= 0.99999 ? material.waterScatteringMaxDepth.w :
        length(reconstructViewPosition(opaqueDepth, distortedUv) - reconstructViewPosition(waterDepth, screenUv));
    thickness = min(thickness, material.waterScatteringMaxDepth.w);
    // Water is a homogeneous participating medium.  Both absorption and
    // out-scattering remove radiance from the refracted background.
    float3 sigmaA = max(material.waterAbsorptionRefraction.rgb, 0.0);
    float3 sigmaS = max(material.waterScatteringMaxDepth.rgb, 0.0);
    float3 extinction = max(sigmaA + sigmaS, float3(1.0e-4));
    float3 transmittance = exp(-extinction * thickness);
    float3 refracted = opaqueSceneColor.SampleLevel(distortedUv, 0.0).rgb;
    // Retain physical absorption/scattering while making the artist-facing
    // shallow/deep colours an explicit tint of in-scattered light.
    float3 scatteringTint = lerp(material.waterShallowColorRoughness.rgb,
                                 material.waterDeepColorIor.rgb,
                                 saturate(thickness / material.waterScatteringMaxDepth.w));
    // g > 0 favours forward scattering, making a submerged view towards the
    // sun visibly brighter without introducing a separate artist coefficient.
    const float phaseG = 0.2;
    float phase = schlickPhase(dot(-lightDirection, viewDirection), phaseG);
    float3 scatterRadiance = scatteringTint * frame.lightColor.rgb *
        frame.lightDirectionIntensity.w * phase;
    float3 transmission = refracted * transmittance +
        (sigmaS / extinction) * (1.0 - transmittance) * scatterRadiance;

    uint reflectionWidth; uint reflectionHeight; uint reflectionMips;
    prefilteredEnvironmentMap.GetDimensions(0, reflectionWidth, reflectionHeight, reflectionMips);
    float3 reflected = prefilteredEnvironmentMap.SampleLevel(reflect(-viewDirection, normal),
        material.waterShallowColorRoughness.w * float(max(reflectionMips, 1u) - 1u)).rgb;
    // Dielectric GGX/Trowbridge-Reitz BRDF.  It responds to the actual local
    // wave and normal-map slopes, yielding distributed sun glitter rather
    // than a single exponent-shaped highlight.
    float3 halfVector = normalize(lightDirection + viewDirection);
    float roughness = clamp(material.waterShallowColorRoughness.w, 0.045, 1.0);
    float3 sunFresnel = fresnelSchlick(saturate(dot(halfVector, viewDirection)), surfaceF0);
    float3 sunSpecular = distributionGGX(normal, halfVector, roughness) *
        visibilitySmithGGXCorrelated(nDotV, nDotL, roughness) * sunFresnel;
    float3 color = lerp(transmission, reflected, reflectionFresnel) +
        frame.lightColor.rgb * frame.lightDirectionIntensity.w * sunSpecular * nDotL;

    WaterFragmentOutput output;
    output.color = float4(color, 1.0);
#if FORWARD_OUTPUT_VELOCITY
    float2 previousNdc = input.previousClip.xy / input.previousClip.w;
    output.velocity = (input.currentNdc - previousNdc) * 0.5;
#endif
    return output;
}
