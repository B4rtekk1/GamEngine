#pragma once

#include <Engine/UI/UIBatch.h>
#include <Engine/UI/Vulkan/UIPipeline.h>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
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
                Assets::AssetManager& assets,
                VkImageView fontAtlasView = VK_NULL_HANDLE,
                VkSampler fontAtlasSampler = VK_NULL_HANDLE,
                VmaAllocator allocator = VK_NULL_HANDLE);
    void destroy() noexcept;

    void record(const Canvas& canvas, VkCommandBuffer commandBuffer,
                std::uint32_t imageIndex, std::uint32_t frameIndex,
                VkExtent2D extent);

private:
    struct FrameResources;

    void appendElement(const UIElement& element);
    [[nodiscard]] const std::vector<const UIElement*>& sortedChildren(const UIElement& element);
    void sortIfNeeded(const std::vector<const UIElement*>& source,
                      std::vector<const UIElement*>& cache);
    [[nodiscard]] bool ensureCapacity(FrameResources& frame);

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    UIPipeline pipeline_;
    UIBatch batch_;
    std::vector<std::unique_ptr<FrameResources>> frames_;
    std::vector<const UIElement*> sortedRootElements_;
    std::vector<const UIElement*> rootElementsSource_;
    std::unordered_map<const UIElement*, std::vector<const UIElement*>> sortedChildren_;
    std::uint64_t cachedCanvasRevision_{};
    bool batchDirty_{true};
    std::uint64_t pendingFrameUploads_{};
};

} // namespace Engine::UI
