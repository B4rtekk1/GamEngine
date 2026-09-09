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

// Resolves one virtual texel to the centre of its physical atlas texel.
bool resolveVirtualShadowTexel(int2 texel, uint level,
                               StructuredBuffer<uint4> pageTable, out float2 atlasUv)
{
    atlasUv = 0.0;
    if (any(texel < int2(0)) || any(texel >= int2(VIRTUAL_RESOLUTION)))
        return false;
    uint2 pixel = uint2(texel);
    uint2 page = pixel / PAGE_RESOLUTION;
    uint tableIndex = level * VIRTUAL_PAGES_PER_AXIS * VIRTUAL_PAGES_PER_AXIS +
                      page.y * VIRTUAL_PAGES_PER_AXIS + page.x;
    uint physical = pageTable[tableIndex >> 2][tableIndex & 3];
    if (physical == INVALID_SHADOW_PAGE)
        return false;
    uint2 physicalPage = uint2(physical % PHYSICAL_PAGES_PER_AXIS,
                               physical / PHYSICAL_PAGES_PER_AXIS);
    uint2 localPixel = pixel % PAGE_RESOLUTION;
    float2 atlasPixel = float2(physicalPage * PAGE_RESOLUTION + localPixel) + 0.5;
    atlasUv = atlasPixel / float(PHYSICAL_PAGES_PER_AXIS * PAGE_RESOLUTION);
    return true;
}

bool virtualShadowAtlasUv(float2 virtualPixel, uint level,
                          StructuredBuffer<uint4> pageTable, out float2 atlasUv)
{
    return resolveVirtualShadowTexel(int2(floor(virtualPixel)), level, pageTable, atlasUv);
}

float shadowWorldTexelSize(float4x4 lightSpace)
{
    // Orthographic clip matrices scale the light-space X/Y axes by 1/extent.
    // Reading that scale keeps the receiver bias correct for the dynamic far
    // clipmap as well as the fixed near levels.
    float lightSpaceScale = max(length(lightSpace[0].xyz), 1.0e-6);
    return 2.0 / (float(VIRTUAL_RESOLUTION) * lightSpaceScale);
}

float sampleVirtualShadowLevel(Sampler2DShadow shadowMap, float3 worldPosition,
                               float3 normal, float3 lightDirection,
                               float4x4 lightSpace, uint level,
                               StructuredBuffer<uint4> pageTable, uint shadowQuality, out bool resident,
                               out float clipEdge)
{
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

    const float2 virtualPixel = uv * float(VIRTUAL_RESOLUTION);
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
        // Spend the samples nearest the receiver, but keep the footprint
        // below one virtual texel. This gives a soft, continuous PCF edge
        // rather than sparse coverage over a large disk.
        const uint tapCount = level == 0 ? 12u : (level == 1 ? 8u : 4u);
        const float radius = level == 0 ? 0.90 : (level == 1 ? 0.75 : 0.65);
        float filteredShadow = 0.0;
        float totalWeight = 0.0;
        [loop]
        for (uint tap = 0; tap < tapCount; ++tap)
        {
            float2 tapAtlasUv;
            const int2 texel = int2(floor(virtualPixel + poisson[tap] * radius));
            if (!resolveVirtualShadowTexel(texel, level, pageTable, tapAtlasUv))
                continue;
            filteredShadow += 1.0 - shadowMap.SampleCmpLevelZero(tapAtlasUv, currentDepth - bias);
            totalWeight += 1.0;
        }
        return totalWeight > 0.0 ? filteredShadow / totalWeight : 0.0;
    }

    // Medium remains deliberately inexpensive; High uses 2x2 at L2.
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
                float2 tapAtlasUv;
                const int2 texel = int2(floor(virtualPixel)) + int2(x, y);
                if (!resolveVirtualShadowTexel(texel, level, pageTable, tapAtlasUv)) continue;
                filteredShadow += 1.0 - shadowMap.SampleCmpLevelZero(tapAtlasUv, currentDepth - bias);
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
            float2 tapAtlasUv;
            const int2 texel = int2(floor(virtualPixel)) + int2(x, y);
            if (!resolveVirtualShadowTexel(texel, level, pageTable, tapAtlasUv))
                continue;
            filteredShadow += (1.0 - shadowMap.SampleCmpLevelZero(
                tapAtlasUv, currentDepth - bias)) * weight;
            totalWeight += weight;
        }
    }
    return totalWeight > 0.0 ? filteredShadow / totalWeight : 0.0;
}

float sampleVirtualShadow(Sampler2DShadow shadowMap, float3 worldPosition,
                          float3 normal, float3 lightDirection,
                          float4x4 clipMatrices[7], StructuredBuffer<uint4> pageTable,
                          uint shadowQuality)
{
    bool resident;
    [unroll]
    for (uint level = 0; level < SHADOW_CLIP_LEVELS; ++level)
    {
        float clipEdge;
        float shadow = sampleVirtualShadowLevel(shadowMap, worldPosition, normal, lightDirection,
                                                 clipMatrices[level], level,
                                                 pageTable, shadowQuality, resident, clipEdge);
        if (resident)
        {
            // Fade through the outer fifth of the clipmap. Do not blend with
            // an absent next-level page: it is safer to keep the valid level.
            if (level + 1 < SHADOW_CLIP_LEVELS && clipEdge > 0.75)
            {
                bool nextResident;
                float nextClipEdge;
                float nextShadow = sampleVirtualShadowLevel(
                    shadowMap, worldPosition, normal, lightDirection, clipMatrices[level + 1], level + 1,
                    pageTable, shadowQuality, nextResident, nextClipEdge);
                if (nextResident)
                    shadow = lerp(shadow, nextShadow, smoothstep(0.75, 0.95, clipEdge));
            }
            return shadow;
        }
    }
    return 0.0;
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
