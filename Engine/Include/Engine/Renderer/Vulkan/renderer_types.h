#pragma once

#include <cstdint>
#include <limits>
#include <array>

#include <glm/glm.hpp>

#include "Engine/Core/Camera.h"
#include "Engine/Renderer/Vulkan/shadow_map.h"

namespace Engine {
    inline constexpr std::uint32_t MaxLocalLights = 32;

    /** GPU-friendly record for point and spot lights. */
    struct alignas(16) LocalLightGPU {
        glm::vec4 positionRange{};
        glm::vec4 directionOuterCos{};
        glm::vec4 colorIntensity{};
        // x: inner cone cosine, y: LightType, z: casts local shadow.
        glm::vec4 parameters{};
    };

    struct RendererUniformBufferObject {
        Mat4 view;
        Mat4 projection;
        Mat4 previousView;
        Mat4 previousProjection;
        // Camera-centred directional-light virtual clipmaps.
        std::array<Mat4, 4> shadowClipMatrices{};
        Vec4 cameraPosition;
        Vec4 lightDirectionIntensity;
        Vec4 lightColor;
        // xyz: normalized world-space direction, w: displacement strength.
        Vec4 windDirectionStrength;
        // xyz: source position, w: radius in world units.
        Vec4 windSourcePositionRange;
        // x: gust strength, y: frequency, z: current time, w: previous time.
        Vec4 windGustFrequencyTime;
        std::uint32_t shadowEnabled{0};
        std::uint32_t materialSlots{1};
        std::uint32_t selectedInstance{std::numeric_limits<std::uint32_t>::max()};
        std::uint32_t materialSlotsPadding{};
        std::uint32_t localLightCount{};
        std::array<LocalLightGPU, MaxLocalLights> localLights{};
    };

    /**
     * Generic-renderer instance data. It is intentionally kept separate from
     * GPUGrassInstance: history and full quaternion/scale data make this a
     * 128-byte format and it is unsuitable for dense foliage.
     */
    struct RendererInstanceData {
        // xyz: world position, w: bit-cast material-table base index.
        glm::vec4 positionMaterial{};
        // Quaternion stored as xyzw.
        glm::vec4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
        // xyz: non-uniform scale, w: local mesh minimum Y for grass bending.
        glm::vec4 scaleBase{1.0F, 1.0F, 1.0F, 0.0F};
        // xyz: bend X, bend Z, trample; w: reciprocal grass mesh height.
        glm::vec4 grassDeformation{};
        glm::vec4 previousGrassDeformation{};
        glm::vec4 previousPosition{};
        glm::vec4 previousRotation{0.0F, 0.0F, 0.0F, 1.0F};
        glm::vec4 previousScale{1.0F};
    };
    static_assert(sizeof(RendererInstanceData) == 128);

    /** std430-compatible records backing the persistent GPU Scene SSBOs. */
    struct alignas(16) GPUSceneInstanceRecord {
        glm::mat4 worldMatrix{1.0F};
        glm::vec4 localBoundsMin{};
        glm::vec4 localBoundsMax{};
        glm::uvec4 idsAndFlags{}; // meshId, materialId, objectId, flags
    };

    struct alignas(16) GPUSceneMeshRecord {
        glm::uvec4 draw{}; // firstIndex, indexCount, vertexOffset, lod1IndexCount
        glm::uvec4 lod{};  // lod2IndexCount, reserved, reserved, reserved
    };

    struct alignas(16) GPUSceneMaterialRecord {
        glm::uvec4 data{}; // material-table offset, pipeline class, flags, reserved
    };

    /** Parameters shared by the grass count/scatter/finalize compute passes. */
    struct alignas(16) GrassIndirectUniformData {
        std::uint32_t visibleCapacity{};
        std::uint32_t binCapacity{};
        std::uint32_t compactBase{};
        std::uint32_t maxBins{};
        glm::vec4 cameraPosition{};
    };

    struct alignas(16) GrassPrefixUniformData {
        std::uint32_t binCount{};
        std::uint32_t compactBase{};
        std::uint32_t padding0{};
        std::uint32_t padding1{};
    };

    /** Parameters for the packed-grass visibility and stream split passes. */
    struct alignas(16) GrassClassifyUniformData {
        std::uint32_t visibleCount{};
        float mainDistance{};
        float shadowDistance{};
        float velocityDistance{};
        glm::vec4 cameraPosition{};
    };

    struct alignas(16) GrassPackedCullUniformData {
        glm::mat4 viewProjection{1.0F};
        glm::vec4 cameraPosition{};
        std::uint32_t instanceCount{};
        std::uint32_t padding0{};
        std::uint32_t padding1{};
        std::uint32_t padding2{};
    };
    struct alignas(16) GrassPackedStreamUniformData {
        std::uint32_t visibleCapacity{};
        std::uint32_t streamIndex{};
        std::uint32_t clusterCount{};
        std::uint32_t padding{};
    };

    /**
     * Dense-foliage instance, decoded relative to GPUGrassCluster. Keeping
     * this independent of RendererInstanceData prevents generic transform
     * history from leaking into the grass renderer.
     *
     * packedXZ: UNORM16 local X | UNORM16 local Z
     * packedYRotation: FP16 world Y | UNORM16 yaw
     * packedScaleSeed: FP16 uniform scale | uint16 deterministic seed
     * flags: grass type / future interaction flags
     */
    struct alignas(4) GPUGrassInstance {
        std::uint32_t packedXZ{};
        std::uint32_t packedYRotation{};
        std::uint32_t packedScaleSeed{};
        std::uint32_t flags{};
    };
    static_assert(sizeof(GPUGrassInstance) == 16);

    /** One culling/rendering unit for a contiguous range of packed blades. */
    struct alignas(16) GPUGrassCluster {
        // xy: world-space XZ origin; z: largest local XZ extent; w: reserved.
        glm::vec4 originExtent{};
        // x: first packed instance, y: count, z: material table offset, w: flags.
        glm::uvec4 instanceRange{};
        // firstIndex, indexCount, vertexOffset, LOD policy/flags.  Kept with
        // the packed cluster so indirect generation never consults GPUScene.
        glm::uvec4 draw{};
    };
    static_assert(sizeof(GPUGrassCluster) == 48);
} // namespace Engine
