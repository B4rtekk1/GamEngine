#version 450

layout(location = 0) flat in uint fragMaterialIndex;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) flat in uint fragInstanceIndex;
layout(location = 0) out vec4 outColor;

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
layout(std430, binding = 2) readonly buffer MaterialBuffer { MaterialData materials[]; };
layout(binding = 3) uniform sampler2D materialTextures[16];

void main() {
    if (fragInstanceIndex != frame.selectedInstance) discard;
    MaterialData material = materials[fragMaterialIndex];
    vec3 textureColor = material.baseColorMetallic.rgb;
    if (material.textureIndices.x >= 0)
        textureColor *= texture(materialTextures[material.textureIndices.x], fragTexCoord).rgb;
    float luma = dot(textureColor, vec3(0.299, 0.587, 0.114));
    outColor = vec4(luma > 0.5 ? vec3(0.02) : vec3(1.0), 1.0);
}
