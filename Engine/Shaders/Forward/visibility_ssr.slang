// Screen-space reflections of the visibility-resolved pixels
// (VisibilityBufferPass). The resolve programs shade with the probe/sky
// reflection and queue every pixel whose reflection the trace may replace
// (forward_pbr.slang, FORWARD_DEFERRED_SSR); one thread per queued pixel
// then traces it and adds weight * confidence * (ssr - fallback). Traced
// apart from the surface shading, the Hi-Z loop runs with few registers on
// warps of traced pixels only.
//
// Set 0 is the forward scene set, set 2 the resolve set: same pipeline
// layout as the resolve programs.
import Common.SceneData;
import Common.HiZTrace;
import Common.ReflectionFallback;
import Common.DeferredLocalLighting;
import Common.ScreenSpaceReflection;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(27, 0)]] Sampler2D ssrClosestDepth;
[[vk::binding(28, 0)]] Sampler2D ssrRadiance;
[[vk::binding(1, 2), vk::image_format("rgba16f")]] RWTexture2D<float4> hdrOutput;
// The resolve's deferred local light inputs: the shading normal and roughness.
[[vk::binding(2, 2), vk::image_format("rgba32ui")]] RWTexture2D<uint4> surfaceData;
[[vk::binding(4, 2)]] RWStructuredBuffer<uint4> ssrRequests;
[[vk::binding(5, 2)]] RWStructuredBuffer<uint> resolveDispatch;

// forward_pbr.slang's PBR debug view.
static const uint PBR_DEBUG_SSR_CONFIDENCE = 11u;

[numthreads(SsrThreads, 1, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= resolveDispatch[SsrDispatchCount])
        return;
    const SsrRequest request = unpackSsrRequest(ssrRequests[id.x]);
    DeferredLocalSurface surface;
    if (!unpackDeferredLocalSurface(surfaceData[request.pixel], surface))
        return;
    uint width, height, mips;
    ssrClosestDepth.GetDimensions(0, width, height, mips);
    // The prepass depth of the resolved triangle at the pixel centre, where
    // the resolve interpolated its attributes.
    const float2 pixelCentre = float2(request.pixel) + 0.5;
    const float depth = ssrClosestDepth.Load(int3(int2(request.pixel), 0)).r;
    const float3 view = hizViewFromScreen(pixelCentre / float2(width, height), depth, frame.inverseProjection);
    const float3 worldPosition = mul(float4(view, 1.0), frame.inverseView).xyz;
    const float3 viewDirection = normalize(frame.cameraPosition.xyz - worldPosition);
    const float3 reflection = reflect(-viewDirection, surface.normal);
    const ReflectionCandidate traced = traceScreenSpaceReflection(ssrClosestDepth, ssrRadiance, frame, pixelCentre,
                                                                  worldPosition, reflection, surface.roughness);
    if (traced.confidence <= 0.0)
        return;
    const float3 change = frame.pbrDebugView == PBR_DEBUG_SSR_CONFIDENCE ? request.weight * traced.confidence
                          : request.weight * traced.confidence * (traced.radiance - request.fallback);
    const float4 color = hdrOutput[request.pixel];
    // The colour holds weight * fallback, so the sum stays positive up to
    // half-float rounding.
    hdrOutput[request.pixel] = float4(clamp(color.rgb + change, 0.0, 65000.0), color.a);
}
