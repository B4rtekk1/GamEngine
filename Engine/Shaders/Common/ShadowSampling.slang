import Common.ShadowLod;

// This is only a numerical floor.  The actual receiver bias is calculated
// from the shadow texel footprint below; a fixed 0.0002 depth offset was large
// enough to hide the slope term on nearly every receiver.
static const float MIN_SHADOW_BIAS = 1.0e-5;
static const float LEGACY_MIN_SHADOW_BIAS = 0.0002;
static const float LEGACY_SLOPE_SHADOW_BIAS = 0.003;
static const float SHADOW_TEXEL_BIAS_SCALE = 2.0;
// A small world-space separation prevents the receiver from intersecting its
// own rasterized shadow depth. Keep this modest; the receiver-plane bias below
// handles the larger grazing-angle correction.
static const float GEOMETRIC_NORMAL_OFFSET_TEXELS = 0.5;
static const uint SHADOW_CLIP_LEVELS = 7;
static const uint PHYSICAL_PAGES_PER_AXIS = 16;
static const uint INVALID_SHADOW_PAGE = 0xffffffff;
static const uint SHADOW_DEBUG_OFF = 0;
static const uint SHADOW_DEBUG_BLOCKER_COUNT = 1;
static const uint SHADOW_DEBUG_PENUMBRA_RADIUS = 2;
static const uint SHADOW_DEBUG_DESIRED_CLIP_LEVEL = 3;
static const uint SHADOW_DEBUG_RESOLVED_CLIP_LEVEL = 4;
static const uint SHADOW_DEBUG_FALLBACK_DELTA = 5;
static const uint SHADOW_DEBUG_VIRTUAL_PAGE = 6;

struct ShadowSampleDebug
{
    float blockerCount;
    float penumbraRadius;
    uint desiredClipLevel;
    uint resolvedClipLevel;
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

// Unlike virtualTexelAtlasUv(), this retains the fractional virtual-pixel
// position. It is valid for hardware bilinear PCF only after the caller has
// established that its complete 2x2 footprint is inside this physical page.
float2 virtualPixelAtlasUv(float2 virtualPixel, uint physical)
{
    const uint2 physicalPage = uint2(physical % PHYSICAL_PAGES_PER_AXIS,
                                     physical / PHYSICAL_PAGES_PER_AXIS);
    const float2 localPixel = virtualPixel - floor(virtualPixel / PAGE_RESOLUTION) * PAGE_RESOLUTION;
    return (float2(physicalPage * PAGE_RESOLUTION) + localPixel) /
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
float sampleVirtualShadowBilinear(Sampler2DShadow shadowMap, Sampler2DShadow linearShadowMap,
                                  float2 virtualPixel,
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
    // page. Its physical 2x2 footprint is therefore contiguous too, so one
    // hardware linear comparison exactly replaces four resolved comparisons.
    const bool footprintInPage = (base.x % int(PAGE_RESOLUTION)) != int(PAGE_RESOLUTION - 1) &&
                                 (base.y % int(PAGE_RESOLUTION)) != int(PAGE_RESOLUTION - 1);
    if (all(base >= int2(0)) && all(base + int2(1) < int2(VIRTUAL_RESOLUTION)) && footprintInPage)
    {
        uint physical;
        if (resolveVirtualShadowPage(uint2(base) / PAGE_RESOLUTION, level, pageTable, physical))
        {
            hasSamples = true;
            return 1.0 - linearShadowMap.SampleCmpLevelZero(
                virtualPixelAtlasUv(virtualPixel, physical), compareDepth);
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
    // Positions are transformed as row vectors below, so each clip-space
    // component is formed from a *column* of the matrix, not a row.
    const float3 clipXGradient = float3(lightSpace[0][0], lightSpace[1][0], lightSpace[2][0]);
    const float3 clipYGradient = float3(lightSpace[0][1], lightSpace[1][1], lightSpace[2][1]);
    const float lightSpaceScale = max(max(length(clipXGradient), length(clipYGradient)), 1.0e-6);
    return 2.0 / (float(VIRTUAL_RESOLUTION) * lightSpaceScale);
}

float shadowLightDepthScale(float4x4 lightSpace)
{
    const float3 clipZGradient = float3(lightSpace[0][2], lightSpace[1][2], lightSpace[2][2]);
    return max(length(clipZGradient), 1.0e-6);
}

float shadowReceiverDepthBias(float currentDepth, float2 uv, float4x4 lightSpace,
                              float3 geometricNormal, float3 lightDirection)
{
    // A receiver plane may cross several shadow texels per screen pixel, or
    // only a fraction of one.  Convert the screen-space depth derivatives to
    // depth per virtual-shadow texel, then cover the 2-texel comparison
    // footprint.  This prevents quantized shadow depths from producing acne
    // bands on large, nearly planar receivers.
    const float2 uvDx = ddx(uv);
    const float2 uvDy = ddy(uv);
    const float zDx = ddx(currentDepth);
    const float zDy = ddy(currentDepth);
    const float determinant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
    const float texelDepth = shadowWorldTexelSize(lightSpace) *
                             shadowLightDepthScale(lightSpace);
    const float noL = saturate(abs(dot(normalize(geometricNormal),
                                        normalize(lightDirection))));
    const float slope = sqrt(saturate(1.0 - noL * noL)) / max(noL, 0.05);
    const float maxBiasTexels = lerp(3.0, 8.0, saturate(slope * 0.25));
    const float maxReceiverBias = texelDepth * maxBiasTexels;
    // Invert the screen-to-shadow-UV Jacobian.  Taking independent maxima of
    // depth and UV derivatives can pair values from different screen axes and
    // severely underestimate the required bias on diagonally projected planes.
    // If the screen-to-shadow Jacobian is singular, its derivatives cannot
    // provide a stable plane estimate. Use an angle-aware fallback instead of
    // dropping to MIN_SHADOW_BIAS, which reintroduces grazing-angle acne.
    float receiverPlaneBias = maxReceiverBias;
    if (abs(determinant) > 1.0e-8)
    {
        const float dzdu = (uvDy.y * zDx - uvDx.y * zDy) / determinant;
        const float dzdv = (uvDx.x * zDy - uvDy.x * zDx) / determinant;
        const float calculatedBias = SHADOW_TEXEL_BIAS_SCALE *
                                     (abs(dzdu) + abs(dzdv)) /
                                     float(VIRTUAL_RESOLUTION);
        // A nearly singular Jacobian can turn tiny derivative noise into an
        // arbitrarily large depth offset. Never let receiver-plane bias move
        // the comparison by more than a few shadow texels.
        receiverPlaneBias = min(calculatedBias, maxReceiverBias);
    }

    // The geometric normal offset is applied in world space by the caller.
    // Keep comparison bias responsible only for receiver-plane quantization
    // and slope acne; do not add a second angle-dependent normal offset here.
    return max(MIN_SHADOW_BIAS, receiverPlaneBias);
}

float shadowKernelRotation(uint2 pixelCoordinate, uint temporalSampleIndex,
                           bool temporalShadowJitter)
{
    uint hash = pixelCoordinate.x * 1664525u + pixelCoordinate.y * 1013904223u;
    if (temporalShadowJitter)
        hash ^= temporalSampleIndex * 747796405u;
    hash = (hash ^ (hash >> 16u)) * 2246822519u;
    hash ^= hash >> 13u;
    return float(hash & 0x00ffffffu) / float(0x01000000u) * 6.28318530718;
}

float sampleVirtualShadowLevel(Sampler2DShadow shadowMap, Sampler2DShadow linearShadowMap,
                               Sampler2D shadowDepthMap,
                               float3 worldPosition,
                               float3 normal, float3 lightDirection,
                               float4x4 lightSpace, uint level,
                               StructuredBuffer<uint4> pageTable, uint shadowQuality,
                               uint2 pixelCoordinate, uint temporalSampleIndex,
                               bool temporalShadowJitter, out bool resident,
                               out float clipEdge, out float blockerCount,
                               out float penumbraRadius, out uint2 virtualPage)
{
    blockerCount = 0.0;
    penumbraRadius = 0.0;
    virtualPage = 0;
    // A small geometric separation prevents self-intersection. The receiver
    // comparison bias below handles slope/quantization independently.
    const float normalBias = shadowWorldTexelSize(lightSpace) *
                             GEOMETRIC_NORMAL_OFFSET_TEXELS;
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

    const float bias = shadowReceiverDepthBias(currentDepth, uv, lightSpace,
                                               normal, lightDirection);
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
    // The edge-only PCF16 is also useful on coarse levels, where the virtual
    // grid is otherwise most visible.
    const bool useHighEdgeFilter = shadowQuality == SHADOW_QUALITY_HIGH;

    static const float2 poisson[16] = {
        float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
        float2(-0.09418410, -0.92938870), float2(0.34495938, 0.29387760),
        float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
        float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
        float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420),
        float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
        float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590),
        float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100790)};
    const float rotation = shadowKernelRotation(pixelCoordinate, temporalSampleIndex,
                                                temporalShadowJitter);
    float rotationSin;
    float rotationCos;
    sincos(rotation, rotationSin, rotationCos);

    if (useStablePoisson)
    {
        // Classify the overwhelmingly common interior cases before entering
        // PCSS.  The four taps are the same cheap virtual bilinear probes
        // used by the HIGH path; only a mixed footprint needs blocker search
        // and the expensive Poisson filter.  This is deliberately conservative
        // around the edge so contact silhouettes retain the ULTRA quality.
        float classificationShadow = 0.0;
        uint classificationSamples = 0;
        [unroll]
        for (int y = 0; y < 2; ++y)
        {
            [unroll]
            for (int x = 0; x < 2; ++x)
            {
                bool tapResident;
                const float tapShadow = sampleVirtualShadowBilinear(
                    shadowMap, linearShadowMap,
                    virtualPixel + float2(x - 0.5, y - 0.5), level, pageTable,
                    currentDepth - bias, tapResident);
                if (!tapResident) continue;
                classificationShadow += tapShadow;
                ++classificationSamples;
            }
        }
        if (classificationSamples == 4)
        {
            classificationShadow *= 0.25;
            if (classificationShadow <= 0.001 || classificationShadow >= 0.999)
                return classificationShadow;
        }

        // PCSS needs a receiver-dependent search region too: a finite light
        // subtends a larger cone at receivers farther along the light axis.
        // The first four samples cover the centre. If all four are blockers,
        // the receiver is inside a stable solid shadow and the outer ring is
        // skipped; silhouette and foliage pixels receive all eight samples.
        const float lightDepthScale = shadowLightDepthScale(lightSpace);
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

        // Contact shadows need silhouette antialiasing too; penumbra width
        // must not reduce the comparison budget below twelve samples.
        const uint tapCount = penumbraRadius < 3.5 ? 12u : 16u;
        float filteredShadow = 0.0;
        float totalWeight = 0.0;
        [loop]
        for (uint tap = 0; tap < tapCount; ++tap)
        {
            bool tapResident;
            const float2 offset = float2(
                poisson[tap].x * rotationCos - poisson[tap].y * rotationSin,
                poisson[tap].x * rotationSin + poisson[tap].y * rotationCos);
            const float tapShadow = sampleVirtualShadowBilinear(
                shadowMap, linearShadowMap, virtualPixel + offset * penumbraRadius, level, pageTable,
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
    if (!useHighEdgeFilter)
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
                    shadowMap, linearShadowMap, virtualPixel + float2(x - 0.5, y - 0.5), level, pageTable,
                    currentDepth - bias, tapResident);
                if (!tapResident) continue;
                filteredShadow += tapShadow;
                totalWeight += 1.0;
            }
        }
        return totalWeight > 0.0 ? filteredShadow / totalWeight : 0.0;
    }

    // The four logical probes cheaply reject uniform regions. A full Poisson
    // PCF16 is paid only where the footprint crosses a shadow silhouette.
    float initialShadow = 0.0;
    uint initialTapCount = 0;
    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            bool tapResident;
            const float tapShadow = sampleVirtualShadowBilinear(
                shadowMap, linearShadowMap, virtualPixel + float2(x - 0.5, y - 0.5), level, pageTable,
                currentDepth - bias, tapResident);
            if (!tapResident) continue;
            initialShadow += tapShadow;
            ++initialTapCount;
        }
    }
    initialShadow *= 0.25;
    if (initialTapCount == 4 && (initialShadow <= 0.001 || initialShadow >= 0.999))
        return initialShadow;

    float filteredShadow = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (uint tap = 0; tap < 16; ++tap)
    {
        const float2 offset = float2(
            poisson[tap].x * rotationCos - poisson[tap].y * rotationSin,
            poisson[tap].x * rotationSin + poisson[tap].y * rotationCos);
        bool tapResident;
        const float tapShadow = sampleVirtualShadowBilinear(
            shadowMap, linearShadowMap, virtualPixel + offset * 1.25, level, pageTable,
            currentDepth - bias, tapResident);
        if (!tapResident) continue;
        filteredShadow += tapShadow;
        totalWeight += 1.0;
    }
    return totalWeight > 0.0 ? filteredShadow / totalWeight : 0.0;
}

float shadowFootprintInTexels(float3 worldPosition, float4x4 clipMatrix)
{
    const float4 lightClip = mul(float4(worldPosition, 1.0), clipMatrix);
    if (abs(lightClip.w) < 1.0e-6)
        return 0.0;
    const float2 uv = lightClip.xy / lightClip.w * 0.5 + 0.5;
    return shadowTexelFootprintFromGradients(ddx(uv), ddx(uv), ddy(uv), ddy(uv));
}

uint selectVirtualShadowLevel(float3 worldPosition, float4x4 clipMatrices[7],
                              const uint shadowQuality, out float footprintInTexels)
{
    const float threshold = shadowTexelFootprintThreshold(shadowQuality);
    footprintInTexels = 0.0;
    [unroll]
    for (uint level = 0; level < SHADOW_CLIP_LEVELS; ++level)
    {
        const float4 lightClip = mul(float4(worldPosition, 1.0), clipMatrices[level]);
        if (abs(lightClip.w) < 1.0e-6) continue;
        const float2 uv = lightClip.xy / lightClip.w * 0.5 + 0.5;
        if (any(uv < 0.0) || any(uv >= 1.0)) continue;

        // This is the same LOD rule as receiver marking, but uses both
        // screen-space axes.  Sampling must never let incidental L0 cache
        // residency override the requested geometric LOD.
        const float levelFootprint = shadowFootprintInTexels(worldPosition, clipMatrices[level]);
        if (levelFootprint <= threshold || level + 1 == SHADOW_CLIP_LEVELS)
        {
            footprintInTexels = levelFootprint;
            return level;
        }
    }
    return SHADOW_CLIP_LEVELS - 1;
}

float sampleVirtualShadow(Sampler2DShadow shadowMap, Sampler2DShadow linearShadowMap,
                          Sampler2D shadowDepthMap,
                          float3 worldPosition,
                          float3 normal, float3 lightDirection,
                          float4x4 clipMatrices[7], StructuredBuffer<uint4> pageTable,
                          uint shadowQuality, uint2 pixelCoordinate,
                          uint temporalSampleIndex, bool temporalShadowJitter,
                          out ShadowSampleDebug debug)
{
    debug.blockerCount = 0.0;
    debug.penumbraRadius = 0.0;
    debug.desiredClipLevel = 0;
    debug.resolvedClipLevel = 0;
    debug.virtualPage = 0;
    bool resident;
    float desiredFootprintInTexels;
    const uint desiredLevel = selectVirtualShadowLevel(
        worldPosition, clipMatrices, shadowQuality, desiredFootprintInTexels);
    debug.desiredClipLevel = desiredLevel;
    debug.resolvedClipLevel = desiredLevel;
    [loop]
    for (uint level = desiredLevel; level < SHADOW_CLIP_LEVELS; ++level)
    {
        float clipEdge;
        float blockerCount;
        float penumbraRadius;
        uint2 virtualPage;
        float shadow = sampleVirtualShadowLevel(
            shadowMap, linearShadowMap, shadowDepthMap, worldPosition, normal, lightDirection,
            clipMatrices[level], level, pageTable, shadowQuality, pixelCoordinate,
            temporalSampleIndex, temporalShadowJitter, resident, clipEdge,
            blockerCount, penumbraRadius, virtualPage);
        if (resident)
        {
            debug.blockerCount = blockerCount;
            debug.penumbraRadius = penumbraRadius;
            debug.resolvedClipLevel = level;
            debug.virtualPage = virtualPage;
            // Cross-fade both at the geometric clipmap edge and across the
            // screen-space footprint threshold.  The latter keeps adjacent
            // pixels from abruptly switching between independently rendered
            // clip levels, which otherwise appears as horizontal LOD seams.
            const float footprintInTexels = level == desiredLevel
                ? desiredFootprintInTexels
                : shadowFootprintInTexels(worldPosition, clipMatrices[level]);
            const float threshold = shadowTexelFootprintThreshold(shadowQuality);
            const float lodBlend = smoothstep(threshold * 0.70, threshold, footprintInTexels);
            const float clipBlend = smoothstep(0.75, 0.95, clipEdge);
            const float transition = max(lodBlend, clipBlend);
            // Do not blend with an absent next-level page: it is safer to
            // keep the valid level until its fallback becomes resident.
            if (level + 1 < SHADOW_CLIP_LEVELS && transition > 0.0)
            {
                bool nextResident;
                float nextClipEdge;
                float nextBlockerCount;
                float nextPenumbraRadius;
                uint2 nextVirtualPage;
                float nextShadow = sampleVirtualShadowLevel(
                    shadowMap, linearShadowMap, shadowDepthMap, worldPosition, normal, lightDirection, clipMatrices[level + 1], level + 1,
                    pageTable, shadowQuality, pixelCoordinate, temporalSampleIndex,
                    temporalShadowJitter, nextResident, nextClipEdge,
                    nextBlockerCount, nextPenumbraRadius, nextVirtualPage);
                if (nextResident)
                    shadow = lerp(shadow, nextShadow, transition);
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
    if (view == SHADOW_DEBUG_DESIRED_CLIP_LEVEL || view == SHADOW_DEBUG_RESOLVED_CLIP_LEVEL)
    {
        static const float3 colors[7] = {
            float3(0.90, 0.20, 0.20), float3(0.95, 0.60, 0.15), float3(0.90, 0.90, 0.20),
            float3(0.20, 0.80, 0.30), float3(0.15, 0.70, 0.90), float3(0.30, 0.35, 0.95),
            float3(0.75, 0.25, 0.90)};
        const uint level = view == SHADOW_DEBUG_DESIRED_CLIP_LEVEL
            ? debug.desiredClipLevel : debug.resolvedClipLevel;
        return colors[min(level, 6u)];
    }
    if (view == SHADOW_DEBUG_FALLBACK_DELTA)
    {
        const uint delta = max(debug.resolvedClipLevel, debug.desiredClipLevel) - debug.desiredClipLevel;
        if (delta == 0u) return float3(0.10, 0.90, 0.20);
        if (delta == 1u) return float3(1.00, 0.90, 0.10);
        if (delta == 2u) return float3(1.00, 0.45, 0.05);
        return float3(0.95, 0.05, 0.05);
    }
    // A deterministic page hash makes virtual-page boundaries and residency
    // churn visible without exposing physical atlas placement.
    uint hash = debug.virtualPage.x * 1664525u + debug.virtualPage.y * 1013904223u +
                debug.resolvedClipLevel * 2246822519u;
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
    float bias = max(LEGACY_MIN_SHADOW_BIAS,
                     LEGACY_SLOPE_SHADOW_BIAS *
                         max(abs(ddx(currentDepth)), abs(ddy(currentDepth))));
    return 1.0 - shadowMap.SampleCmpLevelZero(uv, currentDepth - bias);
}
