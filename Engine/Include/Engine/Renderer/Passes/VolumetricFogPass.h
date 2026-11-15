#pragma once

#include "Engine/Renderer/RenderConfig.h"

#include <array>
#include <cstdint>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets {
class AssetManager;
}

/**
 * Volumetric (froxel) fog of the Game View.
 *
 * A camera-aligned grid of RGBA16F froxels (screen tiles x exponential depth
 * slices) holds in-scattered light and extinction. Three compute kernels run
 * once the frame is lit, after the SSR radiance capture so reflections do not
 * carry the fog twice:
 *  - fog_inject: density, sun (VSM), clustered local lights and sky
 *    ambient per froxel, jittered in depth and blended with the
 *    reprojected history;
 *  - fog_integrate: front-to-back integral of every froxel column;
 *  - fog_apply: colour * transmittance + in-scattering over the HDR target.
 * The kernels bind the scene descriptor set (frame data, light clusters,
 * shadow map) as set 0 and the fog images as set 1.
 */
class VolumetricFogPass final {
  public:
    struct CreateInfo {
        VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
        VkDevice device{VK_NULL_HANDLE};
        VmaAllocator allocator{VK_NULL_HANDLE};
        Assets::AssetManager* assets{nullptr};
        /** Layout of the scene set (ShadowPass), bound as set 0. */
        VkDescriptorSetLayout sceneLayout{VK_NULL_HANDLE};
        VkExtent2D extent{};
        VolumetricFogQuality quality{VolumetricFogQuality::Medium};
        /** Opaque depth, sampled in DEPTH_STENCIL_READ_ONLY. */
        VkImageView depthView{VK_NULL_HANDLE};
        VkSampler depthSampler{VK_NULL_HANDLE};
        /** HDR target, read and written in place as a storage image. */
        VkImage hdrImage{VK_NULL_HANDLE};
        VkImageView hdrView{VK_NULL_HANDLE};
    };

    VolumetricFogPass() = default;
    ~VolumetricFogPass();
    VolumetricFogPass(const VolumetricFogPass&) = delete;
    VolumetricFogPass& operator=(const VolumetricFogPass&) = delete;

    void create(const CreateInfo& info);
    void destroy() noexcept;
    [[nodiscard]] bool created() const noexcept {
        return device_ != VK_NULL_HANDLE;
    }
    /** True when the pass was created for these targets and this grid. */
    [[nodiscard]] bool matches(VkExtent2D extent, VolumetricFogQuality quality, VkImageView depthView,
                               VkImageView hdrView) const noexcept;
    /** Drops the temporal history, e.g. after a camera cut. */
    void reset() noexcept {
        historyValid_ = false;
    }
    /**
     * Records inject, integrate and apply. The HDR target must be in
     * SHADER_READ_ONLY and is left there; the depth in DEPTH_STENCIL_READ_ONLY.
     */
    void record(VkCommandBuffer commandBuffer, VkDescriptorSet sceneSet, const FogSettings& settings);

    [[nodiscard]] VkExtent3D gridSize() const noexcept {
        return grid_;
    }

  private:
    enum class Kernel : std::uint8_t { Inject, Integrate, Apply };
    void createImages();
    void createPipelines();
    void createDescriptors();

    CreateInfo info_{};
    VkDevice device_{VK_NULL_HANDLE};
    VkExtent3D grid_{};
    // Ping-pong scattering/extinction volumes (current and history), and the integral.
    std::array<VkImage, 3> images_{};
    std::array<VmaAllocation, 3> allocations_{};
    std::array<VkImageView, 3> views_{};
    VkSampler sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout setLayout_{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
    std::array<VkPipeline, 3> pipelines_{};
    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    // Set v writes scattering volume v and reads volume 1 - v as history.
    std::array<VkDescriptorSet, 2> sets_{};
    std::uint32_t current_{0};
    std::uint32_t frameCounter_{0};
    bool initialized_{false};
    bool historyValid_{false};
};
} // namespace Engine
