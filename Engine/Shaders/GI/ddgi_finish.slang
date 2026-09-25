import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;
[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(11, 1)]] RWStructuredBuffer<DDGIProbeState> probeStates;
[[vk::binding(12, 1)]] StructuredBuffer<DDGIProbeUpdate> probeUpdateList;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ddgiUpdateCount(ddgi)) return;
    const uint probeIndex = probeUpdateList[id.x].probeIndex;
    const int2 pixel = int2(ddgiProbePixel(probeIndex));
    const float4 probe = probeData[pixel];
    DDGIProbeState state = probeStates[probeIndex];
    state.state = probe.w < 0.5 ? 0u : 1u;
    state.variability = 0.0;
    probeStates[probeIndex] = state;
}
