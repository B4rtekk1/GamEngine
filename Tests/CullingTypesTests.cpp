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

    GPUVec4 vector{1.0F, 2.0F, 3.0F, 4.0F};
    if (vector.x != 1.0F || vector.y != 2.0F || vector.z != 3.0F || vector.w != 4.0F) return 1;

    GPUMat4 matrix{};
    for (std::size_t index = 0; index < 16; ++index) {
        matrix.data[index] = static_cast<float>(index);
    }
    if (matrix.data[0] != 0.0F || matrix.data[7] != 7.0F || matrix.data[15] != 15.0F) return 2;

    GPUObjectData object{};
    object.localAabbMin = {-1.0F, -2.0F, -3.0F, 0.0F};
    object.localAabbMax = {1.0F, 2.0F, 3.0F, 0.0F};
    object.indexCount = 36;
    object.instanceCount = 2;
    object.firstIndex = 4;
    object.vertexOffset = 8;
    object.firstInstance = 10;
    object.castShadow = 1;
    if (object.localAabbMin.x != -1.0F || object.localAabbMax.z != 3.0F ||
        object.indexCount != 36 || object.instanceCount != 2 || object.firstIndex != 4 ||
        object.vertexOffset != 8 || object.firstInstance != 10 || object.castShadow != 1) return 3;

    CullingUniformData uniforms{};
    uniforms.objectCount = 12;
    uniforms.maxDrawCount = 64;
    uniforms.hizMipCount = 6;
    uniforms.enableOcclusionCulling = 1;
    uniforms.viewportWidth = 1920.0F;
    uniforms.viewportHeight = 1080.0F;
    uniforms.depthBias = 0.001F;
    uniforms.aabbExpansion = 1.25F;
    uniforms.cameraCut = 1;
    uniforms.shadowPass = 0;
    uniforms.enableFrustumCulling = 1;
    if (uniforms.objectCount != 12 || uniforms.maxDrawCount != 64 ||
        uniforms.hizMipCount != 6 || uniforms.enableOcclusionCulling != 1 ||
        uniforms.viewportWidth != 1920.0F || uniforms.viewportHeight != 1080.0F ||
        uniforms.depthBias != 0.001F || uniforms.aabbExpansion != 1.25F ||
        uniforms.cameraCut != 1 || uniforms.shadowPass != 0 ||
        uniforms.enableFrustumCulling != 1) return 4;

    return 0;
}