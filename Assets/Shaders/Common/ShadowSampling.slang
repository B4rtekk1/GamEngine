static const float MIN_SHADOW_BIAS = 0.0002;
static const float SLOPE_SHADOW_BIAS = 0.003;
static const uint SHADOW_CLIP_LEVELS = 4;
static const uint VIRTUAL_PAGES_PER_AXIS = 32;
static const uint PHYSICAL_PAGES_PER_AXIS = 16;
static const uint PAGE_RESOLUTION = 256;
static const uint VIRTUAL_RESOLUTION = VIRTUAL_PAGES_PER_AXIS * PAGE_RESOLUTION;
static const uint INVALID_SHADOW_PAGE = 0xffffffff;

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
                               StructuredBuffer<uint4> pageTable, out bool resident,
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
    const float2 virtualPixel = uv * float(VIRTUAL_RESOLUTION);
    // virtualShadowAtlasUv resolves a texel centre for page lookup. Reapply
    // this offset when sampling so hardware linear filtering retains motion
    // within that texel.
    const float2 subTexelOffset = frac(virtualPixel) - 0.5;
    const uint2 centerPixel = uint2(floor(virtualPixel));
    const uint2 localPixel = centerPixel % PAGE_RESOLUTION;
    const float atlasResolution = float(PHYSICAL_PAGES_PER_AXIS * PAGE_RESOLUTION);

    if (level >= 2)
    {
        // Nearly all kernels stay inside the central virtual page. Resolve it
        // once and use cheap atlas offsets; resolve per tap only at a border.
        const bool kernelFitsCenterPage = all(localPixel >= uint2(1, 1)) &&
                                          all(localPixel < uint2(PAGE_RESOLUTION - 1,
                                                                  PAGE_RESOLUTION - 1));
        float filteredShadow = 0.0;
        [unroll]
        for (int y = 0; y < 2; ++y)
        {
            [unroll]
            for (int x = 0; x < 2; ++x)
            {
                const float2 tapOffset = float2(x, y) - 0.5;
                float2 tapAtlasUv;
                if (kernelFitsCenterPage)
                    tapAtlasUv = centerAtlasUv +
                                 (tapOffset + subTexelOffset) / atlasResolution;
                else
                {
                    const float2 tapVirtualPixel = virtualPixel + tapOffset;
                    if (virtualShadowAtlasUv(tapVirtualPixel, level, pageTable, tapAtlasUv))
                        tapAtlasUv += (frac(tapVirtualPixel) - 0.5) / atlasResolution;
                    else
                        tapAtlasUv = centerAtlasUv + subTexelOffset / atlasResolution;
                }
                filteredShadow += 1.0 - shadowMap.SampleCmpLevelZero(
                    tapAtlasUv, currentDepth - bias);
            }
        }
        return filteredShadow * 0.25;
    }

    // Levels 0 and 1 use a 3x3 tent. Keep the virtual-page-aware slow path
    // for the one-texel border, but do not pay for it in the common case.
    static const float kernel[3] = {1.0, 2.0, 1.0};
    const bool kernelFitsCenterPage = all(localPixel >= uint2(1, 1)) &&
                                      all(localPixel < uint2(PAGE_RESOLUTION - 1,
                                                              PAGE_RESOLUTION - 1));
    float filteredShadow = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float weight = kernel[x + 1] * kernel[y + 1];
            float2 tapAtlasUv;
            if (kernelFitsCenterPage)
                tapAtlasUv = centerAtlasUv +
                             (float2(x, y) + subTexelOffset) / atlasResolution;
            else
            {
                const float2 tapVirtualPixel = virtualPixel + float2(x, y);
                if (virtualShadowAtlasUv(tapVirtualPixel, level, pageTable, tapAtlasUv))
                    tapAtlasUv += (frac(tapVirtualPixel) - 0.5) / atlasResolution;
                else
                    tapAtlasUv = centerAtlasUv + subTexelOffset / atlasResolution;
            }
            filteredShadow += (1.0 - shadowMap.SampleCmpLevelZero(
                tapAtlasUv, currentDepth - bias)) * weight;
            totalWeight += weight;
        }
    }
    return filteredShadow / totalWeight;
}

float sampleVirtualShadow(Sampler2DShadow shadowMap, float3 worldPosition,
                          float4x4 clipMatrices[4], StructuredBuffer<uint4> pageTable)
{
    bool resident;
    [unroll]
    for (uint level = 0; level < SHADOW_CLIP_LEVELS; ++level)
    {
        float clipEdge;
        float shadow = sampleVirtualShadowLevel(shadowMap, worldPosition,
                                                 clipMatrices[level], level,
                                                 pageTable, resident, clipEdge);
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
                    pageTable, nextResident, nextClipEdge);
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
