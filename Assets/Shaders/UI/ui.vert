#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;
layout(location = 3) in float inTextSample;

layout(push_constant) uniform ScreenData {
    vec2 inverseExtent;
} screen;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;
layout(location = 2) out float outTextSample;

void main() {
    vec2 ndc = inPosition * screen.inverseExtent * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
    outUv = inUv;
    outColor = inColor;
    outTextSample = inTextSample;
}
