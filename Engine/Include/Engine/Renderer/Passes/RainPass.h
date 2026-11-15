#pragma once

#include "Engine/Renderer/Vulkan/graphics_pipeline.h"

#include <cstdint>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets {
class AssetManager;
}

/**
 * Falling rain over the lit Game View: up to MaxDrops thin streaks in a box
 * around the camera, generated entirely in the vertex shader from the
 * instance index (Weather/rain.slang). One instanced draw, no buffers and no
 * CPU simulation. Streaks are depth tested against the opaque depth (read
 * only), lit by the sky and the clustered local lights, and alpha blended
 * into the HDR target before TAA.
 */
class RainPass final {
  public:
    struct CreateInfo {
        VkDevice device{VK_NULL_HANDLE};
        Assets::AssetManager* assets{nullptr};
        /** Layout of the scene set (ShadowPass), bound as set 0. */
        VkDescriptorSetLayout sceneLayout{VK_NULL_HANDLE};
        VkFormat colorFormat{VK_FORMAT_UNDEFINED};
        VkFormat depthFormat{VK_FORMAT_UNDEFINED};
    };
    struct RecordInfo {
        VkDescriptorSet sceneSet{VK_NULL_HANDLE};
        VkImage color{VK_NULL_HANDLE};
        VkImageView colorView{VK_NULL_HANDLE};
        VkImageView depthView{VK_NULL_HANDLE};
        VkExtent2D extent{};
        /** 0..1: drop count and visibility. */
        float intensity{0.0F};
        float time{0.0F};
    };

    /** Drops at intensity 1. */
    static constexpr std::uint32_t MaxDrops = 24000;

    RainPass() = default;
    ~RainPass();
    RainPass(const RainPass&) = delete;
    RainPass& operator=(const RainPass&) = delete;

    void create(const CreateInfo& info);
    void destroy() noexcept;
    [[nodiscard]] bool created() const noexcept {
        return device_ != VK_NULL_HANDLE;
    }
    [[nodiscard]] bool matches(VkFormat colorFormat, VkFormat depthFormat) const noexcept {
        return created() && info_.colorFormat == colorFormat && info_.depthFormat == depthFormat;
    }
    /**
     * Draws the rain. The colour target must be in SHADER_READ_ONLY (left
     * there) and the depth in DEPTH_STENCIL_READ_ONLY (unchanged).
     */
    void record(VkCommandBuffer commandBuffer, const RecordInfo& info);

  private:
    CreateInfo info_{};
    VkDevice device_{VK_NULL_HANDLE};
    GraphicsPipeline pipeline_;
};
} // namespace Engine
