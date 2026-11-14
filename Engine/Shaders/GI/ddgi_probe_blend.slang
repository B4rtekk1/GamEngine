// One workgroup per updated probe, fused into a single dispatch: the probe's
// rays are loaded into shared memory once, then the 8x8 irradiance and 16x16
// mean/mean-squared distance octahedra are integrated with hysteresis, the
// probe is classified, and the one-texel octahedral borders that make
// bilinear filtering seamless are written from the same shared results.
import Common.DdgiCommon;

[[vk::binding(0, 0)]] ConstantBuffer<DdgiVolume> volume;
[[vk::binding(1, 0)]] RWTexture2D<float4> rayData;
[[vk::binding(2, 0)]] StructuredBuffer<uint> probeList;
[[vk::binding(3, 0)]] RWStructuredBuffer<int4> probeData;
[[vk::binding(4, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> irradianceAtlas;
[[vk::binding(5, 0), vk::image_format("rg16f")]] RWTexture2D<float2> distanceAtlas;

static const uint MaxRays = 256u;
static const uint IrradianceTexels = 8u, DistanceTexels = 16u;
static const int ProbeValid = 1, ProbeInactive = 2, ProbeReset = 4, ProbeSleeping = 8;
// Classified as buried but still fading out; traced at the normal cadence until
// its weight reaches zero and it becomes inactive.
static const int ProbeDeactivating = 16;
// A fresh estimate this far from the cached irradiance (relative to the
// probe's brightness) means the lighting changed; it restarts the convergence
// count. It sits well above the Monte Carlo noise of one probe update.
static const float DisturbedChange = 0.5;
// A probe whose rays mostly see back faces sits inside geometry. Two
// thresholds keep a probe near the boundary from toggling as its random ray
// rotation changes; a toggling probe makes nearby shading jump.
static const float DeactivateBackfaceRatio = 0.35, ReactivateBackfaceRatio = 0.15;

groupshared float4 rayRadianceDistance[MaxRays];
groupshared float3 rayDirections[MaxRays];
groupshared uint backfaceCount;
// Largest relative irradiance change of any texel, as float bits.
groupshared uint largestChange;
// The probe weight after this update, from the irradiance texels (all equal).
groupshared float finalProbeWeight;
groupshared float4 irradianceResult[IrradianceTexels * IrradianceTexels];
groupshared float2 distanceResult[DistanceTexels * DistanceTexels];

// Interior source (1-based) of an octahedral border texel of an n x n tile.
int2 borderSource(int2 texel, int n) {
    const int last = n + 1;
    const bool cornerX = texel.x == 0 || texel.x == last, cornerY = texel.y == 0 || texel.y == last;
    if (cornerX && cornerY) return int2(texel.x == 0 ? n : 1, texel.y == 0 ? n : 1);
    if (cornerY) return int2(last - texel.x, texel.y == 0 ? 1 : n);
    return int2(texel.x == 0 ? 1 : n, last - texel.y);
}

float3 octahedralTexelDirection(uint2 texel, uint n) {
    return ddgiOctDecode((float2(texel) + 0.5) / float(n) * 2.0 - 1.0);
}

float luminance(float3 color) { return dot(color, float3(0.2126, 0.7152, 0.0722)); }

[numthreads(256, 1, 1)] [shader("compute")]
void main(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID) {
    const uint lane = threadId.x;
    const uint slot = groupId.x;
    const uint probe = probeList[slot];
    const uint rayCount = min(uint(volume.rayParams.w), MaxRays);
    if (lane == 0u) {
        backfaceCount = 0u;
        largestChange = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane < rayCount) {
        const float4 ray = rayData[int2(lane, slot)];
        rayRadianceDistance[lane] = ray;
        rayDirections[lane] = ddgiRayDirection(volume, lane);
        if (ray.a < 0.0) InterlockedAdd(backfaceCount, 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    const int3 counts = volume.probeCounts.xyz;
    const uint cascade = probe / uint(volume.probeCounts.w);
    const int3 storage = ddgiStorageFromIndex(probe - cascade * uint(volume.probeCounts.w), counts);
    const int2 tile = ddgiTile(storage, counts, cascade);
    const int4 data = probeData[probe];
    const bool reset = (data.w & ProbeReset) != 0;
    const float backfaceRatio = float(backfaceCount) / float(max(rayCount, 1u));
    const bool wasInactive = (data.w & (ProbeInactive | ProbeDeactivating)) != 0 && (data.w & ProbeReset) == 0;
    const bool inactive = backfaceRatio > (wasInactive ? ReactivateBackfaceRatio : DeactivateBackfaceRatio);
    // A coarse cascade is updated 4^c times less often. Squaring the
    // hysteresis per cascade halves that lag in frames while keeping enough
    // updates in the average to hide the per-update noise.
    const float hysteresis = reset ? 0.0 : pow(volume.rayParams.y, float(1u << cascade));
    const float maxDistance = ddgiMaxRayDistance(volume, cascade);

    if (lane < IrradianceTexels * IrradianceTexels) {
        const uint2 texel = uint2(lane % IrradianceTexels, lane / IrradianceTexels);
        const float3 direction = octahedralTexelDirection(texel, IrradianceTexels);
        float3 sum = 0.0;
        float weightSum = 0.0;
        for (uint ray = 0u; ray < rayCount; ++ray) {
            const float4 sample = rayRadianceDistance[ray];
            if (sample.a < 0.0) continue;
            const float weight = max(dot(direction, rayDirections[ray]), 0.0);
            sum += sample.rgb * weight;
            weightSum += weight;
        }
        // The cosine-weighted mean over the hemisphere equals E / pi, the
        // units Forward uses for the irradiance map.
        const float3 current = weightSum > 1e-4 ? sum / weightSum : 0.0;
        const int2 atlasTexel = tile * int(IrradianceTexels + 2u) + int2(texel) + 1;
        const float3 previous = irradianceAtlas[atlasTexel].rgb;
        float blend = hysteresis;
        // Large changes (a light switched on, a door opened) adapt quickly;
        // single bright outliers are limited so they cannot flash the probe.
        float3 delta = current - previous;
        if (max(abs(delta.x), max(abs(delta.y), abs(delta.z))) > 0.25) blend = max(blend - 0.15, 0.0);
        const float deltaLuminance = luminance(delta);
        if (!reset && deltaLuminance > 1.0) delta *= 1.0 / deltaLuminance;
        const float3 result = previous + delta * (1.0 - blend);
        const float relativeChange = luminance(abs(current - previous)) / max(luminance(previous), 0.05);
        InterlockedMax(largestChange, asuint(relativeChange));
        // The probe weight fades with the irradiance hysteresis instead of
        // switching, so (de)activation never pops.
        const float previousWeight = reset ? (inactive ? 0.0 : 1.0) : irradianceAtlas[atlasTexel].a;
        const float probeWeight = lerp(inactive ? 0.0 : 1.0, previousWeight, hysteresis);
        irradianceResult[lane] = float4(max(result, 0.0), probeWeight < 0.01 ? 0.0 : probeWeight);
        if (lane == 0u) finalProbeWeight = irradianceResult[lane].a;
        irradianceAtlas[atlasTexel] = irradianceResult[lane];
    }
    {
        const uint2 texel = uint2(lane % DistanceTexels, lane / DistanceTexels);
        const float3 direction = octahedralTexelDirection(texel, DistanceTexels);
        float2 sum = 0.0;
        float weightSum = 0.0;
        for (uint ray = 0u; ray < rayCount; ++ray) {
            const float weight = pow(max(dot(direction, rayDirections[ray]), 0.0), volume.rayParams.z);
            const float distance = min(abs(rayRadianceDistance[ray].a), maxDistance);
            sum += float2(distance, distance * distance) * weight;
            weightSum += weight;
        }
        const float2 current = weightSum > 1e-4 ? sum / weightSum : float2(maxDistance, maxDistance * maxDistance);
        const int2 atlasTexel = tile * int(DistanceTexels + 2u) + int2(texel) + 1;
        const float2 result = lerp(current, distanceAtlas[atlasTexel], hysteresis);
        distanceResult[lane] = result;
        distanceAtlas[atlasTexel] = result;
    }
    GroupMemoryBarrierWithGroupSync();

    // Borders: 36 irradiance and 68 distance texels per probe.
    const uint irradianceBorder = (IrradianceTexels + 2u) * (IrradianceTexels + 2u);
    for (uint index = lane; index < irradianceBorder; index += 256u) {
        const int2 texel = int2(index % (IrradianceTexels + 2u), index / (IrradianceTexels + 2u));
        if (all(texel >= 1) && all(texel <= int(IrradianceTexels))) continue;
        const int2 source = borderSource(texel, int(IrradianceTexels)) - 1;
        irradianceAtlas[tile * int(IrradianceTexels + 2u) + texel] =
            irradianceResult[source.y * int(IrradianceTexels) + source.x];
    }
    const uint distanceBorder = (DistanceTexels + 2u) * (DistanceTexels + 2u);
    for (uint index = lane; index < distanceBorder; index += 256u) {
        const int2 texel = int2(index % (DistanceTexels + 2u), index / (DistanceTexels + 2u));
        if (all(texel >= 1) && all(texel <= int(DistanceTexels))) continue;
        const int2 source = borderSource(texel, int(DistanceTexels)) - 1;
        distanceAtlas[tile * int(DistanceTexels + 2u) + texel] = distanceResult[source.y * int(DistanceTexels) + source.x];
    }
    if (lane == 0u) {
        // Hysteresis converges geometrically, so after wake.z undisturbed
        // updates the probe holds as good an answer as more rays would give.
        const bool disturbed = reset || asfloat(largestChange) > DisturbedChange;
        const uint updates = disturbed ? 0u : min(uint((data.w >> 8) & 0xff) + 1u, 255u);
        // The squared hysteresis converges in half as many updates.
        const bool sleeping = volume.wake.z != 0u && updates >= max(volume.wake.z >> cascade, 1u);
        const bool faded = finalProbeWeight <= 0.0;
        const int state = !inactive ? 0 : (faded ? ProbeInactive : ProbeDeactivating);
        probeData[probe] = int4(data.xyz, ProbeValid | state | (sleeping ? ProbeSleeping : 0) | int(updates << 8));
    }
}
