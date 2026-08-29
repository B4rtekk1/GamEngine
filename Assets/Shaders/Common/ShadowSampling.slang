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
    // Hardware PCF is bilinear and could otherwise blend with an unrelated
    // physical page stored next to this tile in the atlas. Clamp the lookup
    // to a one-texel guard band; the duplicated edge texel is preferable to
    // temporal noise from the page cache.
    const float atlasResolution = float(PHYSICAL_PAGES_PER_AXIS * PAGE_RESOLUTION);
    float2 atlasPixel = centerAtlasUv * atlasResolution - 0.5;
    float2 pageOrigin = floor(atlasPixel / float(PAGE_RESOLUTION)) * float(PAGE_RESOLUTION);
    atlasPixel = clamp(atlasPixel, pageOrigin + 1.0,
                       pageOrigin + float(PAGE_RESOLUTION - 2));
    centerAtlasUv = (atlasPixel + 0.5) / atlasResolution;
    // Vulkan's comparison sampler now performs the 2x2 PCF in fixed-function
    // hardware, replacing the former nine manual depth reads.
    return 1.0 - shadowMap.SampleCmpLevelZero(centerAtlasUv, currentDepth - bias);
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
