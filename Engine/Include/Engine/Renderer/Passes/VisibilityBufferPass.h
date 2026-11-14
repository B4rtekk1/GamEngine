#pragma once

#include "Engine/Renderer/Vulkan/hdr_buffer.h"

#include <array>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets {
class AssetManager;
}

/**
 * Visibility-buffer prototype for Game View (GAMENGINE_VISBUFFER=1).
 *
 * The mesh-shader depth prepass writes each pixel's triangle into ids(): the
 * meshletDrawList entry above the triangle's index within its meshlet, for
 * the material slots the resolve shades (MeshletDrawVisibility). record()
 * then runs forward_pbr's visibilityResolveMain: one thread per visible
 * pixel interpolates its triangle with analytic barycentrics and runs the
 * forward surface shading, writing the HDR colour and the deferred local
 * light inputs. The lighting pass no longer rasterizes those slots, so no
 * fragment of theirs is shaded twice or in a helper lane of a partly covered
 * quad.
 */
class VisibilityBufferPass final {
  public:
    static constexpr VkFormat IdFormat = VK_FORMAT_R32_UINT;

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
        return pipeline_ != VK_NULL_HANDLE;
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
     * COLOR_ATTACHMENT_OPTIMAL): shades the resolved pixels into both and
     * leaves them in COLOR_ATTACHMENT_OPTIMAL for the deferred local lights.
     */
    void record(VkCommandBuffer commandBuffer, std::uint32_t frameSlot, VkDescriptorSet sceneSet,
                VkDescriptorSet meshletDrawSet, const Targets& targets);

  private:
    VkDevice device_{VK_NULL_HANDLE};
    VkExtent2D extent_{};
    HdrBuffer ids_;
    VkDescriptorSetLayout layout_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, 2> sets_{};
    VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
};
} // namespace Engine
