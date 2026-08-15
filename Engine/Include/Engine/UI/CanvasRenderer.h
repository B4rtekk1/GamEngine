#pragma once

#include <Engine/UI/UIBatch.h>
#include <Engine/UI/Vulkan/UIPipeline.h>

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Engine {
class Buffer;
namespace Assets { class AssetManager; }
}

namespace Engine::UI {

class Canvas;
class UIElement;

class CanvasRenderer final {
public:
    ~CanvasRenderer();

    CanvasRenderer();
    CanvasRenderer(const CanvasRenderer&) = delete;
    CanvasRenderer& operator=(const CanvasRenderer&) = delete;

    void create(VkPhysicalDevice physicalDevice, VkDevice device,
                VkFormat colorFormat, VkExtent2D extent,
                const std::vector<VkImageView>& imageViews,
                std::uint32_t framesInFlight,
                Assets::AssetManager& assets);
    void destroy() noexcept;

    void record(const Canvas& canvas, VkCommandBuffer commandBuffer,
                std::uint32_t imageIndex, std::uint32_t frameIndex,
                VkExtent2D extent);

private:
    struct FrameResources;

    void appendElement(const UIElement& element);
    [[nodiscard]] std::uint64_t canvasRevision(const Canvas& canvas) const noexcept;
    [[nodiscard]] std::uint64_t elementRevision(const UIElement& element) const noexcept;
    [[nodiscard]] bool ensureCapacity(FrameResources& frame);

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    UIPipeline pipeline_;
    UIBatch batch_;
    std::vector<std::unique_ptr<FrameResources>> frames_;
    std::uint64_t cachedCanvasRevision_{};
    bool batchDirty_{true};
    std::uint64_t pendingFrameUploads_{};
};

} // namespace Engine::UI
