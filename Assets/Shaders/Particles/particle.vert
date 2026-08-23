#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;

struct Particle { vec4 positionLife; vec4 velocitySize; vec4 color; vec4 spawnData; };
layout(std430, set = 0, binding = 0) readonly buffer Particles { Particle particles[]; };
layout(std430, set = 0, binding = 2) readonly buffer ActiveIndices { uint activeIndices[]; };

layout(std140, set = 0, binding = 1) uniform Frame {
    mat4 viewProjection;
    vec3 cameraRight;
    vec3 cameraUp;
};

layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 color;

void main() {
    Particle p = particles[activeIndices[gl_InstanceIndex]];
    vec3 world = p.positionLife.xyz +
        (cameraRight * inPosition.x + cameraUp * inPosition.y) * p.velocitySize.w;
    gl_Position = viewProjection * vec4(world, 1.0);
    uv = inUv;
    color = p.color;
}
