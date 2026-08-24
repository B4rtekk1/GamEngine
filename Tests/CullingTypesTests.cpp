#include <Engine/Renderer/Culling/CullingTypes.h>

#include <cstddef>

int main() {
    using namespace Engine::Culling;

    static_assert(alignof(GPUVec4) == 16);
    static_assert(sizeof(GPUVec4) == 16);
    static_assert(alignof(GPUMat4) == 16);
    static_assert(sizeof(GPUMat4) == 64);
    static_assert(alignof(GPUObjectData) == 16);
    static_assert(sizeof(GPUObjectData) == 128);
    static_assert(alignof(CullingUniformData) == 16);
    static_assert(sizeof(CullingUniformData) == 112);

    GPUVec4 vector{1.0f, 2.0f, 3.0f, 4.0f};
    if (vector.x != 1.0f || vector.y != 2.0f || vector.z != 3.0f || vector.w != 4.0f) return 1;

    GPUMat4 matrix{};
    for (std::size_t index = 0; index < 16; ++index) {
        matrix.data[index] = static_cast<float>(index);
    }
    if (matrix.data[0] != 0.0f || matrix.data[7] != 7.0f || matrix.data[15] != 15.0f) return 2;

    GPUObjectData object{};
    object.localAabbMin = {-1.0f, -2.0f, -3.0f, 0.0f};
    object.localAabbMax = {1.0f, 2.0f, 3.0f, 0.0f};
    object.indexCount = 36;
    object.instanceCount = 2;
    object.firstIndex = 4;
    object.vertexOffset = 8;
    object.firstInstance = 10;
    object.castShadow = 1;
    if (object.localAabbMin.x != -1.0f || object.localAabbMax.z != 3.0f ||
        object.indexCount != 36 || object.instanceCount != 2 || object.firstIndex != 4 ||
        object.vertexOffset != 8 || object.firstInstance != 10 || object.castShadow != 1) return 3;

    CullingUniformData uniforms{};
    uniforms.objectCount = 12;
    uniforms.maxDrawCount = 64;
    uniforms.hizMipCount = 6;
    uniforms.enableOcclusionCulling = 1;
    uniforms.viewportWidth = 1920.0f;
    uniforms.viewportHeight = 1080.0f;
    uniforms.depthBias = 0.001f;
    uniforms.aabbExpansion = 1.25f;
    uniforms.cameraCut = 1;
    uniforms.shadowPass = 0;
    uniforms.enableFrustumCulling = 1;
    if (uniforms.objectCount != 12 || uniforms.maxDrawCount != 64 ||
        uniforms.hizMipCount != 6 || uniforms.enableOcclusionCulling != 1 ||
        uniforms.viewportWidth != 1920.0f || uniforms.viewportHeight != 1080.0f ||
        uniforms.depthBias != 0.001f || uniforms.aabbExpansion != 1.25f ||
        uniforms.cameraCut != 1 || uniforms.shadowPass != 0 ||
        uniforms.enableFrustumCulling != 1) return 4;

    return 0;
}
