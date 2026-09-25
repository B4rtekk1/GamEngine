import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;
[[vk::binding(5, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> rayData;
[[vk::binding(6, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> irradianceAtlas;
[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(11, 1)]] RWStructuredBuffer<DDGIProbeState> probeStates;
[[vk::binding(12, 1)]] StructuredBuffer<uint> probeUpdateList;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ddgiUpdateCount(ddgi)) return;
    const uint probeIndex = probeUpdateList[id.x];
    const int2 pixel = int2(ddgiProbePixel(probeIndex));
    const float4 probe = probeData[pixel];
    DDGIProbeState state = probeStates[probeIndex];
    if (probe.w < 0.5) {
        state.state = 0u;
        state.variability = 0.0;
    } else {
        const float3 previous = irradianceAtlas[int2(ddgiAtlasBase(probeIndex, DDGI_IRRADIANCE_INTERIOR) + 5u)].rgb;
        float3 current = 0.0;
        for (uint rayIndex = 0u; rayIndex < uint(ddgi.scrollOffsetRays.w); ++rayIndex)
            current += rayData[int2(rayIndex, probeIndex)].rgb;
        current /= float(ddgi.scrollOffsetRays.w);
        const float change = dot(abs(current - previous / 3.14159265), float3(0.2126, 0.7152, 0.0722));
        state.variability = lerp(state.variability, saturate(change), 0.25);
        state.state = 1u;
    }
    probeStates[probeIndex] = state;
}
