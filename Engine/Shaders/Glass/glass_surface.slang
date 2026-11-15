// Transmissive glass for GlassPass.
//
// Each fragment composes its final colour over the lit opaque scene:
//  - reflection: a hierarchical (Hi-Z) screen-space trace against the
//    closest-surface depth pyramid, resolved over local probes and the sky;
//  - transmission: the scene behind, refracted through a slab of glass
//    (entry through the shading normal, exit through the geometric one),
//    Hi-Z traced when the refracted ray bends away from the view ray;
//    frosted by roughness through the snapshot's mip chain;
//  - Beer-Lambert absorption along the path inside the glass, base colour
//    tint, two-interface (slab) Fresnel, direct sun/local specular, a
//    diffuse share for partly transmissive glass and an optional dirt layer.
import Common.PbrLighting;
import Common.VertexFrame;
import Common.ReflectionFallback;
import Common.ReflectionProbes;
import Common.SceneData;
import Common.ShadowSampling;
import Common.Displacement;
import Common.HiZTrace;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(5, 0)]] StructuredBuffer<RendererInstanceData> visibleInstances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleInstanceIndices;
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
[[vk::binding(24, 0)]] StructuredBuffer<LocalLightData> localLights;

// GlassPass inputs: the lit scene behind the glass (mip-mapped), its
// closest-surface hardware-depth pyramid and the hardware depth itself. The
// pyramid's level 0 is never written: level 0 lookups read opaqueDepth.
[[vk::binding(0, 1)]] Sampler2D sceneColor;
[[vk::binding(1, 1)]] Sampler2D closestDepth;
[[vk::binding(2, 1)]] Sampler2D opaqueDepth;

static const uint SCREEN_SPACE_EFFECTS_TAA = 4u;
// Hi-Z traversal budget. Each step climbs or descends the depth pyramid, so a
// ray crosses the screen in far fewer steps than a linear march; 32 covers
// mirror-like rays, rough ones get half. The trace also stops early when it
// leaves the screen or finds a hit.
static const uint GLASS_HIZ_ITERATIONS = 32u;
static const uint GLASS_HIZ_ROUGH_ITERATIONS = 16u;
static const int GLASS_HIZ_MAX_MIP = 7;
// Above this roughness SSR adds nothing a prefiltered probe does not.
static const float GLASS_SSR_MAX_ROUGHNESS = 0.75;
// Smallest geometric thickness assumed for screen-space surfaces, and the
// share of view depth added to it.
static const float GLASS_DEPTH_THICKNESS = 0.12;
static const float GLASS_DEPTH_THICKNESS_SCALE = 0.03;

struct GlassVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR0;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(4)]] float2 texCoord1 : TEXCOORD1;
    [[vk::location(3)]] uint tangentFrame : NORMAL; // GpuVertex::packTangentFrame
    property float3 normal { get { return decodeVertexNormal(tangentFrame); } }
    [[vk::location(8)]] uint materialIndex : MATERIAL_INDEX;
    property float4 tangent { get { return decodeVertexTangent(tangentFrame, decodeVertexNormal(tangentFrame)); } }
};

struct GlassVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 color;
    [[vk::location(1)]] float3 worldPosition;
    [[vk::location(2)]] float3 normal;
    [[vk::location(3)]] nointerpolation uint materialIndex;
    [[vk::location(4)]] float2 texCoord;
    [[vk::location(5)]] float4 tangent;
    [[vk::location(6)]] float2 texCoord1;
};

[shader("vertex")]
GlassVertexOutput vertexMain(GlassVertexInput input, uint instanceIndex : SV_InstanceID,
                             uint baseInstance : SV_StartInstanceLocation)
{
    const uint visibleIndex = visibleInstanceIndices[baseInstance + instanceIndex];
    RendererInstanceData instance = visibleInstances[visibleIndex];
    const uint materialIndex = asuint(instance.positionMaterial.w) + input.materialIndex;
    MaterialData material = materials[materialIndex];
    const DisplacedVertex displaced = displaceMaterialVertex(material, input.position, input.normal, input.tangent,
                                                             input.texCoord, input.texCoord1);

    const float4 q = instance.rotation;
    const float3x3 rotationMatrix = float3x3(
        1.0 - 2.0 * (q.y * q.y + q.z * q.z), 2.0 * (q.x * q.y + q.z * q.w), 2.0 * (q.x * q.z - q.y * q.w),
        2.0 * (q.x * q.y - q.z * q.w), 1.0 - 2.0 * (q.x * q.x + q.z * q.z), 2.0 * (q.y * q.z + q.x * q.w),
        2.0 * (q.x * q.z + q.y * q.w), 2.0 * (q.y * q.z - q.x * q.w), 1.0 - 2.0 * (q.x * q.x + q.y * q.y));
    const float3 worldPosition = mul(displaced.position * instance.scaleBase.xyz, rotationMatrix) +
                                 instance.positionMaterial.xyz;
    const float3x3 normalMatrix = float3x3(rotationMatrix[0] / instance.scaleBase.x,
                                           rotationMatrix[1] / instance.scaleBase.y,
                                           rotationMatrix[2] / instance.scaleBase.z);

    GlassVertexOutput output;
    output.position = mul(mul(float4(worldPosition, 1.0), frame.view), frame.projection);
    output.color = input.color;
    output.worldPosition = worldPosition;
    output.normal = mul(displaced.normal, normalMatrix);
    output.materialIndex = materialIndex;
    output.texCoord = input.texCoord;
    output.texCoord1 = input.texCoord1;
    output.tangent = float4(mul(displaced.tangent.xyz * instance.scaleBase.xyz, rotationMatrix),
                            displaced.tangent.w);
    return output;
}

struct GlassFragmentInput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 color;
    [[vk::location(1)]] float3 worldPosition;
    [[vk::location(2)]] float3 normal;
    [[vk::location(3)]] nointerpolation uint materialIndex;
    [[vk::location(4)]] float2 texCoord;
    [[vk::location(5)]] float4 tangent;
    [[vk::location(6)]] float2 texCoord1;
    bool isFrontFace : SV_IsFrontFace;
};

struct GlassFragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target;
};

// ---------------------------------------------------------------------------
// Screen space

float2 screenSize()
{
    uint width, height, mips;
    closestDepth.GetDimensions(0, width, height, mips);
    return float2(width, height);
}

int pyramidMipCount()
{
    uint width, height, mips;
    closestDepth.GetDimensions(0, width, height, mips);
    return int(mips);
}

// mipSize: the level's extent (mipResolution), which callers already know;
// a size query per lookup would cost a texture instruction of its own.
float loadClosestDepth(int2 texel, int mip, float2 mipSize)
{
    return hizLoadClosestDepth(opaqueDepth, closestDepth, texel, mip, mipSize);
}

// Exact opaque depth (pyramid level 0).
float loadOpaqueDepth(int2 texel)
{
    uint width, height, mips;
    opaqueDepth.GetDimensions(0, width, height, mips);
    return opaqueDepth.Load(int3(clamp(texel, int2(0, 0), int2(int(width), int(height)) - 1), 0)).r;
}

float sceneDepthAt(float2 uv)
{
    return loadOpaqueDepth(int2(uv * screenSize()));
}

float3 viewFromScreen(float2 uv, float depth)
{
    return hizViewFromScreen(uv, depth, frame.inverseProjection);
}

float3 screenFromView(float3 view)
{
    return hizScreenFromView(view, frame.projection);
}

float3 viewPosition(float3 world)
{
    return mul(float4(world, 1.0), frame.view).xyz;
}

float3 viewDirection(float3 world)
{
    return normalize(mul(float4(world, 0.0), frame.view).xyz);
}

// Mip extents follow HiZBuffer: floor(size / 2^mip), at least one texel.
float2 mipResolution(float2 size, int mip)
{
    return hizMipResolution(size, mip);
}

// Projects a view-space ray to screen space (uv, hardware depth). Perspective
// keeps lines straight, so any end point in front of the camera gives the
// same screen-space line; pull it towards the camera at most half way.
bool screenSpaceRay(float3 originView, float3 directionView, out float3 originScreen, out float3 directionScreen)
{
    return hizScreenSpaceRay(originView, directionView, frame.projection, originScreen, directionScreen);
}

// Hierarchical traversal of the closest-depth pyramid (Uludag, GPU Pro 5;
// AMD FidelityFX SSSR). The ray walks cell boundaries: while it stays in
// front of a cell's closest surface it climbs to a coarser level and skips
// the whole cell, otherwise it descends until it meets the surface at mip 0.
// Hardware depth is linear in screen space, so the ray is a straight line
// in (uv, depth) and the surface test is a plane intersection.
bool traceHiZ(float3 origin, float3 direction, uint maxIterations, out float3 hit)
{
    return hizTrace(opaqueDepth, closestDepth, screenSize(), min(pyramidMipCount() - 1, GLASS_HIZ_MAX_MIP), origin,
                    direction, maxIterations, hit);
}

// Accepts a hit only where the ray actually meets the depth surface (not a
// thin object it passed behind) and fades it out at the screen border.
float hitConfidence(float3 hit, float3 originScreen)
{
    return hizHitConfidence(opaqueDepth, screenSize(), frame.inverseProjection, hit, originScreen,
                            GLASS_DEPTH_THICKNESS, GLASS_DEPTH_THICKNESS_SCALE);
}

// Samples the scene snapshot with a footprint of `radius` pixels: a rotated
// four-tap pattern at the matching mip hides the box-filtered mip chain.
float3 sampleSceneBlurred(float2 uv, float radius, float rotation)
{
    return hizSampleBlurred(sceneColor, screenSize(), uv, radius, rotation);
}

// ---------------------------------------------------------------------------
// Optics

// Unpolarised Fresnel reflectance of a smooth air/glass interface.
float dielectricFresnel(float cosine, float ior)
{
    if (ior <= 1.00001)
        return 0.0;
    cosine = saturate(cosine);
    const float sine2 = (1.0 - cosine * cosine) / (ior * ior);
    if (sine2 >= 1.0)
        return 1.0;
    const float cosineT = sqrt(1.0 - sine2);
    const float s = (cosine - ior * cosineT) / (cosine + ior * cosineT);
    const float p = (ior * cosine - cosineT) / (ior * cosine + cosineT);
    return 0.5 * (s * s + p * p);
}

float3 safeRefract(float3 incident, float3 normal, float eta, float3 fallback)
{
    const float3 refracted = refract(incident, normal, eta);
    return dot(refracted, refracted) > 1.0e-8 ? normalize(refracted) : fallback;
}

float interleavedGradientNoise(float2 pixel, uint frameIndex)
{
    return hizInterleavedGradientNoise(pixel, frameIndex);
}

// GGX visible-normal sample (Heitz 2018) in the frame (tangent, bitangent, normal).
float3 sampleGgxVisibleNormal(float3 viewTangent, float alpha, float2 u)
{
    return hizSampleGgxVisibleNormal(viewTangent, alpha, u);
}

float2 glassTextureUv(MaterialData material, uint slot, float2 uv0, float2 uv1)
{
    const int encoded = material.glassTextureCoordinateSets[slot];
    const float2 uv = (encoded & 1) != 0 ? uv1 : uv0;
    if ((encoded & 2) != 0)
        return uv;
    return float2(dot(uv, material.glassTextureTransforms[slot].xy), dot(uv, material.glassTextureRows1[slot].xy)) +
           material.glassTextureTransforms[slot].zw;
}

// ---------------------------------------------------------------------------

[shader("fragment")]
GlassFragmentOutput fragmentMain(GlassFragmentInput input)
{
    const int2 pixel = int2(input.position.xy);
    const float2 size = screenSize();
    const float2 pixelUv = input.position.xy / size;
    // Opaque occlusion: the scene depth is not this pass's depth attachment
    // (that one only orders glass against glass).
    const float opaqueDepthHere = loadOpaqueDepth(pixel);
    if (input.position.z > opaqueDepthHere)
        discard;

    MaterialData material = materials[input.materialIndex];
    const uint flags = uint(material.textureIndices.w);
    const float3 vertexColor = (flags & 32u) != 0u || input.color.r < 0.0 ? float3(1.0, 1.0, 1.0) : input.color;
    float4 baseColor = float4(material.baseColorMetallic.rgb * vertexColor, material.roughnessAmbientOcclusion.w);
    if (material.textureIndices.x >= 0)
        baseColor *= materialTextures[NonUniformResourceIndex(material.textureIndices.x)]
                         .Sample(materialTextureUv(material, 0, input.texCoord, input.texCoord1));
    if (material.auxiliaryTextureIndices.y >= 0)
        baseColor.a *= materialTextures[NonUniformResourceIndex(material.auxiliaryTextureIndices.y)]
                           .Sample(materialTextureUv(material, 4, input.texCoord, input.texCoord1)).r;
    const uint alphaMode = (flags >> 1u) & 3u;
    if (alphaMode == 1u && baseColor.a < material.roughnessAmbientOcclusion.z)
        discard;
    // Blend: alpha is the coverage of the glass over the unrefracted scene.
    const float coverage = alphaMode == 2u ? saturate(baseColor.a) : 1.0;

    float roughness = material.roughnessAmbientOcclusion.x;
    float metallic = material.baseColorMetallic.a;
    if (material.textureIndices.y >= 0)
    {
        const float4 metallicRoughness = materialTextures[NonUniformResourceIndex(material.textureIndices.y)]
                                             .Sample(materialTextureUv(material, 1, input.texCoord, input.texCoord1));
        metallic *= metallicRoughness.b;
        roughness *= metallicRoughness.g;
    }
    metallic = saturate(metallic);

    float transmission = material.glassParams.x;
    if (material.glassTextureIndices.x >= 0)
        transmission *= materialTextures[NonUniformResourceIndex(material.glassTextureIndices.x)]
                            .Sample(glassTextureUv(material, 0, input.texCoord, input.texCoord1)).r;
    float thickness = material.glassParams.z;
    if (material.glassTextureIndices.y >= 0)
        thickness *= materialTextures[NonUniformResourceIndex(material.glassTextureIndices.y)]
                         .Sample(glassTextureUv(material, 1, input.texCoord, input.texCoord1)).g;
    float dirt = material.glassParams.w;
    float3 dirtAlbedo = float3(0.36, 0.34, 0.31);
    if (material.glassTextureIndices.z >= 0)
    {
        const float4 dirtSample = materialTextures[NonUniformResourceIndex(material.glassTextureIndices.z)]
                                      .Sample(glassTextureUv(material, 2, input.texCoord, input.texCoord1));
        dirt *= dirtSample.r;
        dirtAlbedo = dirtSample.rgb * 0.8;
    }
    const float ior = max(material.glassParams.y, 1.0);
    transmission *= 1.0 - metallic;

    // Normals. Glass seen from behind (two-sided stream) shades its back face.
    const float faceSign = input.isFrontFace ? 1.0 : -1.0;
    const float3 geometricNormal = normalize(input.normal) * faceSign;
    float3 normal = geometricNormal;
    if (material.textureIndices.z >= 0 && abs(material.extensionScalars.x) > 1.0e-6)
    {
        const float3 tangent = normalize(input.tangent.xyz - normal * dot(normal, input.tangent.xyz)) * faceSign;
        const float3 bitangent = normalize(cross(normal, tangent)) * input.tangent.w;
        float2 mapped = materialTextures[NonUniformResourceIndex(material.textureIndices.z)]
                            .Sample(materialTextureUv(material, 2, input.texCoord, input.texCoord1)).rg * 2.0 - 1.0;
        if ((flags & 128u) != 0u)
            mapped.y = -mapped.y;
        const float mappedZ = sqrt(saturate(1.0 - dot(mapped, mapped)));
        normal = normalize(mul(float3(mapped * material.extensionScalars.x, mappedZ),
                               float3x3(tangent, bitangent, normal)));
    }
    const float3 toCamera = normalize(frame.cameraPosition.xyz - input.worldPosition);
    // Keep the shading normal on the camera side (silhouettes, bent maps).
    if (dot(normal, toCamera) < 1.0e-3)
        normal = normalize(normal + geometricNormal * (1.0e-3 - dot(normal, toCamera) + 0.05));
    // Specular anti-aliasing from screen-space normal variance.
    {
        const float3 normalDx = ddx(normal);
        const float3 normalDy = ddy(normal);
        const float variance = 0.25 * (dot(normalDx, normalDx) + dot(normalDy, normalDy));
        const float alpha = roughness * roughness;
        roughness = sqrt(sqrt(saturate(alpha * alpha + min(2.0 * variance, 0.18))));
    }
    roughness = clamp(roughness, 0.0, 1.0);
    const float alpha = max(roughness * roughness, 1.0e-4);
    const float nDotV = max(dot(normal, toCamera), 1.0e-4);

    // Interface reflectance. Glass has two interfaces (entry and exit);
    // multiple reflections between them give R2 = 2R / (1 + R) and leave
    // (1 - R) / (1 + R) for transmission.
    const float f0Scalar = ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0));
    const float3 f0 = lerp(float3(f0Scalar, f0Scalar, f0Scalar), baseColor.rgb, metallic);
    const float2 brdf = brdfIntegrationLut.Sample(float2(nDotV, roughness)).rg;
    const float smoothFresnel = dielectricFresnel(nDotV, ior);
    const float roughFresnel = f0Scalar * brdf.x + brdf.y;
    // Reflectivity (the material's KHR_materials_specular factor) scales the
    // normal-incidence reflectance F0 and keeps the grazing response:
    // F = k F0 + (1 - k F0) w  =  F(k = 1) + (k - 1) F0 (1 - w).
    const float reflectivity = clamp(material.extensionScalars.w, 0.0, 6.0);
    const float interfaceReflectance =
        saturate(lerp(smoothFresnel, roughFresnel, saturate(roughness * 2.0)) +
                 (reflectivity - 1.0) * f0Scalar * (1.0 - schlickWeight(nDotV)));
    const float slabReflectance = 2.0 * interfaceReflectance / (1.0 + interfaceReflectance);
    const float slabTransmittance = (1.0 - interfaceReflectance) / (1.0 + interfaceReflectance);
    const float3 specularColor = lerp(float3(slabReflectance, slabReflectance, slabReflectance),
                                      f0 * brdf.x + brdf.y, metallic);

    const bool temporal = (frame.screenSpaceEffects & SCREEN_SPACE_EFFECTS_TAA) != 0u;
    const float2 noise = temporal ? float2(interleavedGradientNoise(input.position.xy, frame.temporalSampleIndex),
                                           interleavedGradientNoise(input.position.yx + 17.0, frame.temporalSampleIndex))
                                  : float2(0.5, 0.5);

    const float3 originView = viewPosition(input.worldPosition);
    float3 originScreen = float3(pixelUv, input.position.z);

    // Clusters: local lights and reflection probes.
    ClusterLightRange lightRange = {0u, 0u, 0u, 0u};
    if (frame.localLightCount != 0u || frame.reflectionProbeCount != 0u)
    {
        const uint2 grid = (uint2(frame.clusteredViewportNearFar.xy) + ClusterTilePixelSize - 1u) / ClusterTilePixelSize;
        const float clusterDepth = max(-originView.z, frame.clusteredViewportNearFar.z);
        const uint slice = min(uint(max(log2(clusterDepth) * frame.clusterZScaleBiasEnvironmentMipRtContact.x +
                                            frame.clusterZScaleBiasEnvironmentMipRtContact.y,
                                        0.0)),
                               23u);
        const uint2 tile = min(uint2(input.position.xy) / ClusterTilePixelSize, grid - 1u);
        lightRange = clusterRanges[(slice * grid.y + tile.y) * grid.x + tile.x];
    }

    // ---- Reflection ------------------------------------------------------
    float3 reflectionDirection = reflect(-toCamera, normal);
    if (roughness > 0.02)
    {
        // Stochastic GGX lobe under TAA; the snapshot mip covers the rest.
        const float3 up = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
        const float3 tangentX = normalize(cross(up, normal));
        const float3 tangentY = cross(normal, tangentX);
        const float3 viewTangent = float3(dot(toCamera, tangentX), dot(toCamera, tangentY), dot(toCamera, normal));
        const float3 microNormalTangent = sampleGgxVisibleNormal(viewTangent, temporal ? alpha * 0.5 : 0.0, noise);
        const float3 microNormal = normalize(microNormalTangent.x * tangentX + microNormalTangent.y * tangentY +
                                             microNormalTangent.z * normal);
        const float3 sampled = reflect(-toCamera, microNormal);
        if (dot(sampled, geometricNormal) > 0.0)
            reflectionDirection = sampled;
    }
    const uint prefilteredMipCount = max(uint(frame.clusterZScaleBiasEnvironmentMipRtContact.z), 1u);
    // The sky is the last resort: a ray that missed SSR and every probe
    // usually left the screen towards whatever stands in front of the glass
    // (the street, facades), not open sky. A smooth pane would otherwise
    // mirror sharp sky detail (stars, sun disc) straight back at the camera.
    // Only reflections that rise well above the horizon keep the sharp sky;
    // lower ones get the cosine-convolved environment, which keeps the
    // ambient level but carries no point detail.
    const float skyOpenness = smoothstep(0.05, 0.45, reflectionDirection.y);
    const float3 sharpSky =
        prefilteredEnvironmentMap.SampleLevel(reflectionDirection, roughness * float(prefilteredMipCount - 1u)).rgb;
    ReflectionCandidate sky = {lerp(irradianceMap.SampleLevel(reflectionDirection, 0.0).rgb, sharpSky, skyOpenness),
                               1.0};
    ReflectionCandidate probe = sampleLocalProbes(frame, clusterLightIndices, input.worldPosition, reflectionDirection,
                                                  roughness, prefilteredMipCount, lightRange);
    ReflectionCandidate screenSpace = {float3(0.0, 0.0, 0.0), 0.0};
    if (roughness < GLASS_SSR_MAX_ROUGHNESS)
    {
        float3 rayOrigin;
        float3 rayDirection;
        if (screenSpaceRay(originView, viewDirection(reflectionDirection), rayOrigin, rayDirection))
        {
            float3 hit;
            const uint budget = uint(lerp(float(GLASS_HIZ_ITERATIONS), float(GLASS_HIZ_ROUGH_ITERATIONS), saturate(roughness / GLASS_SSR_MAX_ROUGHNESS)));
            if (traceHiZ(rayOrigin, rayDirection, budget, hit))
            {
                const float confidence = hitConfidence(hit, rayOrigin) *
                                         (1.0 - smoothstep(0.6 * GLASS_SSR_MAX_ROUGHNESS, GLASS_SSR_MAX_ROUGHNESS,
                                                           roughness));
                if (confidence > 0.0)
                {
                    // Cone footprint of the remaining lobe at the hit distance.
                    const float travelled = length((hit.xy - rayOrigin.xy) * size);
                    const float radius = (temporal ? 0.3 : 1.0) * alpha * travelled;
                    screenSpace.radiance = sampleSceneBlurred(hit.xy, radius, noise.x * 6.2831853);
                    screenSpace.confidence = confidence;
                }
            }
        }
    }
    ReflectionCandidate noRayTracing = {float3(0.0, 0.0, 0.0), 0.0};
    float3 reflection = resolveReflection(noRayTracing, screenSpace, probe, sky);
    // Normal maps must not reflect what lies behind the surface.
    const float horizon = saturate(1.0 + dot(reflectionDirection, geometricNormal));
    reflection *= horizon * horizon;

    // ---- Transmission ----------------------------------------------------
    const float3 incident = -toCamera;
    const float3 inside = safeRefract(incident, normal, 1.0 / ior, incident);
    // Exit through a face parallel to the geometric surface: a flat pane
    // only shifts the view ray sideways, a normal map bends it.
    const float cosineInside = max(abs(dot(inside, geometricNormal)), 0.05);
    const float pathLength = thickness / cosineInside;
    const float3 exitPoint = input.worldPosition + inside * pathLength;
    const float3 exitDirection = safeRefract(inside, geometricNormal, ior, incident);

    // Scene point behind this pixel, along the unrefracted view ray.
    const float3 behindView = viewFromScreen(pixelUv, opaqueDepthHere);
    const bool skyBehind = opaqueDepthHere >= 0.99999;
    const float behindDistance = skyBehind ? 1.0e4 : max(length(behindView - originView), 0.0);
    // Estimate: continue the exit ray as far as the scene behind the pixel.
    const float3 estimateView = viewPosition(exitPoint) + viewDirection(exitDirection) * behindDistance;
    float2 refractedUv = pixelUv;
    if (estimateView.z < -1.0e-3)
        refractedUv = screenFromView(estimateView).xy;
    float refractedDepth = sceneDepthAt(refractedUv);
    // Hi-Z trace when the ray bends noticeably (normal maps, ripples).
    if (dot(exitDirection, incident) < 0.9995 && !skyBehind)
    {
        float3 rayOrigin;
        float3 rayDirection;
        if (screenSpaceRay(viewPosition(exitPoint), viewDirection(exitDirection), rayOrigin, rayDirection))
        {
            float3 hit;
            if (traceHiZ(rayOrigin, rayDirection, 48u, hit) && all(hit.xy > 0.0) && all(hit.xy < 1.0))
            {
                refractedUv = hit.xy;
                refractedDepth = sceneDepthAt(hit.xy);
            }
        }
    }
    // Scene in front of the glass at the refracted position (a frame or a
    // hand) must not show through it: fall back to the straight view ray.
    if (any(refractedUv <= 0.0) || any(refractedUv >= 1.0) || refractedDepth < input.position.z)
    {
        refractedUv = pixelUv;
        refractedDepth = opaqueDepthHere;
    }
    // Frosting: the transmitted lobe widens with roughness (none at IOR 1)
    // and with the distance to what is seen through the glass.
    const float transmissionRoughness = max(roughness * saturate((ior - 1.0) * 2.0), dirt * 0.35);
    const float3 refractedView = viewFromScreen(refractedUv, refractedDepth);
    const float throughDistance = refractedDepth >= 0.99999 ? 50.0 : length(refractedView - originView);
    const float pixelsPerMetre = 0.5 * size.y * abs(frame.projection[1][1]) / max(-refractedView.z, 1.0e-2);
    const float transmissionAlpha = transmissionRoughness * transmissionRoughness;
    const float blurRadius = min(transmissionAlpha * min(throughDistance, 25.0) * pixelsPerMetre * 0.5, 160.0);
    float3 transmitted = sampleSceneBlurred(refractedUv, blurRadius, noise.y * 6.2831853);

    // Beer-Lambert absorption over the path inside the glass.
    if (material.glassAttenuation.w > 0.0 && pathLength > 0.0)
    {
        const float3 attenuationColor = max(material.glassAttenuation.rgb, float3(1.0e-4, 1.0e-4, 1.0e-4));
        transmitted *= exp(log(attenuationColor) * (pathLength / material.glassAttenuation.w));
    }
    transmitted *= baseColor.rgb;

    // ---- Direct light ----------------------------------------------------
    const float3 lightDirection = normalize(-frame.lightDirectionIntensity.xyz);
    float3 directSpecular = 0.0;
    float3 directIrradiance = 0.0;
    {
        ShadowSampleDebug shadowDebug = {0.0, 0.0, 0u, 0u, uint2(0, 0), 0u, 0u};
        const float shadow = frame.shadowEnabled != 0u
                                 ? sampleVirtualShadow(shadowMap, linearShadowMap, shadowDepthMap, input.worldPosition,
                                                       geometricNormal, lightDirection, frame, shadowPageTable,
                                                       frame.shadowQuality, uint2(input.position.xy),
                                                       frame.temporalSampleIndex, frame.temporalShadowJitter != 0,
                                                       shadowDebug)
                                 : 0.0;
        const float visibility = 1.0 - shadow;
        const float3 radiance = frame.lightColor.rgb * frame.lightDirectionIntensity.w * visibility;
        const float nDotL = saturate(dot(normal, lightDirection));
        if (nDotL > 0.0)
        {
            const float3 halfway = normalize(toCamera + lightDirection);
            const float fresnel = dielectricFresnel(saturate(dot(halfway, toCamera)), ior);
            directSpecular += distributionGGX(normal, halfway, roughness) *
                              geometrySchlickGGX(nDotV, roughness) * geometrySchlickGGX(nDotL, roughness) *
                              lerp(float3(fresnel, fresnel, fresnel), f0, metallic) / max(4.0 * nDotV * nDotL, 1.0e-4) *
                              radiance * nDotL;
        }
        directIrradiance += radiance * abs(dot(normal, lightDirection));
    }
    // Lights go in batches of four, as in forward_pbr: their indices and
    // positions load as independent requests and the range test runs on all
    // four before any shading. One light at a time, the index -> light load
    // chain of every cluster entry dominated the glass at night, where most
    // of a cluster's lights do not reach the pane.
    const uint lightCount = min(lightRange.lightCount, 256u);
    for (uint batchStart = 0u; batchStart < lightCount; batchStart += 4u)
    {
    uint4 batch;
    [unroll] for (uint k = 0u; k < 4u; ++k)
        batch[k] = batchStart + k < lightCount ? clusterLightIndices[lightRange.lightOffset + batchStart + k]
                                               : 0xffffffffu;
    uint inRange = 0u;
    [unroll] for (uint k = 0u; k < 4u; ++k)
    {
        if (batch[k] >= frame.localLightCount)
            continue;
        const float4 positionRange = localLights[batch[k]].positionRange;
        const float3 offset = positionRange.xyz - input.worldPosition;
        const float squared = dot(offset, offset);
        if (squared > 1.0e-8 && squared < positionRange.w * positionRange.w)
            inRange |= 1u << k;
    }
    [loop] while (inRange != 0u)
    {
        const uint slot = firstbitlow(inRange);
        inRange &= inRange - 1u;
        const uint lightIndex = slot == 0u ? batch.x : slot == 1u ? batch.y : slot == 2u ? batch.z : batch.w;
        const LocalLightData local = localLights[lightIndex];
        const float3 toLight = local.positionRange.xyz - input.worldPosition;
        const float distanceSquared = dot(toLight, toLight);
        const float radius = local.positionRange.w;
        if (distanceSquared <= 1.0e-8 || distanceSquared >= radius * radius)
            continue;
        const float inverseDistance = rsqrt(distanceSquared);
        const float3 localDirection = toLight * inverseDistance;
        const float rangeFactor = saturate(1.0 - distanceSquared * inverseDistance / radius);
        float attenuation = rangeFactor * rangeFactor / max(distanceSquared, LocalLightSourceRadiusSquared);
        attenuation *= localLightProfile(local, -localDirection);
        if (local.parameters.y > 1.5)
        {
            const float cone = dot(normalize(-local.directionOuterCos.xyz), localDirection);
            attenuation *= smoothstep(local.directionOuterCos.w, local.parameters.x, cone);
        }
        // Lights near the end of their range add almost nothing (dozens per
        // cluster in dense scenes): skip them before the GGX, with the
        // forward and deferred passes' cutoff. |N.L| bounds both terms.
        const float cosine = dot(normal, localDirection);
        if (local.colorIntensity.w * attenuation * abs(cosine) < LocalLightCutoff)
            continue;
        const float3 radiance = local.colorIntensity.rgb * local.colorIntensity.w * attenuation;
        const float nDotL = saturate(cosine);
        if (nDotL > 0.0)
        {
            const float3 halfway = normalize(toCamera + localDirection);
            const float fresnel = dielectricFresnel(saturate(dot(halfway, toCamera)), ior);
            directSpecular += distributionGGX(normal, halfway, roughness) *
                              geometrySchlickGGX(nDotV, roughness) * geometrySchlickGGX(nDotL, roughness) *
                              lerp(float3(fresnel, fresnel, fresnel), f0, metallic) / max(4.0 * nDotV * nDotL, 1.0e-4) *
                              radiance * nDotL;
        }
        directIrradiance += radiance * abs(cosine);
    }
    }

    // ---- Composite -------------------------------------------------------
    directSpecular *= max(reflectivity, 0.0);
    const float3 ambientIrradiance = irradianceMap.SampleLevel(normal, 0.0).rgb;
    // Light the non-transmissive share and the dirt as rough diffuse layers.
    const float3 diffuseLight = (directIrradiance / PBR_PI + ambientIrradiance) * (1.0 - interfaceReflectance);
    const float3 body = lerp(baseColor.rgb * (1.0 - metallic) * diffuseLight, transmitted * slabTransmittance,
                             transmission);
    float3 color = body + reflection * specularColor + directSpecular;
    dirt = saturate(dirt);
    if (dirt > 0.0)
    {
        const float3 dirtLight = (directIrradiance / PBR_PI + ambientIrradiance) * dirtAlbedo;
        color = lerp(color, dirtLight + reflection * specularColor * 0.5, dirt);
    }
    float3 emissive = material.emissiveColorIntensity.rgb * material.emissiveColorIntensity.w;
    if (material.extensionTextureIndices.x >= 0)
        emissive *= materialTextures[NonUniformResourceIndex(material.extensionTextureIndices.x)]
                        .Sample(materialTextureUv(material, 7, input.texCoord, input.texCoord1)).rgb;
    color += emissive;
    if (coverage < 1.0)
        color = lerp(sceneColor.SampleLevel(pixelUv, 0.0).rgb, color, coverage);

    GlassFragmentOutput output;
    output.color = float4(min(color, 65000.0), 1.0);
    return output;
}
