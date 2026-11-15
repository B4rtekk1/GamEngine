#pragma once

#include "Engine/Renderer/Vulkan/buffer.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"

#include <array>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets {
class AssetManager;
}

/**
 * Visibility buffer for Game View, on by default (GAMENGINE_VISBUFFER=0 disables it).
 *
 * The mesh-shader depth prepass writes each pixel's triangle into ids(): its
 * material class, meshletDrawList entry and triangle (MeshletDraw.slang
 * packVisibility), for the program slots the resolve shades. record() then
 *  - classifies the 8x8 tiles by the material classes among their pixels
 *    (visibility_classify.slang), and
 *  - runs one indirect dispatch per class over its tiles with that class's
 *    own forward_pbr program (visibilityResolveMain): one thread per visible
 *    pixel interpolates its triangle with analytic barycentrics and runs the
 *    forward surface shading, writing the HDR colour and the deferred local
 *    light inputs. Pixels whose screen-space reflection may replace the
 *    probe/sky one are queued instead of traced, and
 *  - recordReflections() traces the queue with one thread per pixel
 *    (visibility_ssr.slang), so the Hi-Z loop and its registers stay out of
 *    the surface shading.
 * The lighting pass no longer rasterizes those slots, so no fragment of
 * theirs is shaded twice or in a helper lane of a partly covered quad, and
 * no pixel pays the registers of a heavier material's program.
 */
class VisibilityBufferPass final {
  public:
    static constexpr VkFormat IdFormat = VK_FORMAT_R32_UINT;
    /** MeshletDraw.slang VisibilityClassCount. */
    static constexpr std::uint32_t ClassCount = 4;
    static constexpr std::uint32_t TileSize = 8;

    struct Targets final {
        VkImage hdr{VK_NULL_HANDLE};
        VkImageView hdrView{VK_NULL_HANDLE};
        VkImage surface{VK_NULL_HANDLE};
        VkImageView surfaceView{VK_NULL_HANDLE};
    };

    ~VisibilityBufferPass();
    VisibilityBufferPass() = default;
    VisibilityBufferPass(const VisibilityBufferPass&) = delete;
    VisibilityBufferPass& operator=(const VisibilityBufferPass&) = delete;

    /** `sceneLayout` and `meshletDrawLayout` are sets 0 and 1 of the forward mesh pipelines. */
    void create(VkPhysicalDevice physicalDevice, VkDevice device, VkExtent2D extent, VmaAllocator allocator,
                Assets::AssetManager& assets, VkDescriptorSetLayout sceneLayout,
                VkDescriptorSetLayout meshletDrawLayout);
    void destroy() noexcept;
    [[nodiscard]] bool ready() const noexcept {
        return classifyPipeline_ != VK_NULL_HANDLE;
    }
    [[nodiscard]] VkImageView ids() const noexcept {
        return ids_.imageView();
    }
    [[nodiscard]] VkExtent2D extent() const noexcept {
        return extent_;
    }
    /** Before the depth prepass: the id target becomes a cleared colour attachment. */
    void prepare(VkCommandBuffer commandBuffer) const;
    /**
     * After the lighting pass ended its rendering (HDR and surface target in
     * COLOR_ATTACHMENT_OPTIMAL): shades the resolved pixels into both, in
     * GENERAL layout, and queues their screen-space reflections. False when
     * nothing was recorded; otherwise recordReflections() must follow.
     */
    bool record(VkCommandBuffer commandBuffer, std::uint32_t frameSlot, VkDescriptorSet sceneSet,
                VkDescriptorSet meshletDrawSet, const Targets& targets);
    /**
     * Traces the reflections record() queued (visibility_ssr.slang) on the
     * sets it bound, then leaves both targets in COLOR_ATTACHMENT_OPTIMAL for
     * the deferred local lights.
     */
    void recordReflections(VkCommandBuffer commandBuffer, const Targets& targets) const;

  private:
    [[nodiscard]] std::uint32_t tileCapacity() const noexcept {
        return tilesX() * tilesY();
    }
    [[nodiscard]] std::uint32_t tilesX() const noexcept {
        return (extent_.width + TileSize - 1) / TileSize;
    }
    [[nodiscard]] std::uint32_t tilesY() const noexcept {
        return (extent_.height + TileSize - 1) / TileSize;
    }

    VkDevice device_{VK_NULL_HANDLE};
    VkExtent2D extent_{};
    HdrBuffer ids_;
    // Per class: the tile list and a padded VkDispatchIndirectCommand.
    Buffer classTiles_;
    Buffer classDispatch_;
    // One packed reflection request per resolved pixel (packSsrRequest).
    Buffer ssrRequests_;
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    VkDescriptorSetLayout classifyLayout_{VK_NULL_HANDLE};
    VkDescriptorSet classifySet_{VK_NULL_HANDLE};
    VkPipelineLayout classifyPipelineLayout_{VK_NULL_HANDLE};
    VkPipeline classifyPipeline_{VK_NULL_HANDLE};
    VkDescriptorSetLayout resolveLayout_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, 2> resolveSets_{};
    VkPipelineLayout resolvePipelineLayout_{VK_NULL_HANDLE};
    std::array<VkPipeline, ClassCount> resolvePipelines_{};
    VkPipeline reflectionPipeline_{VK_NULL_HANDLE};
};
} // namespace Engine
