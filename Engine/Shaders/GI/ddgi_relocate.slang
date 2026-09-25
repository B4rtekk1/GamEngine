import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;

[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(10, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> fixedRayData;
[[vk::binding(12, 1)]] StructuredBuffer<uint> probeUpdateList;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ddgiUpdateCount(ddgi)) return;
    const uint probeIndex = probeUpdateList[id.x];
    const int2 pixel = int2(ddgiProbePixel(probeIndex));
    const float4 previous = ddgi.distanceInitialized.y > 0.5
        ? probeData[pixel] : float4(0.0, 0.0, 0.0, 2.0);
    if (!ddgiShouldValidate(previous.w, probeIndex, uint(ddgi.scrollDeltaFrame.w))) return;
    uint backfaces = 0u;
    float nearestBackface = ddgi.distanceInitialized.x;
    float3 exitDirection = 0.0;
    float3 frontfacePush = 0.0;
    for (uint rayIndex = 0u; rayIndex < DDGI_FIXED_RAYS; ++rayIndex) {
        const float distance = fixedRayData[int2(rayIndex, probeIndex)].r;
        const float3 direction = ddgiFixedRayDirection(rayIndex);
        if (distance < 0.0) {
            ++backfaces;
            if (-distance < nearestBackface) {
                nearestBackface = -distance;
                exitDirection = direction;
            }
        } else if (distance < ddgi.originSpacing.w * 0.25) {
            frontfacePush -= direction * (ddgi.originSpacing.w * 0.25 - distance);
        }
    }
    float3 offset = previous.xyz;
    const bool insideGeometry = float(backfaces) / float(DDGI_FIXED_RAYS) >= DDGI_BACKFACE_THRESHOLD;
    if (insideGeometry && nearestBackface < ddgi.distanceInitialized.x) {
        offset += exitDirection * min(nearestBackface + 0.05, ddgi.originSpacing.w * 0.25);
    } else if (dot(frontfacePush, frontfacePush) > 1e-6) {
        offset += normalize(frontfacePush) *
            min(length(frontfacePush) / float(DDGI_FIXED_RAYS), ddgi.originSpacing.w * 0.1);
    }
    // Keep the probe inside its own voxel even after many update frames.
    const float maxOffset = ddgi.originSpacing.w * 0.45;
    const float offsetLength = length(offset);
    if (offsetLength > maxOffset) offset *= maxOffset / offsetLength;
    const bool moved = length(offset - previous.xyz) > 0.02;
    const int3 oldLogical = int3(ddgiLogicalGrid(probeIndex, ddgi.scrollOffsetRays.xyz)) +
                            ddgi.scrollDeltaFrame.xyz;
    const bool newlyExposed = ddgi.distanceInitialized.y < 0.5 ||
        any(oldLogical < 0) || any(oldLogical >= int3(DDGI_COUNTS));
    const float state = moved || newlyExposed ? 2.0 :
                        (previous.w > 0.5 ? 1.0 : 0.0);
    probeData[pixel] = float4(offset, state);
}
