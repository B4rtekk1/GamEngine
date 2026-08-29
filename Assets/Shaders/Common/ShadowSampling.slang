static const float MIN_SHADOW_BIAS = 0.0015;
static const float SLOPE_SHADOW_BIAS = 0.015;

float sampleVirtualShadowLevel(Sampler2D shadowMap, float3 worldPosition,
                               float4x4 lightSpace, uint level, out bool resident)
{
    float4 lightSpacePosition = mul(float4(worldPosition, 1.0), lightSpace);
    float3 projected = lightSpacePosition.xyz / lightSpacePosition.w;
    float2 uv = projected.xy * 0.5 + 0.5;
    float currentDepth = projected.z;
    resident = uv.x > 0.006 && uv.x < 0.994 && uv.y > 0.006 && uv.y < 0.994 &&
               currentDepth > 0.0 && currentDepth < 1.0;
    if (!resident)
        return 0.0;

    uint width;
    uint height;
    shadowMap.GetDimensions(width, height);
    float2 texel = 1.0 / float2(width, height);
    float2 tileOffset = float2((level & 1) != 0 ? 0.5 : 0.0,
                               (level & 2) != 0 ? 0.5 : 0.0);
    float2 atlasUv = tileOffset + uv * 0.5;
    float2 tileMinimum = tileOffset + 1.5 * texel;
    float2 tileMaximum = tileOffset + 0.5 - 1.5 * texel;
    float depthSlope = max(abs(ddx(currentDepth)), abs(ddy(currentDepth)));
    float bias = max(MIN_SHADOW_BIAS, SLOPE_SHADOW_BIAS * depthSlope);
    float occlusion = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            occlusion += currentDepth - bias >
                shadowMap.Sample(clamp(atlasUv + float2(x, y) * texel,
                                       tileMinimum, tileMaximum)).r ? 1.0 : 0.0;
    return occlusion / 9.0;
}

float sampleVirtualShadow(Sampler2D shadowMap, float3 worldPosition,
                          float4x4 clipMatrices[4])
{
    bool resident;
    [unroll]
    for (uint level = 0; level < 4; ++level)
    {
        float shadow = sampleVirtualShadowLevel(shadowMap, worldPosition,
                                                clipMatrices[level], level, resident);
        if (resident)
            return shadow;
    }
    return 0.0;
}

// Compatibility path for the standalone sample shader, which deliberately
// uses one conventional light matrix instead of the renderer clipmap UBO.
float samplePcfShadow(Sampler2D shadowMap, float4 lightSpacePosition)
{
    float3 projected = lightSpacePosition.xyz / lightSpacePosition.w;
    float2 uv = projected.xy * 0.5 + 0.5;
    float currentDepth = projected.z;
    if (any(uv <= 0.0) || any(uv >= 1.0) || currentDepth <= 0.0 || currentDepth >= 1.0)
        return 0.0;
    uint width;
    uint height;
    shadowMap.GetDimensions(width, height);
    float2 texel = 1.0 / float2(width, height);
    float bias = max(MIN_SHADOW_BIAS,
                     SLOPE_SHADOW_BIAS * max(abs(ddx(currentDepth)), abs(ddy(currentDepth))));
    float occlusion = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            occlusion += currentDepth - bias > shadowMap.Sample(uv + float2(x, y) * texel).r
                ? 1.0 : 0.0;
    return occlusion / 9.0;
}
