#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inTexCoord;
layout(location = 4) in mat4 instanceModel;
layout(location = 8) in uint inMaterialIndex;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) flat out uint fragMaterialIndex;

layout(binding = 1) uniform FrameData {
    mat4 view; mat4 projection; mat4 lightSpaceUnused;
    vec4 cameraPosition; vec4 lightDirectionIntensity;
    vec4 lightColor;
    uint materialSlots;
    uint materialSlotsPadding0;
    uint materialSlotsPadding1;
    uint materialSlotsPadding2;
} frame;

layout(push_constant) uniform PushConstants {
    mat4 lightSpace;
} pushConstants;

void main() {
    gl_Position = pushConstants.lightSpace * instanceModel * vec4(inPosition, 1.0);
    fragTexCoord = inTexCoord;
    fragMaterialIndex = gl_InstanceIndex * frame.materialSlots + inMaterialIndex;
}
