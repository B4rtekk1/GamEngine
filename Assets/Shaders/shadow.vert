#version 450

layout(location = 0) in vec3 inPosition;

layout(push_constant) uniform PushConstants {
    mat4 lightMvp;
} pushConstants;

void main() {
    gl_Position = pushConstants.lightMvp * vec4(inPosition, 1.0);
}
