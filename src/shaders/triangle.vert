#version 450

layout(location = 0) out vec3 fragColor;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
} pushConstants;

const vec3 positions[8] = vec3[](
    vec3(-0.5, -0.5, -0.5), vec3( 0.5, -0.5, -0.5),
    vec3( 0.5,  0.5, -0.5), vec3(-0.5,  0.5, -0.5),
    vec3(-0.5, -0.5,  0.5), vec3( 0.5, -0.5,  0.5),
    vec3( 0.5,  0.5,  0.5), vec3(-0.5,  0.5,  0.5)
);

const int indices[36] = int[](
    0, 1, 2, 2, 3, 0,  4, 6, 5, 6, 4, 7,
    0, 4, 5, 5, 1, 0,  3, 2, 6, 6, 7, 3,
    1, 5, 6, 6, 2, 1,  0, 3, 7, 7, 4, 0
);

const vec3 colors[6] = vec3[](
    vec3(0.95, 0.25, 0.20), vec3(0.20, 0.75, 0.95),
    vec3(0.25, 0.90, 0.40), vec3(0.95, 0.75, 0.20),
    vec3(0.75, 0.30, 0.95), vec3(0.20, 0.85, 0.75)
);

void main() {
    gl_Position = pushConstants.mvp * vec4(positions[indices[gl_VertexIndex]], 1.0);
    fragColor = colors[gl_VertexIndex / 6];
}
