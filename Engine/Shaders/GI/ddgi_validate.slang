import GI.ddgi_common;
import GI.ddgi_alpha;
[[vk::push_constant]] DDGIConstants ddgi;

[[vk::binding(0, 1)]] RaytracingAccelerationStructure sceneTlas;
[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(10, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> fixedRayData;
[[vk::binding(12, 1)]] StructuredBuffer<DDGIProbeUpdate> probeUpdateList;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= DDGI_FIXED_RAYS || id.y >= ddgiUpdateCount(ddgi)) return;
    const uint probeIndex = probeUpdateList[id.y].probeIndex;
    const float4 probe = ddgi.distanceInitialized.y > 0.5
        ? probeData[int2(ddgiProbePixel(probeIndex))] : float4(0.0, 0.0, 0.0, 2.0);
    RayDesc ray;
    ray.Origin = ddgiPosition(probeIndex, ddgi.originSpacing.xyz, ddgi.originSpacing.w, ddgi.scrollOffsetRays.xyz) + probe.xyz;
    ray.Direction = ddgiFixedRayDirection(id.x);
    ray.TMin = 0.02;
    ray.TMax = ddgi.distanceInitialized.x;
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(sceneTlas, RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
                         0x02, ray);
    while (query.Proceed()) {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            ddgiCandidatePassesAlpha(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(),
                                     query.CandidateTriangleBarycentrics()))
            query.CommitNonOpaqueTriangleHit();
    }
    float distance = ddgi.distanceInitialized.x;
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        const bool realBackface = !query.CommittedTriangleFrontFace() &&
            !ddgiHitIsDoubleSided(query.CommittedInstanceID(), query.CommittedPrimitiveIndex());
        distance = query.CommittedRayT() * (realBackface ? -1.0 : 1.0);
    }
    fixedRayData[int2(id.x, probeIndex)] = float4(distance, 0.0, 0.0, 0.0);
}
