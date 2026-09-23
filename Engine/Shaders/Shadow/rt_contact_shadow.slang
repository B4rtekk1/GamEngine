// Inline Ray Query contact visibility.  This module is deliberately separate
// from forward_pbr so non-RT forward pipelines never declare an AS binding.
import Common.SceneData;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure sceneTlas;
[[vk::binding(1, 0)]] Sampler2D depthTexture;
[[vk::binding(2, 0)]] Sampler2D viewNormalTexture;
[[vk::binding(3, 0)]] RWTexture2D<float> contactVisibility;
[[vk::binding(4, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(5, 0)]] Sampler2D directionalVisibility;

struct ContactSettings { float maxDistance; float normalBias; };
[[vk::push_constant]] ContactSettings settings;

float3 decodeOct(float2 e) {
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * select(-1.0, 1.0, n.xy >= 0.0);
    return normalize(n);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint width, height; contactVisibility.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    int2 pixel = int2(id.xy);
    uint sourceWidth, sourceHeight; depthTexture.GetDimensions(sourceWidth, sourceHeight);
    // The visibility target can be smaller than the depth/normal targets.
    // Address the centre of the corresponding full-resolution footprint.
    int2 sourcePixel = min(int2((float2(id.xy) + 0.5) * float2(sourceWidth, sourceHeight) /
                                float2(width, height)), int2(sourceWidth, sourceHeight) - 1);
    float depth = depthTexture.Load(int3(sourcePixel, 0)).x;
    if (depth >= 0.999999) { contactVisibility[pixel] = 1.0; return; }
    float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    const float vsmVisibility = directionalVisibility.SampleLevel(uv, 0).r;
    if (vsmVisibility <= 1.0 / 255.0) {
        contactVisibility[pixel] = 0.0;
        return;
    }
    float4 viewPosition = mul(float4(uv * 2.0 - 1.0, depth, 1.0), frame.inverseProjection);
    viewPosition /= viewPosition.w;
    float3 worldPosition = mul(viewPosition, frame.inverseView).xyz;
    float3 viewNormal = decodeOct(viewNormalTexture.Load(int3(sourcePixel, 0)).xy);
    float3 worldNormal = normalize(mul(float4(viewNormal, 0.0), frame.inverseView).xyz);
    if (frame.lightDirectionIntensity.w <= 0.0001) { contactVisibility[pixel] = vsmVisibility; return; }
    float3 lightDirection = normalize(-frame.lightDirectionIntensity.xyz);
    if (dot(worldNormal, lightDirection) <= 0.0) { contactVisibility[pixel] = vsmVisibility; return; }
    RayDesc ray;
    ray.Origin = worldPosition + worldNormal * settings.normalBias;
    ray.Direction = lightDirection;
    ray.TMin = 0.001;
    ray.TMax = settings.maxDistance;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(sceneTlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE, 0x01, ray);
    query.Proceed();
    contactVisibility[pixel] = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : vsmVisibility;
}
