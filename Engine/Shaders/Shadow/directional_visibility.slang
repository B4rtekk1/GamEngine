import Common.SceneData;
import Common.ShadowSampling;

[[vk::binding(0, 0)]] Sampler2DShadow shadowMap;
[[vk::binding(1, 0)]] Sampler2DShadow linearShadowMap;
[[vk::binding(2, 0)]] Sampler2D shadowDepthMap;
[[vk::binding(3, 0)]] StructuredBuffer<uint4> shadowPageTable;
[[vk::binding(4, 0)]] Sampler2D depthTexture;
[[vk::binding(5, 0)]] Sampler2D viewNormalTexture;
[[vk::binding(6, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(7, 0)]] RWTexture2D<float> visibility;

float3 decodeOct(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * select(-1.0, 1.0, n.xy >= 0.0);
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
    float4 viewPosition = mul(float4(uv * 2.0 - 1.0, depth, 1.0), frame.inverseProjection);
    viewPosition /= viewPosition.w;
    float4 viewRight = mul(float4((uv + float2(1.0 / width, 0.0)) * 2.0 - 1.0,
        depth, 1.0), frame.inverseProjection);
    float4 viewDown = mul(float4((uv + float2(0.0, 1.0 / height)) * 2.0 - 1.0,
        depth, 1.0), frame.inverseProjection);
    viewRight /= viewRight.w;
    viewDown /= viewDown.w;
    const float3 worldPosition = mul(viewPosition, frame.inverseView).xyz;
    const float3 worldRight = mul(viewRight, frame.inverseView).xyz;
    const float3 worldDown = mul(viewDown, frame.inverseView).xyz;
    const float3 viewNormal = decodeOct(viewNormalTexture.Load(int3(pixel, 0)).xy);
    const float3 worldNormal = normalize(mul(float4(viewNormal, 0.0), frame.inverseView).xyz);
    const float3 lightDirection = normalize(-frame.lightDirectionIntensity.xyz);
    ShadowSampleDebug debug;
    const float shadow = sampleVirtualShadow(shadowMap, linearShadowMap, shadowDepthMap,
        worldPosition, worldNormal, lightDirection, frame.shadowClipMatrices, shadowPageTable,
        frame.shadowQuality, id.xy, frame.temporalSampleIndex,
        frame.temporalShadowJitter != 0, worldRight, worldDown, debug);
    visibility[pixel] = saturate(1.0 - shadow);
}
