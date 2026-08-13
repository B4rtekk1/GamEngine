#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 3) in vec3 inNormal;
layout(location = 4) in mat4 instanceModel;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec4 fragLightSpacePosition;
layout(location = 2) out vec3 fragWorldPosition;
layout(location = 3) out vec3 fragNormal;

layout(binding = 1) uniform FrameData {
    mat4 view;
    mat4 projection;
    mat4 lightSpace;
    vec4 cameraPosition;
    vec4 lightDirectionIntensity;
} frame;

layout(push_constant) uniform PushConstants {
    vec4 baseColorMetallic;
    vec4 roughnessAo;
} pushConstants;

void main() {
    vec4 worldPosition = instanceModel * vec4(inPosition, 1.0);
    gl_Position = frame.projection * frame.view * worldPosition;
    fragColor = inColor;
    fragWorldPosition = worldPosition.xyz;
    fragNormal = mat3(transpose(inverse(instanceModel))) * inNormal;
    fragLightSpacePosition = frame.lightSpace * worldPosition;
}
