#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) flat in uint fragMaterialIndex;

struct MaterialData {
    vec4 baseColorMetallic;
    vec4 roughnessAmbientOcclusion;
    ivec4 textureIndices;
};

layout(std430, binding = 2) readonly buffer MaterialBuffer {
    MaterialData materials[];
};
layout(binding = 3) uniform sampler2D materialTextures[16];

void main() {
    MaterialData material = materials[fragMaterialIndex];
    if (material.textureIndices.x >= 0) {
        const float alpha = texture(materialTextures[material.textureIndices.x], fragTexCoord).a;
        const float cutoff = (material.textureIndices.w & 2) != 0
            ? material.roughnessAmbientOcclusion.z : 0.01;
        if (alpha < cutoff) discard;
    }
}
