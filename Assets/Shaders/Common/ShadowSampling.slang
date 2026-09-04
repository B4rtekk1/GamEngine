static const float MIN_SHADOW_BIAS = 0.0002;
static const float SLOPE_SHADOW_BIAS = 0.003;
static const uint SHADOW_CLIP_LEVELS = 4;
static const uint VIRTUAL_PAGES_PER_AXIS = 32;
static const uint PHYSICAL_PAGES_PER_AXIS = 16;
static const uint PAGE_RESOLUTION = 256;
static const uint VIRTUAL_RESOLUTION = VIRTUAL_PAGES_PER_AXIS * PAGE_RESOLUTION;
static const uint INVALID_SHADOW_PAGE = 0xffffffff;

bool virtualShadowAtlasUv(float2 virtualPixel, uint level,
                          StructuredBuffer<uint4> pageTable, out float2 atlasUv)
{
    atlasUv = 0.0;
    int2 pixel = int2(floor(virtualPixel));
    if (any(pixel < int2(0)) || any(pixel >= int2(VIRTUAL_RESOLUTION)))
        return false;
    uint2 page = uint2(pixel) / PAGE_RESOLUTION;
    uint tableIndex = level * VIRTUAL_PAGES_PER_AXIS * VIRTUAL_PAGES_PER_AXIS +
                      page.y * VIRTUAL_PAGES_PER_AXIS + page.x;
    uint physical = pageTable[tableIndex >> 2][tableIndex & 3];
    if (physical == INVALID_SHADOW_PAGE)
        return false;
    uint2 physicalPage = uint2(physical % PHYSICAL_PAGES_PER_AXIS,
                               physical / PHYSICAL_PAGES_PER_AXIS);
    uint2 localPixel = uint2(pixel) % PAGE_RESOLUTION;
    float2 atlasPixel = float2(physicalPage * PAGE_RESOLUTION + localPixel) + 0.5;
    atlasUv = atlasPixel / float(PHYSICAL_PAGES_PER_AXIS * PAGE_RESOLUTION);
    return true;
}

float sampleVirtualShadowLevel(Sampler2DShadow shadowMap, float3 worldPosition,
                               float4x4 lightSpace, uint level,
                               StructuredBuffer<uint4> pageTable, out bool resident)
{
    float4 lightSpacePosition = mul(float4(worldPosition, 1.0), lightSpace);
    float3 projected = lightSpacePosition.xyz / lightSpacePosition.w;
    float2 uv = projected.xy * 0.5 + 0.5;
    float currentDepth = projected.z;
    float2 centerAtlasUv = 0.0;
    resident = currentDepth > 0.0 && currentDepth < 1.0 &&
               virtualShadowAtlasUv(uv * VIRTUAL_RESOLUTION, level,
                                    pageTable, centerAtlasUv);
    if (!resident)
        return 0.0;

    float depthSlope = max(abs(ddx(currentDepth)), abs(ddy(currentDepth)));
    float bias = max(MIN_SHADOW_BIAS, SLOPE_SHADOW_BIAS * depthSlope);
    // A single comparison lookup uses only hardware 2x2 PCF. The near clip
    // levels get a 3x3 tent; distant levels use a 2x2 filter and a single
    // comparison respectively. TAA integrates the resulting low-frequency
    // noise while avoiding a 25-tap cost at every shaded pixel.
    const float2 virtualPixel = uv * float(VIRTUAL_RESOLUTION);
    const uint2 centerPixel = uint2(floor(virtualPixel));
    const uint2 localPixel = centerPixel % PAGE_RESOLUTION;
    const float atlasResolution = float(PHYSICAL_PAGES_PER_AXIS * PAGE_RESOLUTION);

    if (level == 3)
        return 1.0 - shadowMap.SampleCmpLevelZero(centerAtlasUv, currentDepth - bias);

    if (level == 2)
    {
        // Centre the four taps around the projected pixel. The one-pixel
        // border uses virtual addressing because a tap can cross a page.
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
                    tapAtlasUv = centerAtlasUv + tapOffset / atlasResolution;
                else if (!virtualShadowAtlasUv(virtualPixel + tapOffset, level,
                                               pageTable, tapAtlasUv))
                    tapAtlasUv = centerAtlasUv;
                filteredShadow += 1.0 - shadowMap.SampleCmpLevelZero(
                    tapAtlasUv, currentDepth - bias);
            }
        }
        return filteredShadow * 0.25;
    }

    // Levels 0 and 1 use a 3x3 tent. Usually every tap is in the centre
    // page, so resolve that page once and offset directly in atlas space.
    // Only a one-texel border needs the virtual-page slow path.
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
            float2 tapAtlasUv;
            if (kernelFitsCenterPage)
            {
                tapAtlasUv = centerAtlasUv + float2(x, y) / atlasResolution;
            }
            else
            {
                // Outside the resident clip region, replicate the centre sample
                // instead of accidentally reading an unrelated atlas tile.
                if (!virtualShadowAtlasUv(virtualPixel + float2(x, y), level,
                                          pageTable, tapAtlasUv))
                    tapAtlasUv = centerAtlasUv;
            }
            const float weight = kernel[x + 1] * kernel[y + 1];
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
        float shadow = sampleVirtualShadowLevel(shadowMap, worldPosition,
                                                clipMatrices[level], level,
                                                pageTable, resident);
        if (resident)
            return shadow;
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
