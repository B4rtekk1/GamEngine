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
     * Compact per-instance data.  Position, rotation and scale reconstruct
     * the model and normal transforms in the vertex shader; this halves the
     * old matrix-plus-normal representation (128 B -> 64 B).
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
} // namespace Engine
