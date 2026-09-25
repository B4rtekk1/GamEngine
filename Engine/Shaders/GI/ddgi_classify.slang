import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;

[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(10, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> fixedRayData;
[[vk::binding(12, 1)]] StructuredBuffer<DDGIProbeUpdate> probeUpdateList;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ddgiUpdateCount(ddgi)) return;
    const uint probeIndex = probeUpdateList[id.x].probeIndex;
    const int2 pixel = int2(ddgiProbePixel(probeIndex));
    const float4 probe = probeData[pixel];
    if (!ddgiShouldValidate(probe.w, probeIndex, uint(ddgi.scrollDeltaFrame.w))) return;
    uint backfaces = 0u;
    for (uint rayIndex = 0u; rayIndex < DDGI_FIXED_RAYS; ++rayIndex) {
        const float distance = fixedRayData[int2(rayIndex, probeIndex)].r;
        backfaces += distance < 0.0 ? 1u : 0u;
    }
    const bool insideGeometry = float(backfaces) / float(DDGI_FIXED_RAYS) > DDGI_BACKFACE_THRESHOLD;
    bool active = false;
    if (!insideGeometry) {
        for (uint rayIndex = 0u; rayIndex < DDGI_FIXED_RAYS; ++rayIndex) {
            const float distance = fixedRayData[int2(rayIndex, probeIndex)].r;
            if (distance < 0.0 || distance >= ddgi.distanceInitialized.x - 0.01) continue;
            const float3 direction = ddgiFixedRayDirection(rayIndex);
            // The nearest probe-spacing boundary along this ray.
            const float3 planeDistances = ddgi.originSpacing.w /
                max(abs(direction), float3(1e-6));
            if (distance <= min(planeDistances.x, min(planeDistances.y, planeDistances.z))) {
                active = true;
                break;
            }
        }
    }
    const float state = active ? (probe.w < 0.5 ? 2.0 : probe.w) : 0.0;
    probeData[pixel] = float4(probe.xyz, state);
}
