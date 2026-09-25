#pragma once

#include <cstdint>
#include <limits>
#include <array>

#include <glm/glm.hpp>

#include "Engine/Core/Camera.h"
#include "Engine/Renderer/Vulkan/GpuScene.h"
#include "Engine/Renderer/Vulkan/shadow_map.h"

namespace Engine {
    // Legacy forward UBO. It is intentionally kept below the Vulkan-guaranteed
    // maxUniformBufferRange (64 KiB): 32 * LocalLightGPU is 2 KiB.
    //
    // Clustered lighting uses a separate SSBO and must use
    // MaxClusteredLocalLights instead. Do not raise this value while
    // RendererUniformBufferObject still contains localLights.
    inline constexpr std::uint32_t MaxLocalLights = 32;
    inline constexpr std::uint32_t MaxClusteredLocalLights = 1024;
    inline constexpr std::uint32_t ClusterTileSize = 64;
    inline constexpr std::uint32_t ClusterDepthSlices = 24;
    inline constexpr std::uint32_t MaxLightsPerCluster = 64;

    /** GPU-friendly record for point and spot lights. */
    struct alignas(16) LocalLightGPU {
        glm::vec4 positionRange{};
        glm::vec4 directionOuterCos{};
        glm::vec4 colorIntensity{};
        // x: inner cone cosine, y: LightType, z: casts local shadow.
        glm::vec4 parameters{};
    };

    /** std430 header for one (x, y, logarithmic-depth) forward+ cluster. */
    struct alignas(8) ClusterLightRangeGPU {
        std::uint32_t lightOffset{};
        std::uint32_t lightCount{};
        std::uint32_t probeOffset{};
        std::uint32_t probeCount{};
    };

    /**
     * Per-view parameters consumed by clustered_light_culling.  The cluster
     * grid is sized from the actual render target, so Game View and Scene
     * View never share an incompatible light list.
     */
    struct alignas(16) ClusteredLightingUniforms {
        glm::mat4 view{1.0F};
        glm::mat4 projection{1.0F};
        glm::uvec4 gridAndLightCount{}; // x tiles, y tiles, z slices, light count
        glm::vec4 viewportNearFar{};    // xy pixels, z near, w far
    };

    /** Inputs for receiver-driven virtual-shadow page marking. */
    struct alignas(16) VsmPageMarkingUniforms {
        glm::mat4 inverseViewProjection{1.0F};
        std::array<glm::mat4, ShadowMap::ClipLevelCount> clipMatrices{};
        std::uint32_t pageCountPerAxis{};
        std::uint32_t virtualResolution{};
        std::uint32_t clipLevelCount{};
        std::uint32_t depthWidth{};
        std::uint32_t depthHeight{};
        // Kept in step with the fragment sampler so receiver requests target
        // the same quality-dependent virtual-shadow LOD.
        std::uint32_t shadowQuality{};
        std::uint32_t padding[2]{};
    };

    struct alignas(16) ShadowClipConstantsGPU {
        float worldTexelSize{};
        float depthScale{};
        glm::vec2 padding{};
    };
    static_assert(sizeof(ShadowClipConstantsGPU) == 16);

    inline std::array<ShadowClipConstantsGPU, ShadowMap::ClipLevelCount> makeShadowClipConstants(
        const std::array<Mat4, ShadowMap::ClipLevelCount>& matrices) {
        std::array<ShadowClipConstantsGPU, ShadowMap::ClipLevelCount> constants{};
        for (std::size_t level = 0; level < constants.size(); ++level) {
            const glm::mat4& matrix = matrices[level].native();
            const float xScale = glm::length(glm::vec3(matrix[0][0], matrix[1][0], matrix[2][0]));
            const float yScale = glm::length(glm::vec3(matrix[0][1], matrix[1][1], matrix[2][1]));
            const float zScale = glm::length(glm::vec3(matrix[0][2], matrix[1][2], matrix[2][2]));
            constants[level].worldTexelSize = 2.0F /
                (static_cast<float>(ShadowMap::VirtualResolution) * glm::max(glm::max(xScale, yScale), 1.0e-6F));
            constants[level].depthScale = glm::max(zScale, 1.0e-6F);
        }
        return constants;
    }

    struct RendererUniformBufferObject {
        Mat4 view;
        Mat4 projection;
        Mat4 inverseProjection;
        Mat4 inverseView;
        Mat4 previousView;
        Mat4 previousProjection;
        // Camera-centred directional-light virtual clipmaps.
        std::array<Mat4, ShadowMap::ClipLevelCount> shadowClipMatrices{};
        std::array<ShadowClipConstantsGPU, ShadowMap::ClipLevelCount> shadowClipConstants{};
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
        std::uint32_t shadowQuality{2};
        std::uint32_t shadowDebugView{0};
        std::uint32_t gtaoDebugView{0};
        std::uint32_t materialSlots{1};
        std::uint32_t selectedInstance{std::numeric_limits<std::uint32_t>::max()};
        std::uint32_t localLightCount{};
        std::uint32_t reflectionProbeCount{};
        // Screen-space inputs (GTAO, opaque colour/depth) are valid only for
        // the Game View render graph. Scene View owns a differently sized
        // target and must not sample Game View textures.
        std::uint32_t screenSpaceEffects{1};
        // Shadow kernels vary their phase only while this view is resolved by
        // TAA; Scene View intentionally remains spatially stable.
        std::uint32_t temporalShadowJitter{0};
        std::uint32_t temporalSampleIndex{0};
        std::uint32_t pbrDebugView{0};
        glm::vec4 clusteredViewportNearFar{};
        // x: logarithmic cluster Z scale, y: bias, z: prefiltered environment mip count, w: RT contact shadows enabled.
        glm::vec4 clusterZScaleBiasEnvironmentMipRtContact{};
        std::array<LocalLightGPU, MaxLocalLights> localLights{};
        // Each cascade uses a snapped world origin and a toroidal atlas offset.
        std::array<glm::vec4, 3> ddgiOriginSpacing{};
        std::array<glm::ivec4, 3> ddgiScrollOffsets{};
        float ddgiIndirectIntensity{15.0F};
    };

    /**
     * Generic-renderer instance data. Dense foliage uses GPUGrassInstance and
     * GPUGrassDeformation instead; do not add vegetation-only state here.
     *
     * This current-state stream remains 48 bytes. TAA history lives in the
     * separately allocated RendererPreviousTransformData stream instead.
     */
    struct RendererInstanceData {
        // xyz: world position, w: bit-cast material-table base index.
        glm::vec4 positionMaterial{};
        // Quaternion stored as xyzw.
        glm::vec4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
        // xyz: non-uniform scale; w is std430 padding.
        glm::vec4 scaleBase{1.0F, 1.0F, 1.0F, 0.0F};
    };
    static_assert(sizeof(RendererInstanceData) == 48);

    /** TAA-only transform history. Kept out of the current instance stream
     * so disabling TAA does not reserve a second transform per renderable. */
    struct RendererPreviousTransformData {
        glm::vec4 previousPosition{};
        glm::vec4 previousRotation{0.0F, 0.0F, 0.0F, 1.0F};
        glm::vec4 previousScale{1.0F};
    };
    static_assert(sizeof(RendererPreviousTransformData) == 48);

    /** std430-compatible records backing the persistent GPU Scene SSBOs. */
    struct alignas(16) GPUSceneInstanceRecord {
        glm::mat4 worldMatrix{1.0F};
        glm::vec4 localBoundsMin{};
        glm::vec4 localBoundsMax{};
        glm::uvec4 idsAndFlags{}; // meshId, materialId, objectId, flags
        glm::vec4 displacementBoundsPadding{}; // x local-space radius padding; yzw reserved
    };
    static_assert(sizeof(GPUSceneInstanceRecord) == 128);

    /** Compact spatial record consumed by coarse instance visibility only.
     * Its index intentionally matches GPUSceneInstanceRecord, preserving the
     * stable GPUScene instance ID used by all downstream passes. */
    struct alignas(16) GPUVisibilityInstanceRecord {
        glm::vec4 worldCenterRadius{};
        glm::uvec4 idsAndFlags{}; // meshId, materialId, objectId, flags
    };
    static_assert(sizeof(GPUVisibilityInstanceRecord) == 32);

    struct alignas(16) GPUSceneMeshRecord {
        glm::uvec4 draw{}; // firstIndex, indexCount, vertexOffset, lod1IndexCount
        // lod2IndexCount, firstMeshlet, meshletCount, reserved
        glm::uvec4 lod{};
        // First node in the global hierarchy payload, root for this draw
        // range, node count, reserved.
        glm::uvec4 clusters{};
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
        float mainDistance{};
        float shadowDistance{};
        float velocityDistance{};
        float padding{};
        glm::vec4 cameraPosition{};
    };

    struct alignas(16) GrassPackedCullUniformData {
        glm::mat4 viewProjection{1.0F};
        glm::vec4 cameraPosition{};
        // Normalized, inward-facing left/right/bottom/top/near/far planes.
        std::array<glm::vec4, 6> frustumPlanes{};
        std::uint32_t clusterCount{};
        std::uint32_t padding0{};
        std::uint32_t padding1{};
        std::uint32_t padding2{};
    };
    /** Inputs for the VSM-specific grass command compaction pass. */
    struct alignas(16) GrassShadowPageCullUniformData {
        std::uint32_t pageCount{};
        std::uint32_t clusterCount{};
        std::uint32_t padding0{};
        std::uint32_t padding1{};
    };
    /** Vulkan DispatchIndirectCommand, written by the cluster cull pass. */
    struct alignas(4) GrassBladeDispatchData {
        std::uint32_t groupCountX{};
        std::uint32_t groupCountY{};
        std::uint32_t groupCountZ{1};
    };
    static_assert(sizeof(GrassBladeDispatchData) == 12);
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

    /** Dynamic, per-blade interaction state.  Each word contains SNORM8 X/Z
     * bend and UNORM8 trampling; keeping current and previous values makes
     * motion vectors agree with the visible deformation. */
    struct alignas(4) GPUGrassDeformation {
        std::uint32_t packedCurrent{};
        std::uint32_t packedPrevious{};
    };
    static_assert(sizeof(GPUGrassDeformation) == 8);

    /** One culling/rendering unit for a contiguous range of packed blades. */
    struct alignas(16) GPUGrassCluster {
        // xy: world-space XZ origin; z: largest local XZ extent; w: mesh
        // bounding radius relative to the blade root at unit scale.
        glm::vec4 originExtent{};
        // x: first packed instance, y: count, z: material table offset, w: flags.
        glm::uvec4 instanceRange{};
        // firstIndex, indexCount, vertexOffset, packed FP16 cluster Y center
        // and half-height. Kept with
        // the packed cluster so indirect generation never consults GPUScene.
        glm::uvec4 draw{};
        // x: mesh minimum Y, y: mesh height, z: material stiffness,
        // w: maximum bend angle in radians.
        glm::vec4 bladeShape{};
    };
    static_assert(sizeof(GPUGrassCluster) == 64);
} // namespace Engine
