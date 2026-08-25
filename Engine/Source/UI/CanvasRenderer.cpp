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
                            Assets::AssetManager& assets,
                            const VkImageView fontAtlasView,
                            const VkSampler fontAtlasSampler,
                            const VmaAllocator allocator) {
    if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
        framesInFlight == 0) {
        throw std::invalid_argument("Canvas renderer received incomplete resources");
    }

    destroy();
    physicalDevice_ = physicalDevice;
    device_ = device;
    allocator_ = allocator;
    try {
        pipeline_.create(device_, colorFormat, extent, imageViews, assets,
                         fontAtlasView, fontAtlasSampler);
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
    sortedRootElements_.clear();
    rootElementsSource_.clear();
    sortedChildren_.clear();
    pipeline_.destroy();
    device_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
    allocator_ = VK_NULL_HANDLE;
    cachedCanvasRevision_ = 0;
    batchDirty_ = true;
    pendingFrameUploads_ = 0;
}

void CanvasRenderer::sortIfNeeded(const std::vector<const UIElement*>& source,
                                  std::vector<const UIElement*>& cache) {
    bool valid = cache.size() == source.size();
    if (valid) {
        std::unordered_map<const UIElement*, std::size_t> sourceIndices;
        sourceIndices.reserve(source.size());
        for (std::size_t index = 0; index < source.size(); ++index) {
            sourceIndices.emplace(source[index], index);
        }

        for (std::size_t index = 0; index < cache.size() && valid; ++index) {
            const auto sourceIndex = sourceIndices.find(cache[index]);
            valid = sourceIndex != sourceIndices.end();
            if (!valid) {
                break;
            }
            if (index > 0) {
                const UIElement* const previous = cache[index - 1];
                const UIElement* const current = cache[index];
                valid = previous->sortingOrder < current->sortingOrder ||
                        (previous->sortingOrder == current->sortingOrder &&
                         sourceIndices.at(previous) < sourceIndex->second);
            }
        }
    }

    if (!valid) {
        cache = source;
        std::stable_sort(cache.begin(), cache.end(),
                         [](const UIElement* lhs, const UIElement* rhs) {
                             return lhs->sortingOrder < rhs->sortingOrder;
                         });
    }
}

const std::vector<const UIElement*>&
CanvasRenderer::sortedChildren(const UIElement& element) {
    auto& source = sortedChildren_[&element];
    std::vector<const UIElement*> current;
    current.reserve(element.children().size());
    for (const auto& child : element.children()) {
        current.push_back(child.get());
    }
    sortIfNeeded(current, source);
    return source;
}

void CanvasRenderer::appendElement(const UIElement& element) {
    if (!element.visible) {
        return;
    }
    element.buildGeometry(batch_);

    for (const UIElement* child : sortedChildren(element)) {
        appendElement(*child);
    }
}

bool CanvasRenderer::ensureCapacity(FrameResources& frame) {
    bool recreated = false;
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
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, allocator_);
        recreated = true;
    }
    if (batch_.indices.size() > frame.indexCapacity) {
        frame.indexCapacity = grow(batch_.indices.size());
        frame.indices.createHostVisible(
            physicalDevice_, device_, frame.indexCapacity * sizeof(std::uint32_t),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT, allocator_);
        recreated = true;
    }
    return recreated;
}

void CanvasRenderer::record(const Canvas& canvas,
                            const VkCommandBuffer commandBuffer,
                            const std::uint32_t imageIndex,
                            const std::uint32_t frameIndex,
                            const VkExtent2D extent) {
    const std::uint64_t revision = canvas.revision();
    if (batchDirty_ || revision != cachedCanvasRevision_) {
        batch_.clear();

        rootElementsSource_.clear();
        rootElementsSource_.reserve(canvas.elements().size());
        for (const auto& element : canvas.elements()) {
            rootElementsSource_.push_back(element.get());
        }
        sortIfNeeded(rootElementsSource_, sortedRootElements_);
        for (const UIElement* element : sortedRootElements_) {
            appendElement(*element);
        }
        cachedCanvasRevision_ = revision;
        batchDirty_ = false;
        pendingFrameUploads_ = (std::uint64_t{1} << frames_.size()) - 1;
    }

    FrameResources& frame = *frames_.at(frameIndex);
    if (!batch_.empty()) {
        const bool recreated = ensureCapacity(frame);
        const std::uint64_t frameBit = std::uint64_t{1} << frameIndex;
        if (recreated || (pendingFrameUploads_ & frameBit) != 0) {
            frame.vertices.update(batch_.vertices.data(),
                                  batch_.vertices.size() * sizeof(UIVertex));
            frame.indices.update(batch_.indices.data(),
                                 batch_.indices.size() * sizeof(std::uint32_t));
            pendingFrameUploads_ &= ~frameBit;
        }
    }

    pipeline_.record(commandBuffer, imageIndex, extent,
                     frame.vertices.handle(), frame.indices.handle(),
                     static_cast<std::uint32_t>(batch_.indices.size()));
}

} // namespace Engine::UI
