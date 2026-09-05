static const float MIN_SHADOW_BIAS = 0.0002;
static const float SLOPE_SHADOW_BIAS = 0.003;
static const uint SHADOW_CLIP_LEVELS = 4;
static const uint VIRTUAL_PAGES_PER_AXIS = 32;
static const uint PHYSICAL_PAGES_PER_AXIS = 16;
static const uint PAGE_RESOLUTION = 256;
static const uint VIRTUAL_RESOLUTION = VIRTUAL_PAGES_PER_AXIS * PAGE_RESOLUTION;
static const uint INVALID_SHADOW_PAGE = 0xffffffff;

// Resolves one virtual texel to the centre of its physical atlas texel. Keep
// this integer-addressed: filtering is done explicitly below so it remains
// correct when a bilinear footprint crosses a virtual-page boundary.
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

// Performs comparison filtering in virtual texture space. Sampling each
// resolved atlas texel at its centre avoids bleeding into an adjacent,
// unrelated physical page while preserving the fractional UV for the blend.
float sampleVirtualShadowBilinear(Sampler2DShadow shadowMap, float2 uv,
                                  float depth, uint level,
                                  StructuredBuffer<uint4> pageTable,
                                  out bool resident)
{
    float2 texelPosition = uv * float(VIRTUAL_RESOLUTION) - 0.5;
    int2 baseTexel = int2(floor(texelPosition));
    float2 fraction = frac(texelPosition);

    float2 uv00, uv10, uv01, uv11;
    bool r00 = resolveVirtualShadowTexel(baseTexel, level, pageTable, uv00);
    bool r10 = resolveVirtualShadowTexel(baseTexel + int2(1, 0), level, pageTable, uv10);
    bool r01 = resolveVirtualShadowTexel(baseTexel + int2(0, 1), level, pageTable, uv01);
    bool r11 = resolveVirtualShadowTexel(baseTexel + int2(1, 1), level, pageTable, uv11);

    resident = r00;
    if (!resident)
        return 0.0;

    // A non-resident neighbouring texel must not expose an arbitrary atlas
    // tile. Replicating the base texel gives a stable boundary instead.
    if (!r10) uv10 = uv00;
    if (!r01) uv01 = uv00;
    if (!r11) uv11 = uv00;

    float s00 = 1.0 - shadowMap.SampleCmpLevelZero(uv00, depth);
    float s10 = 1.0 - shadowMap.SampleCmpLevelZero(uv10, depth);
    float s01 = 1.0 - shadowMap.SampleCmpLevelZero(uv01, depth);
    float s11 = 1.0 - shadowMap.SampleCmpLevelZero(uv11, depth);
    return lerp(lerp(s00, s10, fraction.x), lerp(s01, s11, fraction.x), fraction.y);
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
    resident = currentDepth > 0.0 && currentDepth < 1.0;
    if (!resident)
        return 0.0;

    float depthSlope = max(abs(ddx(currentDepth)), abs(ddy(currentDepth)));
    float bias = max(MIN_SHADOW_BIAS, SLOPE_SHADOW_BIAS * depthSlope);
    // Each lookup keeps the fractional virtual position. This is deliberately
    // virtual-page aware because adjacent virtual texels need not be adjacent
    // in the physical atlas.
    const float2 virtualPixel = uv * float(VIRTUAL_RESOLUTION);
    if (level >= 2)
    {
        // The coarser visible levels retain a four-tap PCF kernel; a single
        // comparison sample at L3 made its already large texels conspicuous.
        float filteredShadow = 0.0;
        bool centreResident = false;
        [unroll]
        for (int y = 0; y < 2; ++y)
        {
            [unroll]
            for (int x = 0; x < 2; ++x)
            {
                bool tapResident;
                float tap = sampleVirtualShadowBilinear(
                    shadowMap, (virtualPixel + float2(x, y) - 0.5) / float(VIRTUAL_RESOLUTION),
                    currentDepth - bias, level, pageTable, tapResident);
                if (x == 0 && y == 0)
                    centreResident = tapResident;
                filteredShadow += tap;
            }
        }
        resident = centreResident;
        return centreResident ? filteredShadow * 0.25 : 0.0;
    }

    // Levels 0 and 1 use a 3x3 tent around virtual bilinear lookups.
    static const float kernel[3] = {1.0, 2.0, 1.0};
    float filteredShadow = 0.0;
    float totalWeight = 0.0;
    bool centreResident = false;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float weight = kernel[x + 1] * kernel[y + 1];
            bool tapResident;
            float tap = sampleVirtualShadowBilinear(
                shadowMap, (virtualPixel + float2(x, y)) / float(VIRTUAL_RESOLUTION),
                currentDepth - bias, level, pageTable, tapResident);
            if (x == 0 && y == 0)
                centreResident = tapResident;
            filteredShadow += tap * weight;
            totalWeight += weight;
        }
    }
    resident = centreResident;
    return centreResident ? filteredShadow / totalWeight : 0.0;
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
            if (level + 1 < SHADOW_CLIP_LEVELS)
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
