import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;
[[vk::binding(18, 1)]] RWStructuredBuffer<float4> rayDirections;

groupshared float4 sharedRotation;

[numthreads(256, 1, 1)]
void main(uint groupIndex : SV_GroupIndex) {
    const uint rayCount = min(ddgiRandomRayCount(ddgi, 0u), DDGI_MAX_RAYS);
    // A partial cascade update must not give neighboring probes different
    // frame-dependent ray estimators during trilinear interpolation.
    if (groupIndex == 0u)
        sharedRotation = ddgiRayRotation(0u, rayCount);
    GroupMemoryBarrierWithGroupSync();
    if (groupIndex < rayCount)
        rayDirections[groupIndex] = float4(
            ddgiRayDirection(groupIndex, rayCount, sharedRotation), 0.0);
}
