#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec4 fragLightSpacePosition;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 lightMvp;
} pushConstants;

void main() {
    gl_Position = pushConstants.mvp * vec4(inPosition, 1.0);
    fragColor = inColor;
    fragLightSpacePosition = pushConstants.lightMvp * vec4(inPosition, 1.0);
}
