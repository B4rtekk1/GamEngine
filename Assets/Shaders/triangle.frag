#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec4 fragLightSpacePosition;
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D shadowMap;

const float MIN_SHADOW_BIAS = 0.0025;
const float SLOPE_SHADOW_BIAS = 0.02;

float calculateShadow() {
    vec3 projected = fragLightSpacePosition.xyz / fragLightSpacePosition.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    float currentDepth = projected.z;
    if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0 || currentDepth <= 0.0 || currentDepth >= 1.0) return 0.0;

    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    float depthSlope = max(abs(dFdx(currentDepth)), abs(dFdy(currentDepth)));
    float bias = max(MIN_SHADOW_BIAS, SLOPE_SHADOW_BIAS * depthSlope);
    float occlusion = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float storedDepth = texture(shadowMap, uv + vec2(x, y) * texel).r;
            occlusion += currentDepth - bias > storedDepth ? 1.0 : 0.0;
        }
    }
    return occlusion / 9.0;
}

void main() {
    float shadow = calculateShadow();
    outColor = vec4(fragColor * (0.25 + 0.75 * (1.0 - shadow)), 1.0);
}
