static const float MIN_SHADOW_BIAS = 0.0002;
static const float SLOPE_SHADOW_BIAS = 0.003;
static const uint SHADOW_CLIP_LEVELS = 7;
static const uint VIRTUAL_PAGES_PER_AXIS = 32;
static const uint PHYSICAL_PAGES_PER_AXIS = 16;
static const uint PAGE_RESOLUTION = 256;
static const uint VIRTUAL_RESOLUTION = VIRTUAL_PAGES_PER_AXIS * PAGE_RESOLUTION;
static const uint INVALID_SHADOW_PAGE = 0xffffffff;
static const uint SHADOW_QUALITY_LOW = 0;
static const uint SHADOW_QUALITY_MEDIUM = 1;
static const uint SHADOW_QUALITY_HIGH = 2;
static const uint SHADOW_QUALITY_ULTRA = 3;
static const uint SHADOW_DEBUG_OFF = 0;
static const uint SHADOW_DEBUG_BLOCKER_COUNT = 1;
static const uint SHADOW_DEBUG_PENUMBRA_RADIUS = 2;
static const uint SHADOW_DEBUG_CLIP_LEVEL = 3;
static const uint SHADOW_DEBUG_VIRTUAL_PAGE = 4;

struct ShadowSampleDebug
{
    float blockerCount;
    float penumbraRadius;
    uint clipLevel;
    uint2 virtualPage;
};

// Resolving the page once is enough for every texel of a bilinear footprint
// that stays inside that page. This is the common case (only a two-texel-wide
// strip around each page edge needs the general resolver below), and avoids
// three redundant page-table fetches per logical PCF tap.
bool resolveVirtualShadowPage(uint2 page, uint level,
                              StructuredBuffer<uint4> pageTable, out uint physical)
{
    physical = INVALID_SHADOW_PAGE;
    if (any(page >= uint2(VIRTUAL_PAGES_PER_AXIS)))
        return false;
    const uint tableIndex = level * VIRTUAL_PAGES_PER_AXIS * VIRTUAL_PAGES_PER_AXIS +
                            page.y * VIRTUAL_PAGES_PER_AXIS + page.x;
    physical = pageTable[tableIndex >> 2][tableIndex & 3];
    return physical != INVALID_SHADOW_PAGE;
}

float2 virtualTexelAtlasUv(uint2 texel, uint physical)
{
    const uint2 physicalPage = uint2(physical % PHYSICAL_PAGES_PER_AXIS,
                                     physical / PHYSICAL_PAGES_PER_AXIS);
    const uint2 localPixel = texel % PAGE_RESOLUTION;
    return (float2(physicalPage * PAGE_RESOLUTION + localPixel) + 0.5) /
           float(PHYSICAL_PAGES_PER_AXIS * PAGE_RESOLUTION);
}

// Resolves one virtual texel to the centre of its physical atlas texel.
bool resolveVirtualShadowTexel(int2 texel, uint level,
                               StructuredBuffer<uint4> pageTable, out float2 atlasUv)
{
    atlasUv = 0.0;
    if (any(texel < int2(0)) || any(texel >= int2(VIRTUAL_RESOLUTION)))
        return false;
    uint2 pixel = uint2(texel);
    uint physical;
    if (!resolveVirtualShadowPage(pixel / PAGE_RESOLUTION, level, pageTable, physical))
        return false;
    atlasUv = virtualTexelAtlasUv(pixel, physical);
    return true;
}

bool virtualShadowAtlasUv(float2 virtualPixel, uint level,
                          StructuredBuffer<uint4> pageTable, out float2 atlasUv)
{
    return resolveVirtualShadowTexel(int2(floor(virtualPixel)), level, pageTable, atlasUv);
}

// The atlas sampler is intentionally nearest-filtered: filtering it directly
// would blend unrelated physical pages.  Reconstruct a bilinear sample in
// *virtual* texel space instead, resolving each of the four texels separately.
// A missing neighbour is omitted and the remaining weights are normalized.  A
// resident centre texel therefore remains usable at a cache boundary rather
// than forcing an abrupt clipmap fallback.
float sampleVirtualShadowBilinear(Sampler2DShadow shadowMap, float2 virtualPixel,
                                  uint level, StructuredBuffer<uint4> pageTable,
                                  float compareDepth, out bool hasSamples)
{
    const float2 texelSpace = virtualPixel - 0.5;
    const int2 base = int2(floor(texelSpace));
    const float2 fraction = frac(texelSpace);
    const float weights[4] = {
        (1.0 - fraction.x) * (1.0 - fraction.y),
        fraction.x * (1.0 - fraction.y),
        (1.0 - fraction.x) * fraction.y,
        fraction.x * fraction.y};
    const int2 offsets[4] = {int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1)};

    // Fast path: the entire bilinear footprint is resident in one virtual
    // page. Map that page once, then sample its physical texels directly.
    // The atlas sampler remains nearest-filtered, so this preserves virtual
    // bilinear filtering exactly without bleeding across physical pages.
    const bool footprintInPage = (base.x % int(PAGE_RESOLUTION)) != int(PAGE_RESOLUTION - 1) &&
                                 (base.y % int(PAGE_RESOLUTION)) != int(PAGE_RESOLUTION - 1);
    if (all(base >= int2(0)) && all(base + int2(1) < int2(VIRTUAL_RESOLUTION)) && footprintInPage)
    {
        uint physical;
        if (resolveVirtualShadowPage(uint2(base) / PAGE_RESOLUTION, level, pageTable, physical))
        {
            float shadow = 0.0;
            [unroll]
            for (uint tap = 0; tap < 4; ++tap)
            {
                const float2 atlasUv = virtualTexelAtlasUv(uint2(base + offsets[tap]), physical);
                shadow += (1.0 - shadowMap.SampleCmpLevelZero(atlasUv, compareDepth)) * weights[tap];
            }
            hasSamples = true;
            return shadow;
        }
    }

    float shadow = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (uint tap = 0; tap < 4; ++tap)
    {
        float2 atlasUv;
        if (!resolveVirtualShadowTexel(base + offsets[tap], level, pageTable, atlasUv))
            continue;
        shadow += (1.0 - shadowMap.SampleCmpLevelZero(atlasUv, compareDepth)) * weights[tap];
        totalWeight += weights[tap];
    }
    hasSamples = totalWeight > 0.0;
    return hasSamples ? shadow / totalWeight : 0.0;
}

// Read one unfiltered virtual texel for the PCSS blocker pass. Sampling the
// physical atlas directly is safe only after this virtual-to-physical resolve.
bool sampleVirtualShadowDepth(Sampler2D shadowDepthMap, float2 virtualPixel,
                              uint level, StructuredBuffer<uint4> pageTable,
                              out float depth)
{
    float2 atlasUv;
    if (!virtualShadowAtlasUv(virtualPixel, level, pageTable, atlasUv))
    {
        depth = 1.0;
        return false;
    }
    depth = shadowDepthMap.SampleLevel(atlasUv, 0.0).x;
    return true;
}

float shadowWorldTexelSize(float4x4 lightSpace)
{
    // Orthographic clip matrices scale the light-space X/Y axes by 1/extent.
    // Reading that scale keeps the receiver bias correct for the dynamic far
    // clipmap as well as the fixed near levels.
    float lightSpaceScale = max(length(lightSpace[0].xyz), 1.0e-6);
    return 2.0 / (float(VIRTUAL_RESOLUTION) * lightSpaceScale);
}

float sampleVirtualShadowLevel(Sampler2DShadow shadowMap, Sampler2D shadowDepthMap,
                               float3 worldPosition,
                               float3 normal, float3 lightDirection,
                               float4x4 lightSpace, uint level,
                               StructuredBuffer<uint4> pageTable, uint shadowQuality, out bool resident,
                               out float clipEdge, out float blockerCount,
                               out float penumbraRadius, out uint2 virtualPage)
{
    blockerCount = 0.0;
    penumbraRadius = 0.0;
    virtualPage = 0;
    const float NoL = saturate(dot(normalize(normal), normalize(lightDirection)));
    const float normalBias = shadowWorldTexelSize(lightSpace) * lerp(0.5, 2.0, 1.0 - NoL);
    const float3 shadowPosition = worldPosition + normalize(normal) * normalBias;
    float4 lightSpacePosition = mul(float4(shadowPosition, 1.0), lightSpace);
    float3 projected = lightSpacePosition.xyz / lightSpacePosition.w;
    float2 uv = projected.xy * 0.5 + 0.5;
    float currentDepth = projected.z;
    clipEdge = max(abs(projected.x), abs(projected.y));
    float2 centerAtlasUv = 0.0;
    resident = currentDepth > 0.0 && currentDepth < 1.0 &&
               virtualShadowAtlasUv(uv * VIRTUAL_RESOLUTION, level,
                                    pageTable, centerAtlasUv);
    if (!resident)
        return 0.0;

    const float2 virtualPixel = uv * float(VIRTUAL_RESOLUTION);
    virtualPage = uint2(virtualPixel) / PAGE_RESOLUTION;

    float depthSlope = max(abs(ddx(currentDepth)), abs(ddy(currentDepth)));
    float bias = max(MIN_SHADOW_BIAS, SLOPE_SHADOW_BIAS * depthSlope * 0.25);
    // Keep a single comparison exclusively for the deliberately lowest
    // settings.  A single coarse-clipmap texel spans centimetres in world
    // space, so using it at High creates visible stair-steps after a page
    // fallback.
    const bool useSingleTap = shadowQuality == SHADOW_QUALITY_LOW ||
                              (shadowQuality == SHADOW_QUALITY_MEDIUM && level >= 2);
    if (useSingleTap)
        return 1.0 - shadowMap.SampleCmpLevelZero(centerAtlasUv, currentDepth - bias);

    // The physical atlas deliberately has a nearest sampler: adjacent tiles
    // can represent unrelated virtual pages. Every filtered tap must therefore
    // resolve its own virtual texel before sampling the atlas.
    const bool useStablePoisson = shadowQuality == SHADOW_QUALITY_ULTRA;
    const bool useTent3x3 = shadowQuality == SHADOW_QUALITY_HIGH && level < 2;

    // Keep this kernel fixed. A world-position rotation makes a single Scene
    // View frame read as a noisy dither, and must not be enabled unless the
    // pass is guaranteed to receive a shadow-aware temporal accumulation.
    static const float2 poisson[16] = {
        float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
        float2(-0.09418410, -0.92938870), float2(0.34495938, 0.29387760),
        float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
        float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
        float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420),
        float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
        float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590),
        float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100790)};

    if (useStablePoisson)
    {
        // PCSS needs a receiver-dependent search region too: a finite light
        // subtends a larger cone at receivers farther along the light axis.
        // The first four samples cover the centre. If all four are blockers,
        // the receiver is inside a stable solid shadow and the outer ring is
        // skipped; silhouette and foliage pixels receive all eight samples.
        const float lightDepthScale = max(length(lightSpace[2].xyz), 1.0e-6);
        const float sunAngularRadius = 0.00465;
        const float blockerSearchRadius = clamp(
            tan(sunAngularRadius) * max(currentDepth, 0.02) /
            (lightDepthScale * shadowWorldTexelSize(lightSpace)), 0.75, 4.0);
        static const float2 blockerOffsets[8] = {
            float2(0.0, 0.0), float2(-0.45, -0.35),
            float2(0.45, -0.35), float2(0.0, 0.48),
            float2(-0.92, -0.62), float2(0.88, -0.58),
            float2(-0.67, 0.82), float2(0.72, 0.76)};
        float blockerDepthSum = 0.0;
        [unroll]
        for (uint blockerTap = 0; blockerTap < 4; ++blockerTap)
        {
            float blockerDepth;
            if (sampleVirtualShadowDepth(shadowDepthMap,
                                         virtualPixel + blockerOffsets[blockerTap] * blockerSearchRadius,
                                         level, pageTable, blockerDepth) &&
                blockerDepth < currentDepth - bias)
            {
                blockerDepthSum += blockerDepth;
                blockerCount += 1.0;
            }
        }
        if (blockerCount < 4.0)
        {
            [unroll]
            for (uint blockerTap = 4; blockerTap < 8; ++blockerTap)
            {
                float blockerDepth;
                if (sampleVirtualShadowDepth(shadowDepthMap,
                                             virtualPixel + blockerOffsets[blockerTap] * blockerSearchRadius,
                                             level, pageTable, blockerDepth) &&
                    blockerDepth < currentDepth - bias)
                {
                    blockerDepthSum += blockerDepth;
                    blockerCount += 1.0;
                }
            }
        }
        if (blockerCount == 0.0)
            return 0.0;

        // Convert the receiver/blocker separation from projected depth to a
        // world-space distance, then to virtual texels.  0.00465 rad is the
        // Sun's angular radius; clamping prevents a distant occluder from
        // turning a page miss or a thin object into an impractically broad
        // filter.  This is intentionally a PCSS approximation, not SMRT.
        const float averageBlockerDepth = blockerDepthSum / blockerCount;
        const float receiverBlockerDistance =
            max(currentDepth - averageBlockerDepth, 0.0) / lightDepthScale;
        penumbraRadius = clamp(
            tan(sunAngularRadius) * receiverBlockerDistance /
            shadowWorldTexelSize(lightSpace), 0.25, 6.0);

        // Scale the compare budget with the visible penumbra. At contact the
        // four-tap path is cheaper than the previous fixed twelve samples;
        // only broad penumbrae use the complete stable Poisson kernel.
        const uint tapCount = penumbraRadius < 1.5 ? 4u :
                              (penumbraRadius < 3.5 ? 8u : 12u);
        float filteredShadow = 0.0;
        float totalWeight = 0.0;
        [loop]
        for (uint tap = 0; tap < tapCount; ++tap)
        {
            bool tapResident;
            const float tapShadow = sampleVirtualShadowBilinear(
                shadowMap, virtualPixel + poisson[tap] * penumbraRadius, level, pageTable,
                currentDepth - bias, tapResident);
            if (!tapResident)
                continue;
            filteredShadow += tapShadow;
            totalWeight += 1.0;
        }
        return totalWeight > 0.0 ? filteredShadow / totalWeight : 0.0;
    }

    // Medium remains deliberately inexpensive. Centre its four continuous
    // bilinear samples on the receiver; using offsets 0..1 shifted the whole
    // filter half a virtual texel towards +X/+Y.
    if (!useTent3x3)
    {
        float filteredShadow = 0.0;
        float totalWeight = 0.0;
        [unroll]
        for (int y = 0; y < 2; ++y)
        {
            [unroll]
            for (int x = 0; x < 2; ++x)
            {
                bool tapResident;
                const float tapShadow = sampleVirtualShadowBilinear(
                    shadowMap, virtualPixel + float2(x - 0.5, y - 0.5), level, pageTable,
                    currentDepth - bias, tapResident);
                if (!tapResident) continue;
                filteredShadow += tapShadow;
                totalWeight += 1.0;
            }
        }
        return totalWeight > 0.0 ? filteredShadow / totalWeight : 0.0;
    }

    static const float kernel[3] = {1.0, 2.0, 1.0};
    float filteredShadow = 0.0;
    float totalWeight = 0.0;
    const int firstTap = useTent3x3 ? -1 : 0;
    const int lastTap = 1;
    [loop]
    for (int y = firstTap; y <= lastTap; ++y)
    {
        [loop]
        for (int x = firstTap; x <= lastTap; ++x)
        {
            const float weight = useTent3x3 ? kernel[x + 1] * kernel[y + 1] : 1.0;
            bool tapResident;
            const float tapShadow = sampleVirtualShadowBilinear(
                shadowMap, virtualPixel + float2(x, y), level, pageTable,
                currentDepth - bias, tapResident);
            if (!tapResident)
                continue;
            filteredShadow += tapShadow * weight;
            totalWeight += weight;
        }
    }
    return totalWeight > 0.0 ? filteredShadow / totalWeight : 0.0;
}

float sampleVirtualShadow(Sampler2DShadow shadowMap, Sampler2D shadowDepthMap,
                          float3 worldPosition,
                          float3 normal, float3 lightDirection,
                          float4x4 clipMatrices[7], StructuredBuffer<uint4> pageTable,
                          uint shadowQuality, out ShadowSampleDebug debug)
{
    debug.blockerCount = 0.0;
    debug.penumbraRadius = 0.0;
    debug.clipLevel = 0;
    debug.virtualPage = 0;
    bool resident;
    [unroll]
    for (uint level = 0; level < SHADOW_CLIP_LEVELS; ++level)
    {
        float clipEdge;
        float blockerCount;
        float penumbraRadius;
        uint2 virtualPage;
        float shadow = sampleVirtualShadowLevel(
            shadowMap, shadowDepthMap, worldPosition, normal, lightDirection,
            clipMatrices[level], level, pageTable, shadowQuality, resident, clipEdge,
            blockerCount, penumbraRadius, virtualPage);
        if (resident)
        {
            debug.blockerCount = blockerCount;
            debug.penumbraRadius = penumbraRadius;
            debug.clipLevel = level;
            debug.virtualPage = virtualPage;
            // Fade through the outer fifth of the clipmap. Do not blend with
            // an absent next-level page: it is safer to keep the valid level.
            if (level + 1 < SHADOW_CLIP_LEVELS && clipEdge > 0.75)
            {
                bool nextResident;
                float nextClipEdge;
                float nextBlockerCount;
                float nextPenumbraRadius;
                uint2 nextVirtualPage;
                float nextShadow = sampleVirtualShadowLevel(
                    shadowMap, shadowDepthMap, worldPosition, normal, lightDirection, clipMatrices[level + 1], level + 1,
                    pageTable, shadowQuality, nextResident, nextClipEdge,
                    nextBlockerCount, nextPenumbraRadius, nextVirtualPage);
                if (nextResident)
                    shadow = lerp(shadow, nextShadow, smoothstep(0.75, 0.95, clipEdge));
            }
            return shadow;
        }
    }
    return 0.0;
}

float3 shadowDebugColor(uint view, ShadowSampleDebug debug)
{
    if (view == SHADOW_DEBUG_BLOCKER_COUNT)
        return saturate(debug.blockerCount / 8.0).xxx;
    if (view == SHADOW_DEBUG_PENUMBRA_RADIUS)
    {
        const float t = saturate(debug.penumbraRadius / 6.0);
        return float3(t, 0.15 + 0.85 * (1.0 - abs(2.0 * t - 1.0)), 1.0 - t);
    }
    if (view == SHADOW_DEBUG_CLIP_LEVEL)
    {
        static const float3 colors[7] = {
            float3(0.90, 0.20, 0.20), float3(0.95, 0.60, 0.15), float3(0.90, 0.90, 0.20),
            float3(0.20, 0.80, 0.30), float3(0.15, 0.70, 0.90), float3(0.30, 0.35, 0.95),
            float3(0.75, 0.25, 0.90)};
        return colors[min(debug.clipLevel, 6u)];
    }
    // A deterministic page hash makes virtual-page boundaries and residency
    // churn visible without exposing physical atlas placement.
    uint hash = debug.virtualPage.x * 1664525u + debug.virtualPage.y * 1013904223u +
                debug.clipLevel * 2246822519u;
    return float3(float(hash & 255u), float((hash >> 8u) & 255u),
                  float((hash >> 16u) & 255u)) / 255.0;
}

// Compatibility path for the standalone sample shader.
float samplePcfShadow(Sampler2DShadow shadowMap, float4 lightSpacePosition)
{
    float3 projected = lightSpacePosition.xyz / lightSpacePosition.w;
    float2 uv = projected.xy * 0.5 + 0.5;
    float currentDepth = projected.z;
    if (any(uv <= 0.0) || any(uv >= 1.0) || currentDepth <= 0.0 || currentDepth >= 1.0)
        return 0.0;
    float bias = max(MIN_SHADOW_BIAS,
                     SLOPE_SHADOW_BIAS * max(abs(ddx(currentDepth)), abs(ddy(currentDepth))));
    return 1.0 - shadowMap.SampleCmpLevelZero(uv, currentDepth - bias);
}
