#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec3 inNormal;
layout(location = 4) in mat4 instanceModel;
layout(location = 8) in uint inMaterialIndex;

layout(location = 0) flat out uint fragMaterialIndex;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) flat out uint fragInstanceIndex;

layout(binding = 1) uniform FrameData {
    mat4 view;
    mat4 projection;
    mat4 lightSpace;
    vec4 cameraPosition;
    vec4 lightDirectionIntensity;
    vec4 lightColor;
    uint materialSlots;
    uint selectedInstance;
    uint materialSlotsPadding;
} frame;

void main() {
    vec4 worldPosition = instanceModel * vec4(inPosition, 1.0);
    vec4 clipPosition = frame.projection * frame.view * worldPosition;
    vec3 viewNormal = normalize(mat3(frame.view * instanceModel) * inNormal);
    vec2 screenDirection = normalize(viewNormal.xy);
    if (length(viewNormal.xy) < 0.001) screenDirection = vec2(0.0, 1.0);
    clipPosition.xy += screenDirection * 0.012 * clipPosition.w;
    gl_Position = clipPosition;
    fragMaterialIndex = gl_InstanceIndex * frame.materialSlots + inMaterialIndex;
    fragTexCoord = inTexCoord;
    fragInstanceIndex = gl_InstanceIndex;
}
