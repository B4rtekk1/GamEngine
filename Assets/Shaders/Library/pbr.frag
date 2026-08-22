#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec4 fragLightSpacePosition;
layout(location = 2) in vec3 fragWorldPosition;
layout(location = 3) in vec3 fragNormal;
layout(location = 4) flat in uint fragMaterialIndex;
layout(location = 5) in vec2 fragTexCoord;
layout(location = 6) in vec4 fragTangent;
layout(location = 7) flat in uint fragInstanceIndex;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D shadowMap;
layout(binding = 1) uniform FrameData {
    mat4 view; mat4 projection; mat4 lightSpace;
    vec4 cameraPosition; vec4 lightDirectionIntensity; vec4 lightColor;
    uint shadowEnabled; uint materialSlots; uint selectedInstance; uint materialSlotsPadding;
} frame;
struct MaterialData {
    vec4 baseColorMetallic;
    vec4 roughnessAmbientOcclusion;
    ivec4 textureIndices;
};
layout(std430, binding = 2) readonly buffer MaterialBuffer {
    MaterialData materials[];
};
layout(binding = 3) uniform sampler2D materialTextures[16];

const float PI = 3.14159265359;
const float MIN_SHADOW_BIAS = 0.0025;
const float SLOPE_SHADOW_BIAS = 0.02;

float calculateShadow() {
    vec3 projected = fragLightSpacePosition.xyz / fragLightSpacePosition.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    float currentDepth = projected.z;
    if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0 || currentDepth <= 0.0 || currentDepth >= 1.0) return 0.0;
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    float bias = max(MIN_SHADOW_BIAS, SLOPE_SHADOW_BIAS * max(abs(dFdx(currentDepth)), abs(dFdy(currentDepth))));
    float occlusion = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            occlusion += currentDepth - bias > texture(shadowMap, uv + vec2(x, y) * texel).r ? 1.0 : 0.0;
    return occlusion / 9.0;
}

float distributionGGX(vec3 n, vec3 h, float r) {
    float a2 = r * r * r * r;
    float nDotH2 = max(dot(n, h), 0.0); nDotH2 *= nDotH2;
    float denominator = nDotH2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denominator * denominator, 0.0001);
}
float geometrySchlickGGX(float nDotV, float r) {
    float k = (r + 1.0) * (r + 1.0) / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 0.0001);
}
vec3 fresnelSchlick(float cosTheta, vec3 f0) { return f0 + (1.0 - f0) * pow(1.0 - cosTheta, 5.0); }

void main() {
    MaterialData material = materials[fragMaterialIndex];
    vec4 baseColor = vec4(fragColor * material.baseColorMetallic.rgb, 1.0);
    if (material.textureIndices.x >= 0)
        baseColor *= texture(materialTextures[material.textureIndices.x], fragTexCoord);
    // This renderer combines all primitives in one draw, so it cannot sort
    // glTF BLEND foliage cards back-to-front. Treat their alpha as a cutout:
    // this preserves correct depth for overlapping leaves and avoids their
    // rectangular cards becoming visible. Opaque materials keep alpha 1.
    const bool doubleSided = (material.textureIndices.w & 1) != 0;
    const bool alphaBlend = (material.textureIndices.w & 2) != 0;
    if (alphaBlend) {
        if (baseColor.a < material.roughnessAmbientOcclusion.z) discard;
        baseColor.a = 1.0;
    } else if (baseColor.a <= 0.01) discard;
    // Base-colour textures are uploaded as VK_FORMAT_*_SRGB and are already
    // converted to linear space by sampling. Converting them again here made
    // Blender foliage unnaturally dark.
    vec3 albedo = baseColor.rgb;
    const bool selected = fragInstanceIndex == frame.selectedInstance;
    float metallic = material.baseColorMetallic.a;
    float roughness = material.roughnessAmbientOcclusion.x;
    if (material.textureIndices.y >= 0) {
        vec4 metallicRoughness = texture(materialTextures[material.textureIndices.y], fragTexCoord);
        metallic *= metallicRoughness.b;
        roughness *= metallicRoughness.g;
    }
    metallic = clamp(metallic, 0.0, 1.0);
    roughness = clamp(roughness, 0.045, 1.0);
    vec3 n = normalize(fragNormal);
    if (doubleSided && !gl_FrontFacing) n = -n;
    if (material.textureIndices.z >= 0 && abs(material.roughnessAmbientOcclusion.w) > 1e-6) {
        vec3 tangent = normalize(fragTangent.xyz - n * dot(n, fragTangent.xyz));
        vec3 bitangent = normalize(cross(n, tangent)) * fragTangent.w;
        vec3 mappedNormal = texture(materialTextures[material.textureIndices.z], fragTexCoord).xyz * 2.0 - 1.0;
        mappedNormal.xy *= material.roughnessAmbientOcclusion.w;
        n = normalize(mat3(tangent, bitangent, n) * mappedNormal);
    }
    vec3 v = normalize(frame.cameraPosition.xyz - fragWorldPosition);
    vec3 l = normalize(-frame.lightDirectionIntensity.xyz);
    vec3 h = normalize(v + l);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 f = fresnelSchlick(max(dot(h, v), 0.0), f0);
    float nDotV = max(dot(n, v), 0.0), nDotL = max(dot(n, l), 0.0);
    float geometry = geometrySchlickGGX(nDotV, roughness) * geometrySchlickGGX(nDotL, roughness);
    vec3 specular = distributionGGX(n, h, roughness) * geometry * f / max(4.0 * nDotV * nDotL, 0.0001);
    vec3 diffuse = (vec3(1.0) - f) * (1.0 - metallic) * albedo / PI;
    // The default 27k-cube stress scene has no shadow casters. Avoid the
    // 3x3 PCF texture fetches for every shaded fragment in that case.
    float shadow = frame.shadowEnabled != 0u ? calculateShadow() : 0.0;
    vec3 direct = (diffuse + specular) * frame.lightDirectionIntensity.w * frame.lightColor.rgb * nDotL * (1.0 - shadow);
    vec3 color = vec3(0.035) * albedo * clamp(material.roughnessAmbientOcclusion.y, 0.0, 1.0) + direct;
    // Keep lighting in linear HDR space. Display mapping is performed once,
    // after the complete scene (including the skybox) has been rendered.
    outColor = vec4(color, baseColor.a);
}
