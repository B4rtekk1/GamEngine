#version 450

layout(location = 0) in vec3 outDirection;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 1) uniform samplerCube environmentMap;

void main() {
    outColor = texture(environmentMap, normalize(outDirection));
}
