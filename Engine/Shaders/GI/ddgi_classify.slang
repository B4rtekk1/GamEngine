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
    const float4 probe = probeData[pixel];
    if (!ddgiShouldValidate(probe.w, probeIndex, uint(ddgi.scrollDeltaFrame.w))) return;
    uint backfaces = 0u;
    uint hits = 0u;
    for (uint rayIndex = 0u; rayIndex < DDGI_FIXED_RAYS; ++rayIndex) {
        const float distance = fixedRayData[int2(rayIndex, probeIndex)].r;
        backfaces += distance < 0.0 ? 1u : 0u;
        hits += abs(distance) < ddgi.distanceInitialized.x - 0.01 ? 1u : 0u;
    }
    const bool active = backfaces < DDGI_FIXED_RAYS / 2u && hits > 0u;
    const float state = active ? (probe.w < 0.5 ? 2.0 : probe.w) : 0.0;
    probeData[pixel] = float4(probe.xyz, state);
}
