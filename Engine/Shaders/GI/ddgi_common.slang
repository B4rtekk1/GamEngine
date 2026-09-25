static const uint3 DDGI_COUNTS = uint3(16u, 8u, 16u);
static const uint DDGI_FIXED_RAYS = 32u;
static const float DDGI_BACKFACE_THRESHOLD = 0.25;
static const float DDGI_RANDOM_BACKFACE_THRESHOLD = 0.1;
static const uint DDGI_PROBES = 2048u;
static const uint DDGI_UPDATES_PER_FRAME = 256u;
static const uint DDGI_IRRADIANCE_INTERIOR = 8u;
static const uint DDGI_DISTANCE_INTERIOR = 16u;

struct DDGIConstants {
    float4 originSpacing;
    int4 scrollOffsetRays;
    int4 scrollDeltaFrame;
    float4 distanceInitialized;
};
uint ddgiUpdateCount(DDGIConstants constants) {
    return min(uint(constants.distanceInitialized.z), DDGI_UPDATES_PER_FRAME);
}

struct DDGIProbeState {
    uint state;
    uint lastUpdatedFrame;
    float variability;
};

struct DDGIProbeUpdate {
    uint probeIndex;
    uint framesSinceLastUpdate;
};

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
bool ddgiShouldValidate(float state, uint probeIndex, uint frameIndex) {
    return state > 0.5 || frameIndex % 8u == probeIndex % 8u;
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
