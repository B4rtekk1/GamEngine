#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec4 fragLightSpacePosition;
layout(location = 2) in vec3 fragWorldPosition;
layout(location = 3) in vec3 fragNormal;
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D shadowMap;
layout(binding = 1) uniform FrameData {
    mat4 view;
    mat4 projection;
    mat4 lightSpace;
    vec4 cameraPosition;
    vec4 lightDirectionIntensity;
} frame;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColorMetallic;
    vec4 roughnessAo;
} pushConstants;

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

const float PI = 3.14159265359;

float distributionGGX(vec3 n, vec3 h, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float nDotH = max(dot(n, h), 0.0);
    float nDotH2 = nDotH * nDotH;
    float denominator = nDotH2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denominator * denominator, 0.0001);
}

float geometrySchlickGGX(float nDotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 0.0001);
}

float geometrySmith(vec3 n, vec3 v, vec3 l, float roughness) {
    return geometrySchlickGGX(max(dot(n, v), 0.0), roughness) *
           geometrySchlickGGX(max(dot(n, l), 0.0), roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - cosTheta, 5.0);
}

void main() {
    vec3 albedo = pow(fragColor * pushConstants.baseColorMetallic.rgb, vec3(2.2));
    float metallic = clamp(pushConstants.baseColorMetallic.a, 0.0, 1.0);
    float roughness = clamp(pushConstants.roughnessAo.x, 0.045, 1.0);
    float ao = clamp(pushConstants.roughnessAo.y, 0.0, 1.0);
    vec3 n = normalize(fragNormal);
    vec3 v = normalize(frame.cameraPosition.xyz - fragWorldPosition);
    vec3 l = normalize(-frame.lightDirectionIntensity.xyz);
    vec3 h = normalize(v + l);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 f = fresnelSchlick(max(dot(h, v), 0.0), f0);
    float ndf = distributionGGX(n, h, roughness);
    float geometry = geometrySmith(n, v, l, roughness);
    vec3 specular = (ndf * geometry * f) /
                    max(4.0 * max(dot(n, v), 0.0) * max(dot(n, l), 0.0), 0.0001);
    vec3 kS = f;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
    float nDotL = max(dot(n, l), 0.0);
    vec3 radiance = vec3(1.0) * frame.lightDirectionIntensity.w;
    float shadow = calculateShadow();
    vec3 direct = (kD * albedo / PI + specular) * radiance * nDotL * (1.0 - shadow);
    vec3 ambient = vec3(0.035) * albedo * ao;
    vec3 color = ambient + direct;
    // The swapchain is an sRGB display target; convert the linear PBR result.
    outColor = vec4(pow(color / (color + vec3(1.0)), vec3(1.0 / 2.2)), 1.0);
}
