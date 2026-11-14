// Dynamic diffuse global illumination (Majercik et al. 2019) probe volume.
// Probes sit on the integer world grid `g * spacing`. The volume covers
// gridMin .. gridMin + probeCounts - 1 and scrolls with the camera in whole
// probe steps; storage is toroidal, so probe `g` always lives at
// posMod(g, probeCounts) and a scroll only invalidates the entering planes.
//
// Several cascades share the probe counts at growing spacing, so the far
// field keeps probe lighting instead of meeting a visible edge where the
// finest volume ends. Cascade c owns probes [c * probeCounts.w, (c + 1) *
// probeCounts.w) and the atlas tile rows below the previous cascade's.
static const uint DdgiMaxCascades = 3u;

struct DdgiVolume
{
    // xyz: world grid coordinate of cascade 0's lowest probe, w: 1 when the atlases hold valid data.
    int4 gridMin;
    // xyz: probes per axis, w: probes per cascade.
    int4 probeCounts;
    // x: cascade 0 probe spacing, y: normal bias, z: view bias (both in units of
    // the cascade's spacing), w: intensity.
    float4 spacingBias;
    // x: max ray distance, y: hysteresis, z: distance weight exponent, w: rays per probe.
    float4 rayParams;
    // x: probe tiles per atlas row, y: irradiance interior texels, z: distance interior texels, w: frame index.
    uint4 atlasInfo;
    // xy: irradiance atlas size, zw: distance atlas size, in texels.
    float4 atlasSize;
    // Rows of this frame's random ray rotation.
    float4 rayRotation[3];
    // Probe cache. x: 1 wakes every probe this frame, y: wake box count,
    // z: updates after which an undisturbed probe sleeps (0 disables sleeping).
    uint4 wake;
    // Min/max corner pairs of regions whose probes must be re-traced.
    float4 wakeBoxes[2 * 16];
    // Per cascade. xyz: world grid coordinate of the lowest probe, w: spacing as float bits.
    int4 cascadeGrid[DdgiMaxCascades];
    // x: cascade count.
    uint4 cascades;
};

uint ddgiCascadeCount(DdgiVolume volume)
{
    return min(volume.cascades.x, DdgiMaxCascades);
}

float ddgiCascadeSpacing(DdgiVolume volume, uint cascade)
{
    return asfloat(volume.cascadeGrid[cascade].w);
}

// Rays reach the same number of probe spacings in every cascade.
float ddgiMaxRayDistance(DdgiVolume volume, uint cascade)
{
    return volume.rayParams.x * ddgiCascadeSpacing(volume, cascade) / volume.spacingBias.x;
}

// The irradiance alpha channel holds the probe's shading weight: zero while a
// probe is inactive (buried in geometry) or not yet traced.
static const float DdgiPi = 3.14159265359;

// Probe counts are powers of two (DdgiPass asserts it), so the positive
// modulo is a mask: two's complement makes it correct for negative grid
// coordinates too, without the cost of signed integer division.
int3 ddgiPosMod(int3 value, int3 modulus)
{
    return value & (modulus - 1);
}

uint ddgiProbeIndex(int3 storage, int3 counts)
{
    return uint(storage.x + counts.x * (storage.y + counts.y * storage.z));
}

int3 ddgiStorageFromIndex(uint index, int3 counts)
{
    return int3(int(index) % counts.x, (int(index) / counts.x) % counts.y, int(index) / (counts.x * counts.y));
}

// The world grid coordinate held by a storage slot for the cascade's current position.
int3 ddgiGridFromStorage(DdgiVolume volume, uint cascade, int3 storage)
{
    const int3 counts = volume.probeCounts.xyz;
    const int3 gridMin = volume.cascadeGrid[cascade].xyz;
    return gridMin + ddgiPosMod(storage - ddgiPosMod(gridMin, counts), counts);
}

int2 ddgiTile(int3 storage, int3 counts, uint cascade)
{
    return int2(storage.x + storage.y * counts.x, storage.z + int(cascade) * counts.z);
}

float2 ddgiSignNotZero(float2 v)
{
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

float2 ddgiOctEncode(float3 direction)
{
    float3 n = direction / (abs(direction.x) + abs(direction.y) + abs(direction.z));
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * ddgiSignNotZero(n.xy);
}

float3 ddgiOctDecode(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * ddgiSignNotZero(n.xy);
    return normalize(n);
}

float2 ddgiAtlasUv(int3 storage, uint cascade, float3 direction, int3 counts, uint interior, float2 atlasSize)
{
    const float tileSize = float(interior + 2u);
    const float2 inTile = (ddgiOctEncode(direction) * 0.5 + 0.5) * float(interior) + 1.0;
    return (float2(ddgiTile(storage, counts, cascade)) * tileSize + inTile) / atlasSize;
}

float3 ddgiSphericalFibonacci(float index, float count)
{
    const float goldenRatioFraction = 0.61803398875;
    const float phi = 2.0 * DdgiPi * frac(index * goldenRatioFraction);
    const float cosTheta = 1.0 - (2.0 * index + 1.0) / count;
    const float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float3 ddgiRayDirection(DdgiVolume volume, uint ray)
{
    const float3 direction = ddgiSphericalFibonacci(float(ray), volume.rayParams.w);
    return normalize(float3(dot(volume.rayRotation[0].xyz, direction), dot(volume.rayRotation[1].xyz, direction),
                            dot(volume.rayRotation[2].xyz, direction)));
}

// Probes of fade at a cascade's border. Inside a 16-probe-wide cascade the
// fully weighted region still spans 10 probes.
static const float DdgiCascadeFadeProbes = 3.0;

float3 ddgiBiasedPosition(DdgiVolume volume, float spacing, float3 position, float3 normal, float3 toCamera)
{
    return position + (normal * volume.spacingBias.y + toCamera * volume.spacingBias.z) * spacing;
}

// Fade over the outer probes; beyond them the next cascade (or the caller's
// fallback) takes over. A wide fade hides the cascade change.
float ddgiBorderFade(DdgiVolume volume, uint cascade, float3 gridPosition)
{
    const int3 gridMin = volume.cascadeGrid[cascade].xyz;
    const int3 gridMax = gridMin + volume.probeCounts.xyz - 1;
    const float3 inside = min(gridPosition - float3(gridMin), float3(gridMax) - gridPosition);
    return smoothstep(0.0, DdgiCascadeFadeProbes, min(inside.x, min(inside.y, inside.z)));
}

float ddgiCascadeFade(DdgiVolume volume, uint cascade, float3 position, float3 normal, float3 toCamera)
{
    const float spacing = ddgiCascadeSpacing(volume, cascade);
    return ddgiBorderFade(volume, cascade, ddgiBiasedPosition(volume, spacing, position, normal, toCamera) / spacing);
}

float3 ddgiCascadeIrradiance(DdgiVolume volume, uint cascade, Sampler2D irradianceAtlas, Sampler2D distanceAtlas,
                             float3 position, float3 normal, float3 toCamera, out float weight)
{
    weight = 0.0;
    const float spacing = ddgiCascadeSpacing(volume, cascade);
    const int3 counts = volume.probeCounts.xyz;
    const float3 biasedPosition = ddgiBiasedPosition(volume, spacing, position, normal, toCamera);
    const float3 gridPosition = biasedPosition / spacing;
    const float3 baseGrid = floor(gridPosition);
    const float3 alpha = gridPosition - baseGrid;
    const int3 gridMin = volume.cascadeGrid[cascade].xyz, gridMax = gridMin + counts - 1;
    const float borderFade = ddgiBorderFade(volume, cascade, gridPosition);
    if (borderFade <= 0.0)
        return 0.0;

    float3 sum = 0.0;
    float weightSum = 0.0;
    [unroll] for (uint corner = 0u; corner < 8u; ++corner)
    {
        const int3 offset = int3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
        const int3 grid = clamp(int3(baseGrid) + offset, gridMin, gridMax);
        const int3 storage = ddgiPosMod(grid, counts);
        const float3 probePosition = float3(grid) * spacing;
        const float3 trilinear = lerp(1.0 - alpha, alpha, float3(offset));
        float probeWeight = 1.0;
        // Smooth backface test: probes behind the surface contribute little.
        const float3 toProbe = normalize(probePosition - position);
        const float wrap = (dot(toProbe, normal) + 1.0) * 0.5;
        probeWeight *= wrap * wrap + 0.2;
        // Chebyshev visibility against the probe's mean and mean-squared hit distance.
        const float3 probeToPoint = biasedPosition - probePosition;
        const float distanceToProbe = max(length(probeToPoint), 1e-4);
        const float2 moments = distanceAtlas.SampleLevel(
            ddgiAtlasUv(storage, cascade, probeToPoint / distanceToProbe, counts, volume.atlasInfo.z, volume.atlasSize.zw), 0.0).rg;
        if (distanceToProbe > moments.x)
        {
            const float variance = abs(moments.x * moments.x - moments.y);
            const float delta = distanceToProbe - moments.x;
            const float chebyshev = variance / (variance + delta * delta);
            probeWeight *= max(chebyshev * chebyshev * chebyshev, 0.0);
        }
        probeWeight = max(probeWeight, 1e-6);
        // Crush tiny weights so leaking probes cannot dominate a dark corner.
        const float crushThreshold = 0.2;
        if (probeWeight < crushThreshold)
            probeWeight *= probeWeight * probeWeight / (crushThreshold * crushThreshold);
        probeWeight *= trilinear.x * trilinear.y * trilinear.z;
        const float4 irradiance = irradianceAtlas.SampleLevel(
            ddgiAtlasUv(storage, cascade, normal, counts, volume.atlasInfo.y, volume.atlasSize.xy), 0.0);
        probeWeight *= irradiance.a;
        sum += irradiance.rgb * probeWeight;
        weightSum += probeWeight;
    }
    if (weightSum <= 1e-5)
        return 0.0;
    weight = borderFade;
    return sum / weightSum;
}

// Interleaved gradient noise, advanced by the golden ratio each frame so a
// temporal resolve sees every dither phase of a pixel.
float ddgiScreenDither(uint2 pixel, uint frameIndex)
{
    const float ign = frac(52.9829189 * frac(dot(float2(pixel), float2(0.06711056, 0.00583715))));
    return frac(ign + float(frameIndex % 64u) * 0.61803398875);
}

// Irradiance (in the units of the IBL irradiance map: albedo multiplies it
// directly) at a surface. `toCamera` points from the surface to the viewer.
// `weight` fades to zero beyond the coarsest cascade and where no probe is
// valid, so callers blend to their fallback with it.
//
// With `dither` in [0, 1) exactly one cascade is evaluated: it is chosen with
// the probability the blend would give it, and a temporal resolve (TAA)
// averages the choice. A negative `dither` blends the cascades instead.
float3 ddgiIrradiance(DdgiVolume volume, Sampler2D irradianceAtlas, Sampler2D distanceAtlas, float3 position,
                      float3 normal, float3 toCamera, float dither, out float weight)
{
    weight = 0.0;
    if (volume.gridMin.w == 0)
        return 0.0;
    const uint cascadeCount = ddgiCascadeCount(volume);
    if (dither >= 0.0)
    {
        // Walk the cascades finest first, renormalising the uniform sample
        // after each rejected cascade. The coarsest cascade fades to the
        // sky smoothly instead: probe and sky light differ far more than two
        // cascades do, and dithering between them would show as noise.
        float u = dither;
        for (uint cascade = 0u; cascade < cascadeCount; ++cascade)
        {
            const bool coarsest = cascade + 1u == cascadeCount;
            const float fade = coarsest ? 1.0 : ddgiCascadeFade(volume, cascade, position, normal, toCamera);
            if (u < fade)
            {
                float cascadeWeight;
                const float3 irradiance = ddgiCascadeIrradiance(volume, cascade, irradianceAtlas, distanceAtlas,
                                                                position, normal, toCamera, cascadeWeight);
                weight = coarsest ? cascadeWeight : (cascadeWeight > 0.0 ? 1.0 : 0.0);
                return irradiance * volume.spacingBias.w;
            }
            u = (u - fade) / max(1.0 - fade, 1e-4);
        }
        return 0.0;
    }
    // Finest first: each cascade covers what its finer neighbours leave.
    float3 sum = 0.0;
    float remaining = 1.0;
    for (uint cascade = 0u; cascade < cascadeCount && remaining > 0.002; ++cascade)
    {
        float cascadeWeight;
        const float3 irradiance = ddgiCascadeIrradiance(volume, cascade, irradianceAtlas, distanceAtlas, position,
                                                        normal, toCamera, cascadeWeight);
        sum += irradiance * cascadeWeight * remaining;
        remaining *= 1.0 - cascadeWeight;
    }
    weight = 1.0 - remaining;
    return weight > 1e-5 ? sum / weight * volume.spacingBias.w : 0.0;
}
