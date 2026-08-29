static const float MIN_SHADOW_BIAS = 0.0025;
static const float SLOPE_SHADOW_BIAS = 0.02;

float samplePcfShadow(Sampler2D shadowMap, float4 lightSpacePosition)
{
    float3 projected = lightSpacePosition.xyz / lightSpacePosition.w;
    float2 uv = projected.xy * 0.5 + 0.5;
    float currentDepth = projected.z;
    if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0 ||
        currentDepth <= 0.0 || currentDepth >= 1.0)
        return 0.0;

    uint width;
    uint height;
    shadowMap.GetDimensions(width, height);
    float2 texel = 1.0 / float2(width, height);
    float depthSlope = max(abs(ddx(currentDepth)), abs(ddy(currentDepth)));
    float bias = max(MIN_SHADOW_BIAS, SLOPE_SHADOW_BIAS * depthSlope);
    float occlusion = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            occlusion += currentDepth - bias >
                shadowMap.Sample(uv + float2(x, y) * texel).r ? 1.0 : 0.0;
    return occlusion / 9.0;
}
