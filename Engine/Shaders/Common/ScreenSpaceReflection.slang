// Screen-space reflections: Hi-Z traced like GlassPass (Common/HiZTrace),
// against this frame's depth; the hit is reprojected into the previous
// frame, whose lit colour is blurred by the roughness cone.
//
// Shared by the raster forward programs, which trace inline, and the
// visibility-buffer reflection pass (Forward/visibility_ssr.slang), which
// traces the pixels the resolve programs queued. `closestDepth` is SsrPass's
// closest-surface pyramid (level 0 included), `radiance` the previous
// frame's lit colour with its mip chain.
import Common.SceneData;
import Common.HiZTrace;
import Common.ReflectionFallback;

// Bit 6 of `screenSpaceEffects`: SSR traces this frame.
static const uint SCREEN_SPACE_EFFECTS_SSR = 64u;
// Deepest pyramid level the trace visits (SsrPass::MaxPyramidLevel).
static const int SSR_HIZ_MAX_MIP = 7;
// Hits closer than this share of view depth (plus ssrParams.w metres) in
// front of the stored surface count as a hit; beyond it the ray passed behind.
static const float SSR_DEPTH_THICKNESS_SCALE = 0.03;

ReflectionCandidate traceScreenSpaceReflection(Sampler2D closestDepth, Sampler2D radiance, RendererFrameData frame,
                                               float2 pixel, float3 worldPosition, float3 reflectionDirection,
                                               float roughness)
{
    ReflectionCandidate result = {float3(0.0, 0.0, 0.0), 0.0};
    uint width, height, mips;
    closestDepth.GetDimensions(0, width, height, mips);
    const float2 size = float2(width, height);
    const float3 originView = mul(float4(worldPosition, 1.0), frame.view).xyz;
    const float3 directionView = normalize(mul(float4(reflectionDirection, 0.0), frame.view).xyz);
    float3 rayOrigin;
    float3 rayDirection;
    if (!hizScreenSpaceRay(originView, directionView, frame.projection, rayOrigin, rayDirection))
        return result;
    const float maxRoughness = max(frame.ssrParams.y, 1.0e-3);
    const float roughnessFade = 1.0 - smoothstep(0.6 * maxRoughness, maxRoughness, roughness);
    // Rough rays only feed a blurred lookup: half the steps suffice.
    const uint budget = uint(max(lerp(frame.ssrParams.z, 0.5 * frame.ssrParams.z, roughness / maxRoughness), 8.0));
    float3 hit;
    if (!hizTrace(closestDepth, closestDepth, size, min(int(mips) - 1, SSR_HIZ_MAX_MIP), rayOrigin, rayDirection,
                  budget, hit))
        return result;
    float confidence = hizHitConfidence(closestDepth, size, frame.inverseProjection, hit, rayOrigin, frame.ssrParams.w,
                                        SSR_DEPTH_THICKNESS_SCALE) *
                       roughnessFade;
    if (confidence <= 0.0)
        return result;
    // The radiance is last frame's image: move the hit there with the camera.
    const float3 hitView = hizViewFromScreen(hit.xy, hit.z, frame.inverseProjection);
    const float3 hitWorld = mul(float4(hitView, 1.0), frame.inverseView).xyz;
    const float4 previousClip = mul(mul(float4(hitWorld, 1.0), frame.previousView), frame.previousProjection);
    if (previousClip.w <= 1.0e-4)
        return result;
    const float2 previousUv = previousClip.xy / previousClip.w * 0.5 + 0.5;
    const float2 fade = 0.025 * float2(size.y / size.x, 1.0);
    const float2 border = smoothstep(float2(0.0, 0.0), fade, previousUv) *
                          (1.0 - smoothstep(1.0 - fade, float2(1.0, 1.0), previousUv));
    confidence *= border.x * border.y;
    if (confidence <= 0.0)
        return result;
    // Cone footprint of the lobe at the hit distance.
    const float travelled = length((hit.xy - rayOrigin.xy) * size);
    const float radius = roughness * roughness * travelled;
    const float rotation = hizInterleavedGradientNoise(pixel, frame.temporalSampleIndex) * 6.2831853;
    result.radiance = min(hizSampleBlurred(radiance, size, previousUv, radius, rotation), 65000.0);
    result.confidence = saturate(confidence * frame.ssrParams.x);
    return result;
}

// Queued reflection of a visibility-resolved pixel (VisibilityBufferPass):
// the resolve wrote its colour with the probe/sky reflection `fallback`
// weighted by `weight`; the reflection pass adds
// weight * confidence * (ssr - fallback), which is the resolve's own
// lerp(fallback, ssr, confidence) * weight. Packed as halves:
//   x: pixel x | y << 16, y: weight.rg, z: weight.b | fallback.r, w: fallback.gb
struct SsrRequest
{
    uint2 pixel;
    float3 weight;
    float3 fallback;
};

uint4 packSsrRequest(uint2 pixel, float3 weight, float3 fallback)
{
    // Halves top out at 65504; the HDR target already clamps to 65000.
    weight = clamp(weight, -65000.0, 65000.0);
    fallback = min(fallback, 65000.0);
    return uint4(pixel.x | (pixel.y << 16u), f32tof16(weight.r) | (f32tof16(weight.g) << 16u),
                 f32tof16(weight.b) | (f32tof16(fallback.r) << 16u),
                 f32tof16(fallback.g) | (f32tof16(fallback.b) << 16u));
}

SsrRequest unpackSsrRequest(uint4 packed)
{
    SsrRequest request;
    request.pixel = uint2(packed.x & 0xffffu, packed.x >> 16u);
    request.weight = float3(f16tof32(packed.y & 0xffffu), f16tof32(packed.y >> 16u), f16tof32(packed.z & 0xffffu));
    request.fallback = float3(f16tof32(packed.z >> 16u), f16tof32(packed.w & 0xffffu), f16tof32(packed.w >> 16u));
    return request;
}

// VisibilityBufferPass's dispatch buffer: after the ClassCount padded
// VkDispatchIndirectCommands, the reflection pass's, whose fourth word
// counts the queued requests.
static const uint SsrDispatchBase = 4u * 4u;
static const uint SsrDispatchCount = SsrDispatchBase + 3u;
static const uint SsrThreads = 128u;
