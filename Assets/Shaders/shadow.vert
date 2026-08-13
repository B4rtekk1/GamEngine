#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 4) in mat4 instanceModel;

layout(push_constant) uniform PushConstants {
    mat4 lightSpace;
} pushConstants;

void main() {
    gl_Position = pushConstants.lightSpace * instanceModel * vec4(inPosition, 1.0);
}
