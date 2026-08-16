#pragma once

#include <cstdint>

namespace Engine::Culling {
    struct alignas(16) GPUVec4 {
        float x;
        float y;
        float z;
        float w;
    };

    struct alignas(16) GPUMat4 {
        float data[16];
    };

    /**
     *
     */
    struct alignas(16) GPUObjectData {
        GPUMat4 model;

        GPUVec4 localAabbMin;
        GPUVec4 localAabbMax;

        std::uint32_t indexCount;
        std::uint32_t instanceCount;
        std::uint32_t firstIndex;
        std::uint32_t vertexOffset;
        std::uint32_t firstInstance;
        std::uint32_t castShadow;
    };

    struct alignas(16) CullingUniformData
    {
        GPUMat4 viewProjection;

        std::uint32_t objectCount;
        std::uint32_t maxDrawCount;
        std::uint32_t hizMipCount;
        std::uint32_t enableOcclusionCulling;

        float viewportWidth;
        float viewportHeight;
        float depthBias;
        float aabbExpansion;

        std::uint32_t cameraCut;
        std::uint32_t shadowPass;
        std::uint32_t enableFrustumCulling;
        std::uint32_t padding2;
    };
}
