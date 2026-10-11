// Inline Ray Query contact visibility.  This module is deliberately separate
// from forward_pbr so non-RT forward pipelines never declare an AS binding.
import Common.SceneData;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure sceneTlas;
[[vk::binding(1, 0)]] Sampler2D depthTexture;
[[vk::binding(2, 0)]] Sampler2D viewNormalTexture;
// Match RtContactShadowPass's storage image without requiring formatless writes.
[[vk::binding(3, 0), vk::image_format("r32f")]] RWTexture2D<float> contactVisibility;
[[vk::binding(4, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(5, 0)]] Sampler2D directionalVisibility;

struct ContactSettings { float maxDistance; float normalBias; };
[[vk::push_constant]] ContactSettings settings;

float3 decodeOct(float2 e) {
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    return normalize(n);
}

float3 reconstructViewPosition(int2 pixel, float depth, int2 extent) {
    const float2 uv = (float2(pixel) + 0.5) / float2(extent);
    const float4 position = mul(float4(uv * 2.0 - 1.0, depth, 1.0), frame.inverseProjection);
    return position.xyz / position.w;
}

float3 receiverGeometricNormal(int2 pixel, int2 extent, float3 center, float3 fallback) {
    // Prefer the neighbour on the same surface on each axis. Forward-only
    // differences join unrelated surfaces at silhouettes and produce bad offsets.
    const int2 left = max(pixel - int2(1, 0), int2(0));
    const int2 right = min(pixel + int2(1, 0), extent - 1);
    const int2 up = max(pixel - int2(0, 1), int2(0));
    const int2 down = min(pixel + int2(0, 1), extent - 1);
    const float3 pLeft = reconstructViewPosition(left, depthTexture.Load(int3(left, 0)).r, extent);
    const float3 pRight = reconstructViewPosition(right, depthTexture.Load(int3(right, 0)).r, extent);
    const float3 pUp = reconstructViewPosition(up, depthTexture.Load(int3(up, 0)).r, extent);
    const float3 pDown = reconstructViewPosition(down, depthTexture.Load(int3(down, 0)).r, extent);
    const float3 dx = abs(pRight.z - center.z) < abs(pLeft.z - center.z)
        ? pRight - center : center - pLeft;
    const float3 dy = abs(pDown.z - center.z) < abs(pUp.z - center.z)
        ? pDown - center : center - pUp;
    const float3 normal = cross(dx, dy);
    const float normalSquared = dot(normal, normal);
    // Thin geometry and viewport boundaries may not have two valid neighbours.
    if (!all(isfinite(normal)) || normalSquared <= 1.0e-8 * dot(dx, dx) * dot(dy, dy) ||
        max(abs(dx.z), abs(dy.z)) > max(0.01, abs(center.z) * 0.02)) return fallback;
    const float3 result = normal * rsqrt(normalSquared);
    return dot(result, fallback) < 0.0 ? -result : result;
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
    // Use the exact full-resolution texel center selected for depth and
    // normals, so the reconstructed receiver and VSM sample stay aligned.
    float2 sourceUv = (float2(sourcePixel) + 0.5) / float2(sourceWidth, sourceHeight);
    const float vsmVisibility = directionalVisibility.SampleLevel(sourceUv, 0).r;
    if (vsmVisibility <= 1.0 / 255.0) {
        // VSM already occludes this receiver; skip the ray without copying VSM into the contact mask.
        contactVisibility[pixel] = 1.0;
        return;
    }
    const int2 sourceExtent = int2(sourceWidth, sourceHeight);
    const float3 viewPosition = reconstructViewPosition(sourcePixel, depth, sourceExtent);
    float3 worldPosition = mul(float4(viewPosition, 1.0), frame.inverseView).xyz;
    const float3 shadingNormal = decodeOct(viewNormalTexture.Load(int3(sourcePixel, 0)).xy);
    const float3 viewNormal = receiverGeometricNormal(sourcePixel, sourceExtent, viewPosition, shadingNormal);
    float3 worldNormal = normalize(mul(float4(viewNormal, 0.0), frame.inverseView).xyz);
    if (frame.lightDirectionIntensity.w <= 0.0001) { contactVisibility[pixel] = 1.0; return; }
    float3 lightDirection = normalize(-frame.lightDirectionIntensity.xyz);
    // A smooth vertex normal can offset the origin inside its own triangle.
    // Offset along the depth-derived surface normal, accounting for one depth
    // quantization step and world-coordinate rounding as well as the user bias.
    const float3 fartherPosition = reconstructViewPosition(sourcePixel,
        min(depth + 5.96046448e-8, 1.0), sourceExtent);
    const float depthError = abs(dot(fartherPosition - viewPosition, viewNormal));
    const float positionError = max(abs(worldPosition.x), max(abs(worldPosition.y), abs(worldPosition.z))) * 1.0e-6;
    const float receiverBias = max(settings.normalBias, 0.0) + depthError + positionError;
    RayDesc ray;
    ray.Origin = worldPosition + worldNormal * receiverBias;
    ray.Direction = lightDirection;
    ray.TMin = 0.001;
    ray.TMax = settings.maxDistance;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(sceneTlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE, 0x01, ray);
    query.Proceed();
    contactVisibility[pixel] = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}
