#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec3 inNormal;
layout(location = 4) in mat4 instanceModel;
layout(location = 8) in uint inMaterialIndex;
layout(location = 9) in vec4 inTangent;
layout(location = 10) in vec3 inNormalColumn0;
layout(location = 11) in vec3 inNormalColumn1;
layout(location = 12) in vec3 inNormalColumn2;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec4 fragLightSpacePosition;
layout(location = 2) out vec3 fragWorldPosition;
layout(location = 3) out vec3 fragNormal;
layout(location = 4) flat out uint fragMaterialIndex;
layout(location = 5) out vec2 fragTexCoord;
layout(location = 6) out vec4 fragTangent;
layout(location = 7) flat out uint fragInstanceIndex;

layout(binding = 1) uniform FrameData {
    mat4 view;
    mat4 projection;
    mat4 lightSpace;
    vec4 cameraPosition;
    vec4 lightDirectionIntensity;
    vec4 lightColor;
    uint shadowEnabled;
    uint materialSlots;
    uint selectedInstance;
    uint materialSlotsPadding;
} frame;

void main() {
    vec4 worldPosition = instanceModel * vec4(inPosition, 1.0);
    mat3 normalMatrix = mat3(inNormalColumn0, inNormalColumn1, inNormalColumn2);
    gl_Position = frame.projection * frame.view * worldPosition;
    fragColor = inColor;
    fragWorldPosition = worldPosition.xyz;
    fragNormal = normalMatrix * inNormal;
    fragLightSpacePosition = frame.lightSpace * worldPosition;
    fragMaterialIndex = gl_InstanceIndex * frame.materialSlots + inMaterialIndex;
    fragTexCoord = inTexCoord;
    fragTangent = vec4(normalMatrix * inTangent.xyz, inTangent.w);
    fragInstanceIndex = gl_InstanceIndex;
}
