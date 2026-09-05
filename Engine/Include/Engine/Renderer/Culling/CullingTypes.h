#pragma once

/**
 * @file CullingTypes.h
 * @brief Defines CPU/GPU-compatible data structures used by GPU culling.
 */

#include <cstdint>

namespace Engine::Culling {
    /** @brief Four-component floating-point value aligned for GPU access. */
    struct alignas(16) GPUVec4 {
        /// First component.
        float x;
        /// Second component.
        float y;
        /// Third component.
        float z;
        /// Fourth component.
        float w;
    };

    /** @brief Column-major 4x4 matrix representation for GPU buffers. */
    struct alignas(16) GPUMat4 {
        /// Matrix elements stored as a contiguous array of 16 floats.
        float data[16];
    };

    /**
     * @brief Per-object data consumed by the GPU culling shader.
     */
    struct alignas(16) GPUObjectData {
        /// World transformation applied to the object's local bounds.
        GPUMat4 model;

        /// Minimum corner of the object's local-space axis-aligned bounding box.
        GPUVec4 localAabbMin;
        /// Maximum corner of the object's local-space axis-aligned bounding box.
        GPUVec4 localAabbMax;

        /// Number of indices used by the object's draw command.
        std::uint32_t indexCount;
        /// Number of instances used by the object's draw command.
        std::uint32_t instanceCount;
        /// First index in the shared index buffer.
        std::uint32_t firstIndex;
        /// Vertex offset applied by the indexed draw command.
        std::uint32_t vertexOffset;
        /// First instance value written to the draw command.
        std::uint32_t firstInstance;
        /// Non-zero when the object should be included in shadow rendering.
        std::uint32_t castShadow;
        /// Non-zero when the object requires double-sided rasterization.
        std::uint32_t twoSided;
        std::uint32_t lod1IndexCount;
        std::uint32_t lod2IndexCount;
        float lod1Distance;
        float lod2Distance;
    };

    /**
     * @brief Per-dispatch parameters shared with the GPU culling shader.
     */
    struct alignas(16) CullingUniformData
    {
        /// Combined view and projection matrix of the active camera.
        GPUMat4 viewProjection;
        /// Normalized world-space frustum planes: left, right, bottom, top, near, far.
        GPUVec4 frustumPlanes[6];
        GPUVec4 cameraPosition;

        /// Number of objects available to the culling dispatch.
        std::uint32_t objectCount;
        /// Maximum number of indirect draw commands that may be produced.
        std::uint32_t maxDrawCount;
        /// Number of mip levels available in the Hi-Z buffer.
        std::uint32_t hizMipCount;
        /// Non-zero when hierarchical-Z occlusion testing is enabled.
        std::uint32_t enableOcclusionCulling;

        /// Rendering viewport width.
        float viewportWidth;
        /// Rendering viewport height.
        float viewportHeight;
        /// Depth comparison bias used by occlusion testing.
        float depthBias;
        /// Expansion applied to projected bounding boxes.
        float aabbExpansion;

        /// Non-zero when the camera changed discontinuously.
        std::uint32_t cameraCut;
        /// Non-zero when the dispatch belongs to a shadow pass.
        std::uint32_t shadowPass;
        /// Non-zero when frustum culling is enabled.
        std::uint32_t enableFrustumCulling;
        /// 0: opaque-only, 1: double-sided-only, 2: all objects (shadow pass).
        std::uint32_t drawCategory;
    };
}
