#version 450

layout(set = 0, binding = 0) uniform sampler2D hdrImage;

layout(push_constant) uniform TonemapSettings {
    float exposure;
    float applyGamma;
} settings;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

vec3 acesFilm(vec3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) /
                 (color * (c * color + d) + e), 0.0, 1.0);
}

void main() {
    vec3 color = texture(hdrImage, inUv).rgb * exp2(settings.exposure);
    color = acesFilm(color);
    if (settings.applyGamma > 0.5) {
        color = pow(color, vec3(1.0 / 2.2));
    }
    outColor = vec4(color, 1.0);
}
