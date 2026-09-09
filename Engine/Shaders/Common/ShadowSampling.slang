static const float MIN_SHADOW_BIAS = 0.0002;
static const float SLOPE_SHADOW_BIAS = 0.003;
static const uint SHADOW_CLIP_LEVELS = 4;
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

float sampleVirtualShadowLevel(Sampler2DShadow shadowMap, float3 worldPosition,
                               float4x4 lightSpace, uint level,
                               StructuredBuffer<uint4> pageTable, uint shadowQuality, out bool resident,
                               out float clipEdge)
{
    float4 lightSpacePosition = mul(float4(worldPosition, 1.0), lightSpace);
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
    float bias = max(MIN_SHADOW_BIAS, SLOPE_SHADOW_BIAS * depthSlope);
    // Keep a single comparison exclusively for the deliberately lowest
    // settings.  A single coarse-clipmap texel spans centimetres in world
    // space, so using it at High creates visible stair-steps after a page
    // fallback.
    const bool useSingleTap = shadowQuality == SHADOW_QUALITY_LOW ||
                              (shadowQuality == SHADOW_QUALITY_MEDIUM && level >= 2) ||
                              (shadowQuality == SHADOW_QUALITY_HIGH && level >= 3);
    if (useSingleTap)
        return 1.0 - shadowMap.SampleCmpLevelZero(centerAtlasUv, currentDepth - bias);

    const float2 virtualPixel = uv * float(VIRTUAL_RESOLUTION);
    // The physical atlas deliberately has a nearest sampler: adjacent tiles
    // can represent unrelated virtual pages. Every filtered tap must therefore
    // resolve its own virtual texel before sampling the atlas.
    const bool useSmoothTent4x4 = shadowQuality == SHADOW_QUALITY_ULTRA && level < 2;
    const bool useTent3x3 = !useSmoothTent4x4 &&
                             ((shadowQuality == SHADOW_QUALITY_HIGH && level < 2) ||
                              (shadowQuality == SHADOW_QUALITY_ULTRA && level == 2));

    if (useSmoothTent4x4)
    {
        // Page-aware equivalent of a bilinearly filtered 3x3 tent.  Unlike
        // repeated nearest samples, the weights change continuously as the
        // receiver moves inside a virtual texel.
        const float2 p = virtualPixel - 0.5;
        const int2 baseTexel = int2(floor(p));
        const float2 f = frac(p);
        const float weightsX[4] = {1.0 - f.x, 2.0 - f.x, 1.0 + f.x, f.x};
        const float weightsY[4] = {1.0 - f.y, 2.0 - f.y, 1.0 + f.y, f.y};
        float filteredShadow = 0.0;
        float totalWeight = 0.0;
        [unroll]
        for (int y = 0; y < 4; ++y)
        {
            [unroll]
            for (int x = 0; x < 4; ++x)
            {
                float2 tapAtlasUv;
                const int2 texel = baseTexel + int2(x - 1, y - 1);
                if (!resolveVirtualShadowTexel(texel, level, pageTable, tapAtlasUv))
                    continue;
                const float weight = weightsX[x] * weightsY[y];
                filteredShadow += (1.0 - shadowMap.SampleCmpLevelZero(
                    tapAtlasUv, currentDepth - bias)) * weight;
                totalWeight += weight;
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
                          float4x4 clipMatrices[4], StructuredBuffer<uint4> pageTable,
                          uint shadowQuality)
{
    bool resident;
    [unroll]
    for (uint level = 0; level < SHADOW_CLIP_LEVELS; ++level)
    {
        float clipEdge;
        float shadow = sampleVirtualShadowLevel(shadowMap, worldPosition,
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
                    shadowMap, worldPosition, clipMatrices[level + 1], level + 1,
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
