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
    outColor = vec4(1.0);
}
