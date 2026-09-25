static const uint3 DDGI_COUNTS = uint3(16u, 8u, 16u);
static const uint DDGI_FIXED_RAYS = 32u;
static const float DDGI_BACKFACE_THRESHOLD = 0.25;
static const float DDGI_RANDOM_BACKFACE_THRESHOLD = 0.1;
static const uint DDGI_PROBES = 2048u;
static const uint DDGI_UPDATES_PER_FRAME = 256u;
static const uint DDGI_MAX_RAYS = 256u;
static const uint DDGI_MIN_CONVERGENCE_UPDATES = 64u;
static const uint DDGI_STABLE_UPDATES = 8u;
// Temporary result written by irradiance blending and consumed by finish.
static const uint DDGI_ACCEPTED_UPDATE_STATE = 3u;
static const float DDGI_VARIABILITY_THRESHOLD = 0.005;
static const float DDGI_HYSTERESIS = 0.97;
// Active probeData.w runs from 1.4 toward 1.0 after a pending probe is retraced.
static const float DDGI_REACTIVATION_STATE = 1.4;
static const float DDGI_REACTIVATION_STEP = 0.1;
static const float DDGI_IRRADIANCE_ENCODING_GAMMA = 5.0;
// The atlas stores gamma-encoded HDR irradiance in normalized 8-bit channels.
static const float DDGI_IRRADIANCE_ENCODING_RANGE = 256.0;
static const float DDGI_IRRADIANCE_CHANGE_THRESHOLD = 0.25;
static const float DDGI_BRIGHTNESS_THRESHOLD = 0.10;
// RTXGI tile sizes include the one-texel border on every side.
static const uint DDGI_IRRADIANCE_INTERIOR = 6u;
static const uint DDGI_DISTANCE_INTERIOR = 14u;

struct DDGIConstants {
    float4 originSpacing;
    int4 scrollOffsetRays;
    int4 scrollDeltaFrame;
    float4 distanceInitialized;
    float4 updateControl;
};
uint ddgiUpdateCount(DDGIConstants constants) {
    return min(uint(constants.distanceInitialized.z), DDGI_UPDATES_PER_FRAME);
}
float3 ddgiEncodeIrradiance(float3 irradiance) {
    return pow(saturate(irradiance / DDGI_IRRADIANCE_ENCODING_RANGE),
               1.0 / DDGI_IRRADIANCE_ENCODING_GAMMA);
}
float3 ddgiDecodeIrradiance(float3 encodedIrradiance) {
    return DDGI_IRRADIANCE_ENCODING_RANGE *
           pow(saturate(encodedIrradiance), DDGI_IRRADIANCE_ENCODING_GAMMA);
}

struct DDGIProbeState {
    uint state;
    uint lastUpdatedFrame;
    float variability;
    uint updateCount;
    uint stableUpdates;
};

struct DDGIProbeUpdate {
    uint probeIndex;
    uint framesSinceLastUpdate;
    uint relocated;
    uint padding;
    uint4 localLightIndices;
};
// raysPerProbe includes validation at the old and, when moved, new position.
uint ddgiRandomRayCount(DDGIConstants constants, uint relocated) {
    return uint(constants.scrollOffsetRays.w) - DDGI_FIXED_RAYS * (1u + min(relocated, 1u));
}
uint ddgiRandomDirectionIndex(uint rayIndex, uint rayCount, uint maxRandomRays) {
    // Spread the smaller relocated set over the same spherical sequence.
    return rayIndex * maxRandomRays / rayCount;
}

uint3 ddgiGrid(uint probeIndex) {
    return uint3(probeIndex % DDGI_COUNTS.x,
                 (probeIndex / DDGI_COUNTS.x) % DDGI_COUNTS.y,
                 probeIndex / (DDGI_COUNTS.x * DDGI_COUNTS.y));
}
uint3 ddgiLogicalGrid(uint probeIndex, int3 scrollOffset) {
    return uint3((int3(ddgiGrid(probeIndex)) - scrollOffset + int3(DDGI_COUNTS)) % int3(DDGI_COUNTS));
}
uint ddgiPhysicalIndex(uint3 logicalGrid, int3 scrollOffset) {
    const uint3 physical = uint3((int3(logicalGrid) + scrollOffset) % int3(DDGI_COUNTS));
    return (physical.z * DDGI_COUNTS.y + physical.y) * DDGI_COUNTS.x + physical.x;
}
float3 ddgiPosition(uint probeIndex, float3 origin, float spacing, int3 scrollOffset) {
    return origin + float3(ddgiLogicalGrid(probeIndex, scrollOffset)) * spacing;
}
float2 ddgiOctEncode(float3 direction) {
    direction /= abs(direction.x) + abs(direction.y) + abs(direction.z);
    float2 uv = direction.xy;
    if (direction.z < 0.0)
        uv = (1.0 - abs(uv.yx)) * select(uv >= 0.0, 1.0, -1.0);
    return uv;
}
float3 ddgiOctDecode(float2 uv) {
    float3 direction = float3(uv, 1.0 - abs(uv.x) - abs(uv.y));
    if (direction.z < 0.0)
        direction.xy = (1.0 - abs(direction.yx)) * select(direction.xy >= 0.0, 1.0, -1.0);
    return normalize(direction);
}
uint ddgiHash(uint value) {
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}
float4 ddgiRayRotation(uint frameIndex, uint rayCount) {
    const uint seed = frameIndex ^ (rayCount * 0x9e3779b9u);
    const float scale = 1.0 / 4294967296.0;
    const float u1 = (float(ddgiHash(seed ^ 0xa511e9b3u)) + 0.5) * scale;
    const float u2 = (float(ddgiHash(seed ^ 0x63d83595u)) + 0.5) * scale;
    const float u3 = (float(ddgiHash(seed ^ 0x8d22f6e6u)) + 0.5) * scale;
    const float a = 6.2831853 * u2;
    const float b = 6.2831853 * u3;
    const float r1 = sqrt(1.0 - u1);
    const float r2 = sqrt(u1);
    return float4(r1 * sin(a), r1 * cos(a), r2 * sin(b), r2 * cos(b));
}
float3 ddgiRayDirection(uint rayIndex, uint rayCount, float4 rotation) {
    const float z = 1.0 - 2.0 * (float(rayIndex) + 0.5) / float(rayCount);
    const float angle = float(rayIndex) * 2.39996323;
    const float radius = sqrt(max(1.0 - z * z, 0.0));
    const float3 baseDirection = float3(cos(angle) * radius, sin(angle) * radius, z);
    const float3 qv = rotation.xyz;
    const float3 t = 2.0 * cross(qv, baseDirection);
    return baseDirection + rotation.w * t + cross(qv, t);
}
float3 ddgiFixedRayDirection(uint rayIndex) {
    const float z = 1.0 - 2.0 * (float(rayIndex) + 0.5) / float(DDGI_FIXED_RAYS);
    const float angle = float(rayIndex) * 2.39996323;
    const float radius = sqrt(max(1.0 - z * z, 0.0));
    return float3(cos(angle) * radius, sin(angle) * radius, z);
}
uint2 ddgiProbePixel(uint probeIndex) {
    return uint2(probeIndex % 16u, probeIndex / 16u);
}
uint2 ddgiAtlasBase(uint probeIndex, uint interior) {
    const uint stride = interior + 2u;
    return uint2((probeIndex % 16u) * stride, (probeIndex / 16u) * stride);
}

uint2 ddgiBorderSource(uint2 texel, uint interior) {
    const uint last = interior + 1u;
    const bool xBorder = texel.x == 0u || texel.x == last;
    const bool yBorder = texel.y == 0u || texel.y == last;
    if (xBorder && yBorder)
        return uint2(texel.x == 0u ? interior : 1u,
                     texel.y == 0u ? interior : 1u);
    if (yBorder)
        return uint2(last - texel.x, texel.y == 0u ? 1u : interior);
    return uint2(texel.x == 0u ? 1u : interior, last - texel.y);
}

// Region-cache record: valid, five scheduling words, half4 placement,
// 64 packed RGBA8 irradiance texels (including borders), 256 half2 distance texels.
static const uint DDGI_CACHE_PROBE_WORDS = 328u;
uint ddgiPackRgba8(float4 value) {
    const uint4 bytes = uint4(round(saturate(value) * 255.0));
    return bytes.x | (bytes.y << 8u) | (bytes.z << 16u) | (bytes.w << 24u);
}
float4 ddgiUnpackRgba8(uint value) {
    return float4(value & 255u, (value >> 8u) & 255u,
                  (value >> 16u) & 255u, value >> 24u) / 255.0;
}
uint ddgiPackHalf2(float2 value) {
    return f32tof16(value.x) | (f32tof16(value.y) << 16u);
}
float2 ddgiUnpackHalf2(uint value) {
    return float2(f16tof32(value & 65535u), f16tof32(value >> 16u));
}
