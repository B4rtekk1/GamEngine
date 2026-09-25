import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;

[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(10, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> fixedRayData;
[[vk::binding(12, 1)]] RWStructuredBuffer<DDGIProbeUpdate> probeUpdateList;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ddgiUpdateCount(ddgi)) return;
    const uint probeIndex = probeUpdateList[id.x].probeIndex;
    if (probeIndex >= DDGI_PROBES) return;
    const int2 pixel = int2(ddgiProbePixel(probeIndex));
    const float4 previous = ddgi.distanceInitialized.y > 0.5
        ? probeData[pixel] : float4(0.0, 0.0, 0.0, 2.0);
    uint backfaces = 0u;
    int closestBackfaceIndex = -1;
    int closestFrontfaceIndex = -1;
    int farthestFrontfaceIndex = -1;
    float closestBackfaceDistance = ddgi.distanceInitialized.x;
    float closestFrontfaceDistance = ddgi.distanceInitialized.x;
    float farthestFrontfaceDistance = 0.0;
    for (uint rayIndex = 0u; rayIndex < DDGI_FIXED_RAYS; ++rayIndex) {
        const float distance = fixedRayData[int2(rayIndex, id.x)].r;
        if (distance < 0.0) {
            ++backfaces;
            // Fixed-ray data stores the full signed hit distance.
            if (-distance < closestBackfaceDistance) {
                closestBackfaceDistance = -distance;
                closestBackfaceIndex = int(rayIndex);
            }
        } else {
            if (distance < closestFrontfaceDistance) {
                closestFrontfaceDistance = distance;
                closestFrontfaceIndex = int(rayIndex);
            }
            if (distance > farthestFrontfaceDistance) {
                farthestFrontfaceDistance = distance;
                farthestFrontfaceIndex = int(rayIndex);
            }
        }
    }
    float3 offset = previous.xyz;
    const bool insideGeometry = float(backfaces) / float(DDGI_FIXED_RAYS) > DDGI_BACKFACE_THRESHOLD;
    const float minFrontfaceDistance = ddgi.originSpacing.w * 0.25;
    if (insideGeometry && closestBackfaceIndex >= 0) {
        offset += ddgiFixedRayDirection(uint(closestBackfaceIndex)) *
            (closestBackfaceDistance + minFrontfaceDistance * 0.5);
    } else if (closestFrontfaceIndex >= 0 &&
               closestFrontfaceDistance < minFrontfaceDistance &&
               farthestFrontfaceIndex >= 0) {
        const float3 closestDirection = ddgiFixedRayDirection(uint(closestFrontfaceIndex));
        const float3 farthestDirection = ddgiFixedRayDirection(uint(farthestFrontfaceIndex));
        if (dot(closestDirection, farthestDirection) <= 0.0)
            offset += farthestDirection * min(farthestFrontfaceDistance, 1.0);
    } else if (closestFrontfaceDistance > minFrontfaceDistance) {
        // Restore the grid position once the nearby obstruction has gone.
        const float offsetLength = length(offset);
        if (offsetLength > 1e-6) {
            const float moveBack = min(closestFrontfaceDistance - minFrontfaceDistance,
                                       offsetLength);
            offset -= offset * (moveBack / offsetLength);
        }
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
    // Only a successful atlas update may clear the pending-history state.
    const float state = moved || newlyExposed || previous.w > 1.5 ? 2.0 :
                        (previous.w > 0.5 ? 1.0 : 0.0);
    probeData[pixel] = float4(offset, state);
    // Reuse this frame's validation rays only if their origin did not change.
    // Do not use the 0.02 history-reset threshold here: smaller moves also
    // change the distances needed by classification.
    probeUpdateList[id.x].relocated = any(offset != previous.xyz) ? 1u : 0u;
}
