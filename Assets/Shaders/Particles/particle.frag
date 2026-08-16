#version 450

layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 centered = uv * 2.0 - 1.0;
    float alpha = smoothstep(1.0, 0.65, dot(centered, centered));
    outColor = vec4(color.rgb, color.a * alpha);
}
