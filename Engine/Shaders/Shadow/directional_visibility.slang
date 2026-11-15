import Common.SceneData;
import Common.ShadowSampling;

[[vk::binding(0, 0)]] Sampler2DShadow shadowMap;
[[vk::binding(1, 0)]] Sampler2DShadow linearShadowMap;
[[vk::binding(2, 0)]] Sampler2D shadowDepthMap;
[[vk::binding(3, 0)]] StructuredBuffer<uint4> shadowPageTable;
[[vk::binding(4, 0)]] Sampler2D depthTexture;
[[vk::binding(5, 0)]] Sampler2D viewNormalTexture;
[[vk::binding(6, 0)]] ConstantBuffer<RendererFrameData> frame;
// Match DirectionalVisibilityPass's storage image without optional format features.
[[vk::binding(7, 0), vk::image_format("r32f")]] RWTexture2D<float> visibility;

float3 decodeOct(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    return normalize(n);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    visibility.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const int2 pixel = int2(id.xy);
    const float depth = depthTexture.Load(int3(pixel, 0)).r;
    if (depth >= 0.999999 || frame.shadowEnabled == 0)
    {
        visibility[pixel] = 1.0;
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    const float4 homogeneousView = mul(float4(uv * 2.0 - 1.0, depth, 1.0), frame.inverseProjection);
    const float4 viewPosition = homogeneousView / homogeneousView.w;
    // Reconstruct the actual surface footprint. Reusing the centre depth
    // assumes a camera-facing plane and underestimates grazing receivers,
    // selecting fine levels that the depth-driven marker never requested.
    // Quad partners match fragment derivatives, including the viewport edge.
    const uint2 rightPixel = uint2(min(id.x ^ 1u, width - 1u), id.y);
    const uint2 downPixel = uint2(id.x, min(id.y ^ 1u, height - 1u));
    const float rightDepth = depthTexture.Load(int3(rightPixel, 0)).r;
    const float downDepth = depthTexture.Load(int3(downPixel, 0)).r;
    // The unprojection is linear before the divide: a partner differs from
    // this pixel by one NDC axis step and its depth. Likewise the view to
    // world transform is affine, so only the view-space offset is rotated.
    const float4 viewRight = homogeneousView +
        frame.inverseProjection[0] * (2.0 * (float(rightPixel.x) - float(id.x)) / float(width)) +
        frame.inverseProjection[2] * ((rightDepth < 0.999999 ? rightDepth : depth) - depth);
    const float4 viewDown = homogeneousView +
        frame.inverseProjection[1] * (2.0 * (float(downPixel.y) - float(id.y)) / float(height)) +
        frame.inverseProjection[2] * ((downDepth < 0.999999 ? downDepth : depth) - depth);
    const float3x3 viewToWorld = float3x3(frame.inverseView[0].xyz, frame.inverseView[1].xyz,
                                          frame.inverseView[2].xyz);
    const float3 worldPosition = mul(viewPosition, frame.inverseView).xyz;
    const float3 worldRight = worldPosition + mul(viewRight.xyz / viewRight.w - viewPosition.xyz, viewToWorld);
    const float3 worldDown = worldPosition + mul(viewDown.xyz / viewDown.w - viewPosition.xyz, viewToWorld);
    const float3 viewNormal = decodeOct(viewNormalTexture.Load(int3(pixel, 0)).xy);
    const float3 worldNormal = normalize(mul(float4(viewNormal, 0.0), frame.inverseView).xyz);
    const float3 lightDirection = normalize(-frame.lightDirectionIntensity.xyz);
    ShadowSampleDebug debug;
    const float shadow = sampleVirtualShadow(shadowMap, linearShadowMap, shadowDepthMap,
        worldPosition, worldNormal, lightDirection, frame,
        shadowPageTable,
        frame.shadowQuality, id.xy, frame.temporalSampleIndex,
        frame.temporalShadowJitter != 0, worldRight, worldDown, debug);
    // Forward reads the debug colour from here (unpackShadowDebugColor).
    visibility[pixel] = frame.shadowDebugView != SHADOW_DEBUG_OFF
        ? packShadowDebugColor(shadowDebugColor(frame.shadowDebugView, debug))
        : saturate(1.0 - shadow);
}
