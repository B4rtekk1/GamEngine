#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec4 fragLightSpacePosition;

layout(binding = 1) uniform FrameData {
    mat4 view;
    mat4 projection;
    mat4 lightSpace;
} frame;

layout(push_constant) uniform PushConstants {
    mat4 model;
} pushConstants;

void main() {
    gl_Position = frame.projection * frame.view * pushConstants.model * vec4(inPosition, 1.0);
    fragColor = inColor;
    fragLightSpacePosition = frame.lightSpace * pushConstants.model * vec4(inPosition, 1.0);
}
