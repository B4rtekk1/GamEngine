#version 450

layout(location = 0) in vec3 inPosition;

layout(location = 0) out vec3 outDirection;

layout(set = 0, binding = 0) uniform CameraData {
    mat4 view;
    mat4 projection;
} camera;

void main() {
    outDirection = inPosition;

    mat4 view = mat4(mat3(camera.view));

    vec4 position =
    camera.projection *
    view *
    vec4(inPosition, 1.0);

    gl_Position = position.xyww;
}