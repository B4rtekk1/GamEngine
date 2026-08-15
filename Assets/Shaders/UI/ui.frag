#version 450

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;
layout(location = 2) in float inTextSample;

layout(set = 0, binding = 0) uniform sampler2D fontAtlas;

layout(location = 0) out vec4 outColor;

void main() {
    float coverage = inTextSample > 0.5 ? texture(fontAtlas, inUv).r : 1.0;
    outColor = vec4(inColor.rgb, inColor.a * coverage);
}
