#pragma once

#include "Engine/Math/Vec3.h"
#include "Engine/Renderer/RenderConfig.h"
#include "Engine/Renderer/Vulkan/buffer.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"

#include <array>
#include <cstdint>
#include <span>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets {
class AssetManager;
}
class ClusterRayTracingScene;

/**
 * Ray-traced DDGI over a camera-centred, toroidally scrolled probe grid.
 * Per frame: a compute pass picks the probes to refresh (round-robin, newly
 * scrolled-in, and rarely the ones buried in geometry) and writes indirect
 * arguments; a ray tracing pipeline with shader execution reordering traces
 * only those probes against the cluster/partitioned scene; one fused compute
 * dispatch integrates irradiance and visibility and writes the octahedral
 * borders. Forward PBR samples the atlases through its own descriptor set.
 *
 * The grid is repeated in cascades of growing spacing, so distant surfaces
 * keep probe lighting rather than showing where the finest volume ends.
 */
class DdgiPass final {
  public:
    static constexpr std::uint32_t ProbesX = 16, ProbesY = 8, ProbesZ = 16;
    static constexpr std::uint32_t ProbesPerCascade = ProbesX * ProbesY * ProbesZ;
    static_assert((ProbesX & (ProbesX - 1)) == 0 && (ProbesY & (ProbesY - 1)) == 0 && (ProbesZ & (ProbesZ - 1)) == 0,
                  "DdgiCommon.slang wraps probe storage with a bit mask");
    /** Cascade c has spacing probeSpacing * CascadeSpacingScale^c. */
    static constexpr std::uint32_t CascadeCount = 3;
    static constexpr float CascadeSpacingScale = 3.0F;
    static constexpr std::uint32_t ProbeCount = ProbesPerCascade * CascadeCount;
    static constexpr std::uint32_t MaxRaysPerProbe = 256;
    static constexpr std::uint32_t MaxWakeBoxes = 16;
    static constexpr VkFormat IrradianceFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat DistanceFormat = VK_FORMAT_R16G16_SFLOAT;

    /** Matches DdgiVolume in DdgiCommon.slang (std140). */
    struct VolumeData final {
        std::int32_t gridMin[4]{};
        std::int32_t probeCounts[4]{};
        float spacingBias[4]{};
        float rayParams[4]{};
        std::uint32_t atlasInfo[4]{};
        float atlasSize[4]{};
        float rayRotation[3][4]{};
        std::uint32_t wake[4]{};
        float wakeBoxes[2 * MaxWakeBoxes][4]{};
        /** xyz: lowest grid coordinate, w: spacing as float bits. */
        std::int32_t cascadeGrid[CascadeCount][4]{};
        std::uint32_t cascades[4]{};
    };
    static_assert(sizeof(VolumeData) == 736, "VolumeData is a shader ABI");

    /** What disturbed the probe cache this frame. */
    struct WakeRequest final {
        /** Lighting, sky or scene structure changed: every probe re-converges. */
        bool all{false};
        /** World-space min/max boxes of moved geometry (old and new place). */
        std::span<const std::array<float, 6>> boxes;
    };

    struct GeometryBuffers final {
        VkBuffer meshlets{VK_NULL_HANDLE};
        VkBuffer meshletVertices{VK_NULL_HANDLE};
        VkBuffer meshletTriangles{VK_NULL_HANDLE};
        VkBuffer vertices{VK_NULL_HANDLE};
    };

    ~DdgiPass();
    DdgiPass() = default;
    DdgiPass(const DdgiPass&) = delete;
    DdgiPass& operator=(const DdgiPass&) = delete;

    /**
     * Always creates the per-frame volume uniforms (zeroed: DDGI disabled) so
     * Forward can bind them. With `rayTracing`, also creates the probe atlases
     * and the trace pipeline, whose set 0 is Forward's descriptor layout.
     */
    void create(VkPhysicalDevice physicalDevice, VkDevice device, VmaAllocator allocator, Assets::AssetManager& assets,
                bool rayTracing, VkDescriptorSetLayout forwardLayout, std::span<const std::uint32_t> sharingFamilies);
    void destroy() noexcept;
    [[nodiscard]] bool available() const noexcept {
        return tracePipeline_ != VK_NULL_HANDLE;
    }
    void reset() noexcept;
    /** Clears the atlases and probe states; required once before Forward samples them. */
    void initialize(VkCommandBuffer commandBuffer) {
        clearImages(commandBuffer, false);
    }
    [[nodiscard]] bool initialized() const noexcept {
        return initialized_;
    }
    /**
     * Scrolls the volume to the camera and publishes this frame's volume
     * uniforms. With `traced` false the uniforms keep the last traced grid,
     * so other views keep sampling valid data.
     */
    void prepareFrame(std::uint32_t frameSlot, Vec3 cameraPosition, const DdgiSettings& settings, bool traced);
    /**
     * Publishes this frame's probe-cache invalidation. Call after
     * prepareFrame() and before the frame is submitted.
     */
    void wakeProbes(std::uint32_t frameSlot, const WakeRequest& request);
    /** Probes traced by the most recent frame whose GPU work has completed. */
    [[nodiscard]] std::uint32_t tracedProbes() const noexcept {
        return tracedProbes_;
    }
    /**
     * Records probe selection, the trace and the blend. The atlases are
     * written in GENERAL; before initialize() the graph imports them as
     * UNDEFINED and this call clears them first.
     */
    void record(VkCommandBuffer commandBuffer, std::uint32_t frameSlot, VkDescriptorSet forwardSet,
                const ClusterRayTracingScene& scene, const GeometryBuffers& geometry);
    [[nodiscard]] VkPipeline tracePipeline() const noexcept {
        return tracePipeline_;
    }

    /** Whether Forward samples the probes this frame (the volume's gridMin.w). */
    [[nodiscard]] bool volumeValid(std::uint32_t frameSlot) const noexcept {
        return volumeData_[frameSlot % FramesInFlight].gridMin[3] != 0;
    }
    [[nodiscard]] VkBuffer volumeBuffer(std::uint32_t frameSlot) const noexcept {
        return volumes_[frameSlot % FramesInFlight].handle();
    }
    [[nodiscard]] const HdrBuffer& irradianceAtlas() const noexcept {
        return irradiance_;
    }
    [[nodiscard]] const HdrBuffer& distanceAtlas() const noexcept {
        return distance_;
    }
    [[nodiscard]] VkExtent2D irradianceExtent() const noexcept {
        return irradianceExtent_;
    }
    [[nodiscard]] VkExtent2D distanceExtent() const noexcept {
        return distanceExtent_;
    }

  private:
    static constexpr std::uint32_t FramesInFlight = 2;
    static constexpr std::uint32_t IrradianceTexels = 8, DistanceTexels = 16;
    struct Kernel final {
        VkDescriptorSetLayout setLayout{VK_NULL_HANDLE};
        VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
        VkPipeline pipeline{VK_NULL_HANDLE};
        std::array<VkDescriptorSet, FramesInFlight> sets{};
    };
    void clearImages(VkCommandBuffer commandBuffer, bool graphOwnsAtlases);
    void createTracePipeline(Assets::AssetManager& assets, VkDescriptorSetLayout forwardLayout);
    void createComputeKernels(Assets::AssetManager& assets);
    void writeDescriptors(std::uint32_t frame, const ClusterRayTracingScene& scene, const GeometryBuffers& geometry);

    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    PFN_vkCmdTraceRaysIndirectKHR cmdTraceRaysIndirect_{};
    PFN_vkCmdBeginDebugUtilsLabelEXT beginLabel_{};
    PFN_vkCmdEndDebugUtilsLabelEXT endLabel_{};
    std::array<Buffer, FramesInFlight> volumes_{};
    std::array<VolumeData, FramesInFlight> volumeData_{};
    HdrBuffer irradiance_;
    HdrBuffer distance_;
    HdrBuffer rays_;
    VkExtent2D irradianceExtent_{};
    VkExtent2D distanceExtent_{};
    Buffer probeData_;
    Buffer probeList_;
    Buffer indirectArgs_;
    Buffer shaderBindingTable_;
    VkStridedDeviceAddressRegionKHR raygenRegion_{}, missRegion_{}, hitRegion_{}, callableRegion_{};
    VkDescriptorSetLayout traceSetLayout_{VK_NULL_HANDLE};
    VkPipelineLayout tracePipelineLayout_{VK_NULL_HANDLE};
    VkPipeline tracePipeline_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, FramesInFlight> traceSets_{};
    Kernel select_;
    Kernel blend_;
    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    // Trace sets name scene buffers that change on geometry rebuilds.
    std::array<std::array<VkBuffer, 6>, FramesInFlight> traceBuffers_{};
    std::array<std::array<std::int32_t, 3>, CascadeCount> gridMin_{};
    bool gridValid_{false};
    bool traced_{false};
    bool initialized_{false};
    std::uint32_t raysPerProbe_{128};
    std::uint32_t updateInterval_{2};
    std::uint64_t frameIndex_{};
    // The traced-probe count, copied from the indirect arguments per frame slot.
    std::array<Buffer, FramesInFlight> tracedCounts_{};
    std::array<bool, FramesInFlight> tracedCountPending_{};
    std::uint32_t tracedProbes_{};
    // Settings and enablement of the previous frame: a change wakes every probe.
    DdgiSettings lastSettings_{};
    bool wakeAll_{true};
};
} // namespace Engine
