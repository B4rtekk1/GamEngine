#include <Engine/UI/CanvasRenderer.h>

#include <Engine/Renderer/Vulkan/buffer.h>
#include <Engine/UI/Canvas.h>
#include <Engine/UI/UIElement.h>

#include <algorithm>
#include <stdexcept>

namespace Engine::UI {

struct CanvasRenderer::FrameResources {
    Buffer vertices;
    Buffer indices;
    std::size_t vertexCapacity{};
    std::size_t indexCapacity{};
};

CanvasRenderer::CanvasRenderer() = default;

CanvasRenderer::~CanvasRenderer() {
    destroy();
}

void CanvasRenderer::create(const VkPhysicalDevice physicalDevice,
                            const VkDevice device, const VkFormat colorFormat,
                            const VkExtent2D extent,
                            const std::vector<VkImageView>& imageViews,
                            const std::uint32_t framesInFlight,
                            Assets::AssetManager& assets) {
    if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
        framesInFlight == 0) {
        throw std::invalid_argument("Canvas renderer received incomplete resources");
    }

    destroy();
    physicalDevice_ = physicalDevice;
    device_ = device;
    try {
        pipeline_.create(device_, colorFormat, extent, imageViews, assets);
        frames_.reserve(framesInFlight);
        for (std::uint32_t index = 0; index < framesInFlight; ++index) {
            frames_.push_back(std::make_unique<FrameResources>());
        }
    } catch (...) {
        destroy();
        throw;
    }
}

void CanvasRenderer::destroy() noexcept {
    frames_.clear();
    batch_.clear();
    pipeline_.destroy();
    device_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
}

void CanvasRenderer::appendElement(const UIElement& element) {
    if (!element.visible) {
        return;
    }
    element.buildGeometry(batch_);

    std::vector<const UIElement*> children;
    children.reserve(element.children().size());
    for (const auto& child : element.children()) {
        children.push_back(child.get());
    }
    std::stable_sort(children.begin(), children.end(),
                     [](const UIElement* lhs, const UIElement* rhs) {
                         return lhs->sortingOrder < rhs->sortingOrder;
                     });
    for (const UIElement* child : children) {
        appendElement(*child);
    }
}

void CanvasRenderer::ensureCapacity(FrameResources& frame) {
    const auto grow = [](const std::size_t required) {
        std::size_t capacity = 64;
        while (capacity < required) {
            capacity *= 2;
        }
        return capacity;
    };

    if (batch_.vertices.size() > frame.vertexCapacity) {
        frame.vertexCapacity = grow(batch_.vertices.size());
        frame.vertices.createHostVisible(
            physicalDevice_, device_, frame.vertexCapacity * sizeof(UIVertex),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    }
    if (batch_.indices.size() > frame.indexCapacity) {
        frame.indexCapacity = grow(batch_.indices.size());
        frame.indices.createHostVisible(
            physicalDevice_, device_, frame.indexCapacity * sizeof(std::uint32_t),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    }
}

void CanvasRenderer::record(const Canvas& canvas,
                            const VkCommandBuffer commandBuffer,
                            const std::uint32_t imageIndex,
                            const std::uint32_t frameIndex,
                            const VkExtent2D extent) {
    batch_.clear();

    std::vector<const UIElement*> elements;
    elements.reserve(canvas.elements().size());
    for (const auto& element : canvas.elements()) {
        elements.push_back(element.get());
    }
    std::stable_sort(elements.begin(), elements.end(),
                     [](const UIElement* lhs, const UIElement* rhs) {
                         return lhs->sortingOrder < rhs->sortingOrder;
                     });
    for (const UIElement* element : elements) {
        appendElement(*element);
    }

    FrameResources& frame = *frames_.at(frameIndex);
    if (!batch_.empty()) {
        ensureCapacity(frame);
        frame.vertices.update(batch_.vertices.data(),
                              batch_.vertices.size() * sizeof(UIVertex));
        frame.indices.update(batch_.indices.data(),
                             batch_.indices.size() * sizeof(std::uint32_t));
    }

    pipeline_.record(commandBuffer, imageIndex, extent,
                     frame.vertices.handle(), frame.indices.handle(),
                     static_cast<std::uint32_t>(batch_.indices.size()));
}

} // namespace Engine::UI
