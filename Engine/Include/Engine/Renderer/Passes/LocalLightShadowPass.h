#pragma once

#include "Engine/Renderer/Vulkan/buffer.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"

#include <array>
#include <functional>
#include <span>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets {
class AssetManager;
}

/**
 * Ray-traced shadows of point and spot lights. A trace writes one binary
 * visibility sample per quad for each of its strongest lights; a denoiser
 * (spatial cross-bilateral plus reprojected temporal accumulation) turns them
 * into soft per-light occlusion that forward shading reads by light index.
 * Traces the Mega Geometry cluster scene with a ray tracing pipeline when the
 * device supports it, otherwise inline ray queries against the classic
 * triangle TLAS.
 */
class LocalLightShadowPass final {
  public:
    /** Trace output: four 16-bit light samples. */
    static constexpr VkFormat SampleFormat = VK_FORMAT_R32G32_UINT;
    /** Denoised result: four light indices, their occlusion, depth and history length. */
    static constexpr VkFormat ResultFormat = VK_FORMAT_R32G32B32A32_UINT;

    struct Settings final {
        /** Rays per pixel, to its strongest lights (at most 4); the rest stay unshadowed. */
        std::uint32_t maxRaysPerPixel = 4;
        float normalBias = 0.01F;
        float minIrradiance = 0.003F;
        /**
         * Quads whose denoised history has converged retrace only every this
         * many frames (1: every frame). Valid while geometry and lights are
         * static; the denoiser keeps their accumulated visibility meanwhile.
         */
        std::uint32_t convergedRayInterval = 2;
        /**
         * Sample four lights per quad in proportion to their unshadowed
         * irradiance from the tile lists (MegaLights-style) instead of tracing
         * the strongest ones; the deferred local light pass shades those
         * samples. Skips the visibility denoiser.
         */
        bool stochastic = false;
    };

    struct Inputs final {
        /** Partitioned TLAS of the cluster scene; when set, the ray tracing pipeline traces it. */
        VkDeviceAddress clusterScene{};
        /** Classic TLAS for the inline ray query kernel. */
        VkAccelerationStructureKHR tlas{VK_NULL_HANDLE};
        VkImageView depth{VK_NULL_HANDLE};
        VkSampler depthSampler{VK_NULL_HANDLE};
        VkImageView normals{VK_NULL_HANDLE};
        VkSampler normalSampler{VK_NULL_HANDLE};
        VkBuffer clusterRanges{VK_NULL_HANDLE};
        VkBuffer clusterIndices{VK_NULL_HANDLE};
        VkBuffer localLights{VK_NULL_HANDLE};
        /** Depth-bounded tile light lists (TileLightCullingPass); always bound. */
        VkBuffer tileLights{VK_NULL_HANDLE};
    };

    ~LocalLightShadowPass();
    LocalLightShadowPass() = default;
    LocalLightShadowPass(const LocalLightShadowPass&) = delete;
    LocalLightShadowPass& operator=(const LocalLightShadowPass&) = delete;

    /**
     * Without `tracing` (no ray queries) only the masks exist: clear() keeps
     * forward shading's binding valid and every light unshadowed.
     * `clusterTracing` adds the ray tracing pipeline for the cluster scene.
     */
    void create(VkPhysicalDevice physicalDevice, VkDevice device, VkExtent2D extent, VmaAllocator allocator,
                Assets::AssetManager& assets, std::span<const VkBuffer> frameUniformBuffers, bool tracing,
                bool clusterTracing);
    [[nodiscard]] bool tracing() const noexcept {
        return pipeline_ != VK_NULL_HANDLE || clusterTracing();
    }
    [[nodiscard]] bool clusterTracing() const noexcept {
        return tracePipeline_ != VK_NULL_HANDLE;
    }
    void destroy() noexcept;
    /**
     * Traces and denoises into the frame slot's result, accumulating over the
     * other slot's (the previous frame's). Leaves the result in
     * SHADER_READ_ONLY_OPTIMAL for the fragment stage.
     */
    void record(VkCommandBuffer commandBuffer, std::uint32_t frameSlot, const Inputs& inputs,
                const Settings& settings, const std::function<void()>& afterTrace = {});
    /** Fills the frame slot's result with "lit, no history", so forward shading sees every light as lit. */
    void clear(VkCommandBuffer commandBuffer, std::uint32_t frameSlot);
    [[nodiscard]] VkImageView resultView(std::uint32_t frameSlot) const noexcept {
        return results_[frameSlot % results_.size()].imageView();
    }
    [[nodiscard]] VkSampler resultSampler(std::uint32_t frameSlot) const noexcept {
        return results_[frameSlot % results_.size()].sampler();
    }
    [[nodiscard]] VkExtent2D extent() const noexcept {
        return extent_;
    }
    /** Stochastic mode outputs, one texel per 2x2 quad, in GENERAL layout. */
    [[nodiscard]] VkImageView sampleView() const noexcept {
        return samples_.imageView();
    }
    [[nodiscard]] VkImageView sampleWeightView() const noexcept {
        return sampleWeights_.imageView();
    }

  private:
    VkDevice device_{VK_NULL_HANDLE};
    VkExtent2D extent_{};
    HdrBuffer samples_;
    HdrBuffer sampleWeights_;
    std::array<HdrBuffer, 2> results_{};
    std::array<bool, 2> resultDefined_{};
    VkDescriptorSetLayout layout_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, 2> sets_{};
    VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    // Cluster-scene ray tracing pipeline and its shader binding table.
    void createTracePipeline(VkPhysicalDevice physicalDevice, VmaAllocator allocator, Assets::AssetManager& assets);
    VkPipelineLayout tracePipelineLayout_{VK_NULL_HANDLE};
    VkPipeline tracePipeline_{VK_NULL_HANDLE};
    Buffer shaderBindingTable_;
    VkStridedDeviceAddressRegionKHR raygenRegion_{};
    VkStridedDeviceAddressRegionKHR missRegion_{};
    VkStridedDeviceAddressRegionKHR hitRegion_{};
    PFN_vkCmdTraceRaysKHR cmdTraceRays_{};
    // Denoiser.
    void createDenoisePipeline(Assets::AssetManager& assets, std::span<const VkBuffer> frameUniformBuffers);
    void clearResult(VkCommandBuffer commandBuffer, std::uint32_t slot);
    VkDescriptorSetLayout denoiseLayout_{VK_NULL_HANDLE};
    VkDescriptorPool denoisePool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, 2> denoiseSets_{};
    VkPipelineLayout denoisePipelineLayout_{VK_NULL_HANDLE};
    VkPipeline denoisePipeline_{VK_NULL_HANDLE};
};
} // namespace Engine
