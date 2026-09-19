#include "Engine/Renderer/RenderGraph/RenderGraph.h"

#include <algorithm>
#include <queue>
#include <stdexcept>
#include <unordered_set>

namespace Engine::RenderGraph {
    namespace {
        struct UsageInfo final {
            VkPipelineStageFlags2 stage;
            VkAccessFlags2 access;
            VkImageLayout layout;
            bool write;
        };

        [[nodiscard]] UsageInfo usageInfo(const TextureUsage usage) {
            switch (usage) {
                case TextureUsage::SampledRead: return {
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false,
                    };
                case TextureUsage::SampledReadVertex: return {
                        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false,
                    };
                case TextureUsage::SampledReadFragment: return {
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false,
                    };
                case TextureUsage::SampledReadCompute: return {
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false,
                    };
                case TextureUsage::DepthReadFragment: return {
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, false,
                    };
                case TextureUsage::StorageRead: return {
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, false,
                    };
                case TextureUsage::StorageWrite: return {
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, true,
                    };
                case TextureUsage::StorageReadCompute: return {
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                        VK_IMAGE_LAYOUT_GENERAL, false,
                    };
                case TextureUsage::StorageWriteCompute: return {
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_IMAGE_LAYOUT_GENERAL, true,
                    };
                case TextureUsage::ColorAttachment: return {
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true,
                    };
                case TextureUsage::DepthAttachment: return {
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, true,
                    };
                case TextureUsage::TransferRead: return {
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false,
                    };
                case TextureUsage::TransferWrite: return {
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true,
                    };
                case TextureUsage::Present: return {
                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, false,
                    };
                case TextureUsage::ExternalRead: return {
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_MEMORY_READ_BIT,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false,
                    };
            }
            throw std::logic_error("Unknown texture usage");
        }

        struct BufferUsageInfo final {
            VkPipelineStageFlags2 stage;
            VkAccessFlags2 access;
            bool write;
        };

        [[nodiscard]] BufferUsageInfo bufferUsageInfo(const BufferUsage usage) {
            switch (usage) {
                case BufferUsage::UniformRead: return {
                        VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_UNIFORM_READ_BIT, false,
                    };
                case BufferUsage::StorageRead: return {
                        VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT, false,
                    };
                case BufferUsage::StorageWrite: return {
                        VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, true,
                    };
                case BufferUsage::StorageReadCompute: return {
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, false,
                    };
                case BufferUsage::StorageWriteCompute: return {
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, true,
                    };
                case BufferUsage::StorageReadFragment: return {
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, false,
                    };
                case BufferUsage::StorageWriteFragment: return {
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, true,
                    };
                case BufferUsage::VertexRead: return {
                        VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, false,
                    };
                case BufferUsage::IndexRead: return {
                        VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT, false,
                    };
                case BufferUsage::MeshRead: return {
                        VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, false,
                    };
                case BufferUsage::IndirectRead: return {
                        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, false,
                    };
                case BufferUsage::TransferRead: return {
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false,
                    };
                case BufferUsage::TransferWrite: return {
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true,
                    };
                case BufferUsage::ExternalRead: return {
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, false,
                    };
            }
            throw std::logic_error("Unknown buffer usage");
        }

        [[nodiscard]] TextureSubresourceRange normalizeRange(const TextureDesc &desc, TextureSubresourceRange range) {
            if (range.levelCount == 0) range.levelCount = desc.mipLevels - range.baseMipLevel;
            if (range.layerCount == 0) range.layerCount = desc.arrayLayers - range.baseArrayLayer;
            if (range.baseMipLevel >= desc.mipLevels || range.baseArrayLayer >= desc.arrayLayers ||
                range.levelCount > desc.mipLevels - range.baseMipLevel ||
                range.layerCount > desc.arrayLayers - range.baseArrayLayer) {
                throw std::out_of_range("RenderGraph texture subresource range is out of bounds");
            }
            return range;
        }

        void mergeImageBarriers(std::vector<VkImageMemoryBarrier2> &barriers,
                                std::vector<std::uint32_t>& resources) {
            // One state record is produced per mip/layer. Fold adjacent layers,
            // then equal layer spans in adjacent mip levels, into one dependency.
            for (std::size_t index = 0; index < barriers.size(); ++index) {
                for (std::size_t next = index + 1; next < barriers.size();) {
                    auto &left = barriers[index];
                    const auto &right = barriers[next];
                    const bool same = resources[index] == resources[next] &&
                        left.srcStageMask == right.srcStageMask && left.srcAccessMask == right.srcAccessMask &&
                        left.dstStageMask == right.dstStageMask && left.dstAccessMask == right.dstAccessMask &&
                        left.oldLayout == right.oldLayout && left.newLayout == right.newLayout && left.image == right.image &&
                        left.srcQueueFamilyIndex == right.srcQueueFamilyIndex && left.dstQueueFamilyIndex == right.dstQueueFamilyIndex &&
                        left.subresourceRange.aspectMask == right.subresourceRange.aspectMask;
                    if (same && left.subresourceRange.baseMipLevel == right.subresourceRange.baseMipLevel &&
                        left.subresourceRange.baseArrayLayer + left.subresourceRange.layerCount == right.subresourceRange.baseArrayLayer) {
                        left.subresourceRange.layerCount += right.subresourceRange.layerCount;
                        barriers.erase(barriers.begin() + static_cast<std::ptrdiff_t>(next));
                        resources.erase(resources.begin() + static_cast<std::ptrdiff_t>(next));
                    } else {
                        ++next;
                    }
                }
            }
            for (std::size_t index = 0; index < barriers.size(); ++index) {
                for (std::size_t next = index + 1; next < barriers.size();) {
                    auto &left = barriers[index];
                    const auto &right = barriers[next];
                    const bool same = resources[index] == resources[next] &&
                        left.srcStageMask == right.srcStageMask && left.srcAccessMask == right.srcAccessMask &&
                        left.dstStageMask == right.dstStageMask && left.dstAccessMask == right.dstAccessMask && left.oldLayout == right.oldLayout &&
                        left.newLayout == right.newLayout && left.image == right.image && left.srcQueueFamilyIndex == right.srcQueueFamilyIndex &&
                        left.dstQueueFamilyIndex == right.dstQueueFamilyIndex && left.subresourceRange.aspectMask == right.subresourceRange.aspectMask &&
                        left.subresourceRange.baseArrayLayer == right.subresourceRange.baseArrayLayer &&
                        left.subresourceRange.layerCount == right.subresourceRange.layerCount;
                    if (same && left.subresourceRange.baseMipLevel + left.subresourceRange.levelCount == right.subresourceRange.baseMipLevel) {
                        left.subresourceRange.levelCount += right.subresourceRange.levelCount;
                        barriers.erase(barriers.begin() + static_cast<std::ptrdiff_t>(next));
                        resources.erase(resources.begin() + static_cast<std::ptrdiff_t>(next));
                    } else {
                        ++next;
                    }
                }
            }
        }
    }

    bool TextureDesc::compatibleWith(const TextureDesc &other) const noexcept {
        return extent.width == other.extent.width && extent.height == other.extent.height &&
               extent.depth == other.extent.depth && format == other.format && usage == other.usage &&
               aspect == other.aspect && mipLevels == other.mipLevels && arrayLayers == other.arrayLayers &&
               samples == other.samples;
    }

    bool BufferDesc::compatibleWith(const BufferDesc &other) const noexcept {
        return usage == other.usage && size >= other.size;
    }

    void PassBuilder::read(const TextureHandle texture, const TextureUsage usage) {
        read(texture, usage, {});
    }
    void PassBuilder::read(const TextureHandle texture, const TextureUsage usage, const TextureSubresourceRange range) {
        graph_.addAccess(pass_, texture, usage, range, false);
    }

    void PassBuilder::write(const TextureHandle texture, const TextureUsage usage) {
        write(texture, usage, {});
    }
    void PassBuilder::write(const TextureHandle texture, const TextureUsage usage, const TextureSubresourceRange range) {
        graph_.addAccess(pass_, texture, usage, range, true);
    }

    void PassBuilder::setFinalTextureState(const TextureHandle texture, const TextureState state) {
        setFinalTextureState(texture, state, {});
    }
    void PassBuilder::setFinalTextureState(const TextureHandle texture, const TextureState state, const TextureSubresourceRange range) {
        graph_.addFinalTextureState(pass_, texture, state, range);
    }

    TextureHandle PassBuilder::writeTexture(std::string name, const TextureDesc &desc, const TextureUsage usage) {
        return graph_.addTransient(std::move(name), desc, pass_, usage);
    }

    void PassBuilder::read(const BufferHandle buffer, const BufferUsage usage) {
        graph_.addBufferAccess(pass_, buffer, usage, false);
    }

    void PassBuilder::write(const BufferHandle buffer, const BufferUsage usage) {
        graph_.addBufferAccess(pass_, buffer, usage, true);
    }

    BufferHandle PassBuilder::writeBuffer(std::string name, const BufferDesc &desc, const BufferUsage usage) {
        const auto buffer = graph_.createBuffer(std::move(name), desc);
        graph_.addBufferAccess(pass_, buffer, usage, true);
        return buffer;
    }

    void PassBuilder::setSideEffect() {
        graph_.passes_[pass_].sideEffect = true;
    }

    TextureHandle RenderGraph::importTexture(std::string name, const VkImage image, const TextureDesc &desc,
                                             const TextureState initialState) {
        if (image == VK_NULL_HANDLE) {
            throw std::invalid_argument("Cannot import a null image into RenderGraph");
        }
        if (compiled_) {
            throw std::logic_error("Reset RenderGraph before adding resources");
        }
        resources_.push_back({std::move(name), desc, image, initialState, true});
        return {static_cast<std::uint32_t>(resources_.size() - 1)};
    }

    TextureHandle RenderGraph::createTexture(std::string name, const TextureDesc &desc) {
        if (compiled_) {
            throw std::logic_error("Reset RenderGraph before adding resources");
        }
        resources_.push_back({std::move(name), desc});
        return {static_cast<std::uint32_t>(resources_.size() - 1)};
    }

    BufferHandle RenderGraph::importBuffer(std::string name, const VkBuffer buffer, const BufferDesc &desc) {
        if (buffer == VK_NULL_HANDLE) {
            throw std::invalid_argument("Cannot import a null buffer into RenderGraph");
        }
        if (compiled_) {
            throw std::logic_error("Reset RenderGraph before adding resources");
        }
        buffers_.push_back({std::move(name), desc, buffer, true});
        return {static_cast<std::uint32_t>(buffers_.size() - 1)};
    }

    BufferHandle RenderGraph::createBuffer(std::string name, const BufferDesc &desc) {
        if (desc.size == 0 || desc.usage == 0) {
            throw std::invalid_argument("RenderGraph buffer needs size and usage");
        }
        if (compiled_) {
            throw std::logic_error("Reset RenderGraph before adding resources");
        }
        buffers_.push_back({std::move(name), desc});
        return {static_cast<std::uint32_t>(buffers_.size() - 1)};
    }

    void RenderGraph::exportTexture(const TextureHandle texture) {
        requireValid(texture);
        if (compiled_) {
            throw std::logic_error("Reset RenderGraph before exporting resources");
        }
        if (std::ranges::find(exportedTextures_, texture) == exportedTextures_.end()) {
            exportedTextures_.
                    push_back(texture);
        }
    }

    void RenderGraph::exportBuffer(const BufferHandle buffer) {
        requireValid(buffer);
        if (compiled_) {
            throw std::logic_error("Reset RenderGraph before exporting resources");
        }
        if (std::ranges::find(exportedBuffers_, buffer) == exportedBuffers_.end()) {
            exportedBuffers_.push_back(buffer);
        }
    }

    void RenderGraph::enablePassCulling(const bool enabled) noexcept { passCullingEnabled_ = enabled; }

    void RenderGraph::markUploaded(const TextureHandle texture, const std::uint64_t timelineValue) {
        requireValid(texture);
        if (compiled_) {
            throw std::logic_error("Reset RenderGraph before marking an uploaded texture");
        }
        if (!resources_[texture.index].imported) {
            throw std::invalid_argument("Only imported textures can have an upload ticket");
        }
        resources_[texture.index].uploadTimeline = timelineValue;
    }

    void RenderGraph::markUploaded(const BufferHandle buffer, const std::uint64_t timelineValue) {
        requireValid(buffer);
        if (compiled_) {
            throw std::logic_error("Reset RenderGraph before marking an uploaded buffer");
        }
        if (!buffers_[buffer.index].imported) {
            throw std::invalid_argument("Only imported buffers can have an upload ticket");
        }
        buffers_[buffer.index].uploadTimeline = timelineValue;
    }

    void RenderGraph::initialize(const VkDevice device, const VmaAllocator allocator) {
        if (compiled_ || !transientImageSlots_.empty() || !transientBufferSlots_.empty()) {
            throw std::logic_error(
                "Reset RenderGraph before changing its allocator");
        }
        if ((!transientAllocations_.empty() || !transientBufferAllocations_.empty()) &&
            (device != device_ || allocator != allocator_)) {
            throw std::logic_error(
                "Trim RenderGraph's transient pool before changing its allocator");
        }
        if (device == VK_NULL_HANDLE || allocator == VK_NULL_HANDLE) {
            throw std::invalid_argument(
                "RenderGraph requires a valid Vulkan device and VMA allocator");
        }
        device_ = device;
        allocator_ = allocator;
    }

    void RenderGraph::addPass(std::string name, const std::function<void(PassBuilder &)> &setup,
                              ExecuteCallback execute) {
        addPass(std::move(name), Queue::Graphics, setup, std::move(execute));
    }

    void RenderGraph::addPass(std::string name, const Queue queue, const std::function<void(PassBuilder &)> &setup,
                              ExecuteCallback execute) {
        if (compiled_) {
            throw std::logic_error("Reset RenderGraph before adding passes");
        }
        passes_.push_back({std::move(name), queue, {}, {}, {}, false, std::move(execute)});
        PassBuilder builder{*this, static_cast<std::uint32_t>(passes_.size() - 1)};
        setup(builder);
    }

    void RenderGraph::setQueueFamily(const Queue queue, const std::uint32_t family) noexcept {
        queueFamilies_[static_cast<std::uint32_t>(queue)] = family;
    }

    FirstConsumer RenderGraph::firstConsumer(const TextureHandle texture) const {
        requireValid(texture);
        if (!compiled_) {
            throw std::logic_error("Compile RenderGraph before querying a texture consumer");
        }
        for (const auto pass: order_) {
            VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
            bool overwritesContents = false;
            for (const auto &access: passes_[pass].accesses) {
                if (access.texture == texture) {
                    if (access.write) {
                        overwritesContents = true;
                    } else {
                        stages |= usageInfo(access.usage).stage;
                    }
                }
            }
            if (stages != VK_PIPELINE_STAGE_2_NONE) {
                return {stages, passes_[pass].queue};
            }
            if (overwritesContents) {
                return {};
            }
        }
        return {};
    }

    FirstConsumer RenderGraph::firstConsumer(const BufferHandle buffer) const {
        requireValid(buffer);
        if (!compiled_) {
            throw std::logic_error("Compile RenderGraph before querying a buffer consumer");
        }
        for (const auto pass: order_) {
            VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
            bool overwritesContents = false;
            for (const auto &access: passes_[pass].bufferAccesses) {
                if (access.buffer == buffer) {
                    if (access.write) {
                        overwritesContents = true;
                    } else {
                        stages |= bufferUsageInfo(access.usage).stage;
                    }
                }
            }
            if (stages != VK_PIPELINE_STAGE_2_NONE) {
                return {stages, passes_[pass].queue};
            }
            if (overwritesContents) {
                return {};
            }
        }
        return {};
    }

    const std::vector<UploadWait> &RenderGraph::uploadWaits() const noexcept { return uploadWaits_; }

    void RenderGraph::requireValid(const TextureHandle texture) const {
        if (!texture || texture.index >= resources_.size()) {
            throw std::out_of_range(
                "Invalid RenderGraph texture handle");
        }
    }

    void RenderGraph::requireValid(const BufferHandle buffer) const {
        if (!buffer || buffer.index >= buffers_.size()) {
            throw std::out_of_range("Invalid RenderGraph buffer handle");
        }
    }

    void RenderGraph::addAccess(const std::uint32_t pass, const TextureHandle texture, const TextureUsage usage,
                                TextureSubresourceRange range, const bool write) {
        requireValid(texture);
        if (pass >= passes_.size()) {
            throw std::logic_error("Invalid RenderGraph pass");
        }
        if (usageInfo(usage).write != write && !(usage == TextureUsage::ColorAttachment && write) && !(
                usage == TextureUsage::DepthAttachment && write)) {
            throw std::invalid_argument("Texture usage does not match read/write declaration");
        }
        passes_[pass].accesses.push_back({texture, usage, normalizeRange(resources_[texture.index].desc, range), write});
    }

    void RenderGraph::addFinalTextureState(const std::uint32_t pass, const TextureHandle texture,
                                           const TextureState state, TextureSubresourceRange range) {
        requireValid(texture);
        if (pass >= passes_.size()) {
            throw std::logic_error("Invalid RenderGraph pass");
        }
        if (compiled_) {
            throw std::logic_error("Reset RenderGraph before adding accesses");
        }
        passes_[pass].finalTextureStates.push_back({texture, state, normalizeRange(resources_[texture.index].desc, range)});
    }

    void RenderGraph::addBufferAccess(const std::uint32_t pass, const BufferHandle buffer, const BufferUsage usage,
                                      const bool write) {
        requireValid(buffer);
        if (pass >= passes_.size()) {
            throw std::logic_error("Invalid RenderGraph pass");
        }
        if (bufferUsageInfo(usage).write != write) {
            throw std::invalid_argument(
                "Buffer usage does not match read/write declaration");
        }
        passes_[pass].bufferAccesses.push_back({buffer, usage, write});
    }

    TextureHandle RenderGraph::addTransient(std::string name, const TextureDesc &desc, const std::uint32_t pass,
                                            const TextureUsage usage) {
        const TextureHandle texture = createTexture(std::move(name), desc);
        addAccess(pass, texture, usage, {}, true);
        return texture;
    }

    void RenderGraph::compile() {
        if (compiled_) {
            return;
        }
        const auto count = static_cast<std::uint32_t>(passes_.size());
        // Hash only declarative structure, never VkImage/VkBuffer handles:
        // imported physical resources are expected to change per frame.
        std::uint64_t signature = 1469598103934665603ULL;
        const auto mix = [&signature](const std::uint64_t value) {
            signature ^= value;
            signature *= 1099511628211ULL;
        };
        mix(resources_.size());
        mix(buffers_.size());
        mix(count);
        mix(passCullingEnabled_);
        for (const auto family: queueFamilies_) mix(family);
        for (const auto &resource: resources_) {
            mix(resource.imported);
            mix(resource.desc.extent.width);
            mix(resource.desc.extent.height);
            mix(resource.desc.extent.depth);
            mix(resource.desc.format);
            mix(resource.desc.usage);
            mix(resource.desc.aspect);
            mix(resource.desc.mipLevels);
            mix(resource.desc.arrayLayers);
            mix(resource.desc.samples);
            mix(resource.initialState.stage);
            mix(resource.initialState.access);
            mix(resource.initialState.layout);
            mix(resource.initialState.write);
        }
        for (const auto &resource: buffers_) {
            mix(static_cast<std::uint64_t>(resource.imported));
            mix(resource.desc.size);
            mix(resource.desc.usage);
        }
        for (const auto &pass: passes_) {
            mix(static_cast<std::uint8_t>(pass.queue));
            mix(pass.accesses.size());
            mix(pass.finalTextureStates.size());
            mix(pass.bufferAccesses.size());
            mix(pass.sideEffect);
            for (const auto &access: pass.accesses) {
                mix(access.texture.index);
                mix(static_cast<std::uint8_t>(access.usage));
                mix(access.range.baseMipLevel);
                mix(access.range.levelCount);
                mix(access.range.baseArrayLayer);
                mix(access.range.layerCount);
                mix(access.write);
            }
            for (const auto &access: pass.bufferAccesses) {
                mix(access.buffer.index);
                mix(static_cast<std::uint8_t>(access.usage));
                mix(access.write);
            }
            for (const auto &state: pass.finalTextureStates) {
                mix(state.texture.index);
                mix(state.state.stage);
                mix(state.state.access);
                mix(state.state.layout);
                mix(state.state.write);
                mix(state.range.baseMipLevel);
                mix(state.range.levelCount);
                mix(state.range.baseArrayLayer);
                mix(state.range.layerCount);
            }
        }
        mix(exportedTextures_.size());
        for (const auto texture: exportedTextures_) mix(texture.index);
        mix(exportedBuffers_.size());
        for (const auto buffer: exportedBuffers_) mix(buffer.index);

        std::vector<std::uint32_t> imageAliasPredecessor;
        std::vector<std::uint32_t> bufferAliasPredecessor;
        std::vector<TextureDesc> cachedImageSlotDescs;
        std::vector<BufferDesc> cachedBufferSlotDescs;
        std::vector<UploadConsumer> textureUploadConsumers;
        std::vector<UploadConsumer> bufferUploadConsumers;
        const auto cachedTemplate = std::find_if(compiledTemplates_.begin(), compiledTemplates_.end(),
            [signature](const CompiledTemplate& plan) { return plan.valid && plan.signature == signature; });
        const bool templateHit = cachedTemplate != compiledTemplates_.end();
        if (templateHit) {
            order_ = cachedTemplate->order;
            queueDependencies_ = cachedTemplate->queueDependencies;
            queueBatches_ = cachedTemplate->queueBatches;
            imageAliasPredecessor = cachedTemplate->imageAliasPredecessors;
            bufferAliasPredecessor = cachedTemplate->bufferAliasPredecessors;
            cachedImageSlotDescs = cachedTemplate->imageSlotDescs;
            cachedBufferSlotDescs = cachedTemplate->bufferSlotDescs;
            textureUploadConsumers = cachedTemplate->textureUploadConsumers;
            bufferUploadConsumers = cachedTemplate->bufferUploadConsumers;
            for (std::uint32_t resource = 0; resource < resources_.size(); ++resource)
                resources_[resource].lifetime = cachedTemplate->textureLifetimes[resource];
            for (std::uint32_t resource = 0; resource < buffers_.size(); ++resource)
                buffers_[resource].lifetime = cachedTemplate->bufferLifetimes[resource];
            orderNames_.clear();
            for (const auto pass: order_) orderNames_.push_back(passes_[pass].name);
            allocateTransients(cachedImageSlotDescs);
            allocateTransientBuffers(cachedBufferSlotDescs);
            barriers_ = cachedTemplate->barriers;
            releaseBarriers_ = cachedTemplate->releaseBarriers;
            const auto rebindBarriers = [this](std::vector<BarrierBatch>& batches) {
                for (auto& batch: batches) {
                    for (std::size_t index = 0; index < batch.images.size(); ++index)
                        batch.images[index].image = resources_[batch.imageResources[index]].image;
                    for (std::size_t index = 0; index < batch.buffers.size(); ++index)
                        batch.buffers[index].buffer = buffers_[batch.bufferResources[index]].buffer;
                }
            };
            rebindBarriers(barriers_);
            rebindBarriers(releaseBarriers_);
        } else {
        order_.clear();
        orderNames_.clear();
        const auto cachedTopology = std::find_if(topologyCaches_.begin(), topologyCaches_.end(),
                                                 [signature](const TopologyCache &cache) {
                                                     return cache.valid && cache.signature == signature;
                                                 });
        if (cachedTopology != topologyCaches_.end()) {
            order_ = cachedTopology->order;
        } else {
            std::vector<std::unordered_set<std::uint32_t> > edges(count);
            std::vector<std::uint32_t> indegree(count);
            std::vector<std::vector<std::int32_t>> lastWriter(resources_.size());
            std::vector<std::vector<std::vector<std::uint32_t>>> readers(resources_.size());
            for (std::uint32_t resource = 0; resource < resources_.size(); ++resource) {
                const auto subresourceCount = static_cast<std::size_t>(resources_[resource].desc.mipLevels) * resources_[resource].desc.arrayLayers;
                lastWriter[resource].assign(subresourceCount, -1);
                readers[resource].resize(subresourceCount);
            }
            std::vector<std::int32_t> lastBufferWriter(buffers_.size(), -1);
            std::vector<std::vector<std::uint32_t> > bufferReaders(buffers_.size());
            for (std::uint32_t pass = 0; pass < count; ++pass) {
                for (const Access &access: passes_[pass].accesses) {
                    const auto resource = access.texture.index;
                    const auto &desc = resources_[resource].desc;
                    for (std::uint32_t mip = access.range.baseMipLevel; mip < access.range.baseMipLevel + access.range.levelCount; ++mip) {
                        for (std::uint32_t layer = access.range.baseArrayLayer; layer < access.range.baseArrayLayer + access.range.layerCount; ++layer) {
                            const auto subresource = static_cast<std::size_t>(mip) * desc.arrayLayers + layer;
                            if (!access.write) {
                                if (lastWriter[resource][subresource] >= 0 && static_cast<std::uint32_t>(lastWriter[resource][subresource]) != pass) {
                                    edges[static_cast<std::uint32_t>(lastWriter[resource][subresource])].insert(pass);
                                } else if (lastWriter[resource][subresource] < 0 && !resources_[resource].imported) {
                                    throw std::logic_error("Transient texture read before it is written: " + resources_[resource].name);
                                }
                                readers[resource][subresource].push_back(pass);
                            } else {
                                if (lastWriter[resource][subresource] >= 0 && static_cast<std::uint32_t>(lastWriter[resource][subresource]) != pass) {
                                    edges[static_cast<std::uint32_t>(lastWriter[resource][subresource])].insert(pass);
                                }
                                for (const auto reader: readers[resource][subresource]) if (reader != pass) edges[reader].insert(pass);
                                readers[resource][subresource].clear();
                                lastWriter[resource][subresource] = static_cast<std::int32_t>(pass);
                            }
                        }
                    }
                }
                for (const BufferAccess &access: passes_[pass].bufferAccesses) {
                    const auto resource = access.buffer.index;
                    if (!access.write) {
                        if (lastBufferWriter[resource] >= 0) {
                            edges[static_cast<std::uint32_t>(lastBufferWriter[
                                resource])].insert(pass);
                        } else if (!buffers_[resource].imported) {
                            throw std::logic_error(
                                "Transient buffer read before it is written: " + buffers_[resource].name);
                        }
                        bufferReaders[resource].push_back(pass);
                    } else {
                        if (lastBufferWriter[resource] >= 0) {
                            edges[static_cast<std::uint32_t>(lastBufferWriter[
                                resource])].insert(pass);
                        }
                        for (const auto reader: bufferReaders[resource]) {
                            edges[reader].insert(pass);
                        }
                        bufferReaders[resource].clear();
                        lastBufferWriter[resource] = static_cast<std::int32_t>(pass);
                    }
                }
            }
            for (const auto &sources: edges) {
                for (const auto target: sources) {
                    ++indegree[target];
                }
            }
            std::queue<std::uint32_t> ready;
            for (std::uint32_t pass = 0; pass < count; ++pass) {
                if (indegree[pass] == 0) {
                    ready.push(pass);
                }
            }
            while (!ready.empty()) {
                const auto pass = ready.front();
                ready.pop();
                order_.push_back(pass);
                for (const auto target: edges[pass]) {
                    if (--indegree[target] == 0) {
                        ready.push(target);
                    }
                }
            }
            if (order_.size() != passes_.size()) {
                throw std::logic_error("RenderGraph contains a dependency cycle");
            }
            topologyCaches_.push_back({signature, order_, true});
        }
        // Exports and explicitly declared external side effects are real graph
        // roots. A side-effect root is the escape hatch required while a
        // callback is being migrated; it must be declared rather than making
        // every pass implicitly live and defeating culling.
        const bool hasSideEffect = std::ranges::any_of(passes_, [](const Pass& pass) { return pass.sideEffect; });
        if (passCullingEnabled_ && (!exportedTextures_.empty() || !exportedBuffers_.empty() || hasSideEffect)) {
            std::vector<std::vector<std::uint32_t> > dependencies(count);
            std::vector<std::int32_t> writer(resources_.size(), -1);
            std::vector<std::vector<std::uint32_t> > readers(resources_.size());
            std::vector<std::int32_t> bufferWriter(buffers_.size(), -1);
            std::vector<std::vector<std::uint32_t> > bufferReaders(buffers_.size());
            const auto addDependency = [&dependencies](const std::int32_t producer, const std::uint32_t consumer) {
                if (producer >= 0 && static_cast<std::uint32_t>(producer) != consumer) {
                    dependencies[consumer].push_back(static_cast<std::uint32_t>(producer));
                }
            };
            for (std::uint32_t pass = 0; pass < count; ++pass) {
                for (const auto &access: passes_[pass].accesses) {
                    const auto resource = access.texture.index;
                    if (!access.write) {
                        addDependency(writer[resource], pass);
                        readers[resource].push_back(pass);
                    } else {
                        addDependency(writer[resource], pass);
                        for (const auto reader: readers[resource]) {
                            addDependency(
                                static_cast<std::int32_t>(reader), pass);
                        }
                        readers[resource].clear();
                        writer[resource] = static_cast<std::int32_t>(pass);
                    }
                }
                for (const auto &access: passes_[pass].bufferAccesses) {
                    const auto resource = access.buffer.index;
                    if (!access.write) {
                        addDependency(bufferWriter[resource], pass);
                        bufferReaders[resource].push_back(pass);
                    } else {
                        addDependency(bufferWriter[resource], pass);
                        for (const auto reader: bufferReaders[resource]) {
                            addDependency(
                                static_cast<std::int32_t>(reader), pass);
                        }
                        bufferReaders[resource].clear();
                        bufferWriter[resource] = static_cast<std::int32_t>(pass);
                    }
                }
            }
            std::vector<bool> live(count);
            std::vector<std::uint32_t> pending;
            for (const auto texture: exportedTextures_) {
                if (writer[texture.index] >= 0) {
                    pending.push_back(
                        static_cast<std::uint32_t>(writer[texture.index]));
                }
            }
            for (const auto buffer: exportedBuffers_) {
                if (bufferWriter[buffer.index] >= 0) {
                    pending.push_back(
                        static_cast<std::uint32_t>(bufferWriter[buffer.index]));
                }
            }
            for (std::uint32_t pass = 0; pass < count; ++pass) {
                if (passes_[pass].sideEffect) pending.push_back(pass);
            }
            while (!pending.empty()) {
                const auto pass = pending.back();
                pending.pop_back();
                if (live[pass]) {
                    continue;
                }
                live[pass] = true;
                pending.insert(pending.end(), dependencies[pass].begin(), dependencies[pass].end());
            }
            std::erase_if(order_, [&live](const std::uint32_t pass) { return !live[pass]; });
        }
        for (const auto pass: order_) {
            orderNames_.push_back(passes_[pass].name);
        }

        // Keep the cross-queue edges separately from the topological order.
        // The submitter turns each of these into a timeline wait/signal pair;
        // multiple resource hazards between the same two passes collapse here.
        queueDependencies_.clear();
        std::unordered_set<std::uint64_t> seenQueueEdges;
        const auto isLive = [this](const std::uint32_t pass) {
            return std::ranges::find(order_, pass) != order_.end();
        };
        const auto addQueueEdge = [&](const std::uint32_t producer, const std::uint32_t consumer) {
            if (!isLive(producer) || !isLive(consumer)) {
                return;
            }
            if (producer == consumer || passes_[producer].queue == passes_[consumer].queue) {
                return;
            }
            const auto key = (static_cast<std::uint64_t>(producer) << 32U) | consumer;
            if (seenQueueEdges.insert(key).second) {
                queueDependencies_.push_back({producer, consumer, passes_[producer].queue, passes_[consumer].queue});
            }
        };
        std::vector<std::int32_t> dependencyWriter(resources_.size(), -1);
        std::vector<std::vector<std::uint32_t> > dependencyReaders(resources_.size());
        std::vector<std::int32_t> dependencyBufferWriter(buffers_.size(), -1);
        std::vector<std::vector<std::uint32_t> > dependencyBufferReaders(buffers_.size());
        for (std::uint32_t pass = 0; pass < count; ++pass) {
            for (const auto &access: passes_[pass].accesses) {
                const auto resource = access.texture.index;
                if (!access.write) {
                    if (dependencyWriter[resource] >= 0) {
                        addQueueEdge(dependencyWriter[resource], pass);
                    }
                    dependencyReaders[resource].push_back(pass);
                } else {
                    if (dependencyWriter[resource] >= 0) {
                        addQueueEdge(dependencyWriter[resource], pass);
                    }
                    for (const auto reader: dependencyReaders[resource]) {
                        addQueueEdge(reader, pass);
                    }
                    dependencyReaders[resource].clear();
                    dependencyWriter[resource] = static_cast<std::int32_t>(pass);
                }
            }
            for (const auto &access: passes_[pass].bufferAccesses) {
                const auto resource = access.buffer.index;
                if (!access.write) {
                    if (dependencyBufferWriter[resource] >= 0) {
                        addQueueEdge(dependencyBufferWriter[resource], pass);
                    }
                    dependencyBufferReaders[resource].push_back(pass);
                } else {
                    if (dependencyBufferWriter[resource] >= 0) {
                        addQueueEdge(dependencyBufferWriter[resource], pass);
                    }
                    for (const auto reader: dependencyBufferReaders[resource]) {
                        addQueueEdge(reader, pass);
                    }
                    dependencyBufferReaders[resource].clear();
                    dependencyBufferWriter[resource] = static_cast<std::int32_t>(pass);
                }
            }
        }

        // Submission planning belongs to the graph too. A contiguous queue
        // run is one command-recording/submission batch; cross-queue hazards
        // become waits on the producer batch rather than renderer-specific
        // special cases such as the old Hi-Z split.
        queueBatches_.clear();
        std::vector<std::uint32_t> passToBatch(count, std::numeric_limits<std::uint32_t>::max());
        for (const auto pass: order_) {
            if (queueBatches_.empty() || queueBatches_.back().queue != passes_[pass].queue)
                queueBatches_.push_back({.queue = passes_[pass].queue});
            const auto batch = static_cast<std::uint32_t>(queueBatches_.size() - 1);
            queueBatches_.back().passes.push_back(pass);
            passToBatch[pass] = batch;
        }
        for (const auto &dependency: queueDependencies_) {
            const auto producer = passToBatch[dependency.producerPass];
            const auto consumer = passToBatch[dependency.consumerPass];
            if (producer == consumer || producer == std::numeric_limits<std::uint32_t>::max() ||
                consumer == std::numeric_limits<std::uint32_t>::max())
                continue;
            auto &waits = queueBatches_[consumer].waitBatches;
            if (std::ranges::find(waits, producer) == waits.end()) waits.push_back(producer);
        }

        textureUploadConsumers.assign(resources_.size(), {});
        bufferUploadConsumers.assign(buffers_.size(), {});

        // An imported upload is an external producer. Wait at its first read,
        // not at the beginning of the frame; an earlier write discards it.
        uploadWaits_.clear();
        const auto addUploadWait = [this, &passToBatch](const std::uint64_t timelineValue,
                                                        const std::uint32_t pass,
                                                        const VkPipelineStageFlags2 stage) {
            if (timelineValue == 0 || stage == VK_PIPELINE_STAGE_2_NONE) {
                return;
            }
            const auto batch = passToBatch[pass];
            if (batch == std::numeric_limits<std::uint32_t>::max()) {
                return;
            }
            const auto existing = std::ranges::find_if(uploadWaits_, [batch](const UploadWait &wait) {
                return wait.batch == batch;
            });
            if (existing != uploadWaits_.end()) {
                existing->timelineValue = std::max(existing->timelineValue, timelineValue);
                existing->stage |= stage;
            } else {
                uploadWaits_.push_back({timelineValue, stage, queueBatches_[batch].queue, batch});
            }
        };
        for (std::uint32_t resource = 0; resource < resources_.size(); ++resource) {
            if (resources_[resource].uploadTimeline == 0) {
                continue;
            }
            for (const auto pass: order_) {
                VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
                bool overwritesContents = false;
                for (const auto &access: passes_[pass].accesses) {
                    if (access.texture.index == resource) {
                        if (access.write) overwritesContents = true;
                        else {
                            stages |= usageInfo(access.usage).stage;
                        }
                    }
                }
                if (stages != VK_PIPELINE_STAGE_2_NONE) {
                    textureUploadConsumers[resource] = {passToBatch[pass], stages};
                    addUploadWait(resources_[resource].uploadTimeline, pass, stages);
                    break;
                }
                if (overwritesContents) {
                    break;
                }
            }
        }
        for (std::uint32_t resource = 0; resource < buffers_.size(); ++resource) {
            if (buffers_[resource].uploadTimeline == 0) {
                continue;
            }
            for (const auto pass: order_) {
                VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
                bool overwritesContents = false;
                for (const auto &access: passes_[pass].bufferAccesses) {
                    if (access.buffer.index == resource) {
                        if (access.write) {
                            overwritesContents = true;
                        } else {
                            stages |= bufferUsageInfo(access.usage).stage;
                        }
                    }
                }
                if (stages != VK_PIPELINE_STAGE_2_NONE) {
                    bufferUploadConsumers[resource] = {passToBatch[pass], stages};
                    addUploadWait(buffers_[resource].uploadTimeline, pass, stages);
                    break;
                }
                if (overwritesContents) {
                    break;
                }
            }
        }

        for (auto &resource: resources_) {
            resource.lifetime = {count, 0, 0};
        }
        for (std::uint32_t ordered = 0; ordered < order_.size(); ++ordered) {
            for (const auto &access: passes_[order_[ordered]].accesses) {
                auto &lifetime = resources_[access.texture.index].lifetime;
                lifetime.firstPass = std::min(lifetime.firstPass, ordered);
                lifetime.lastPass = std::max(lifetime.lastPass, ordered);
            }
        }
        std::vector<std::uint32_t> transientResources;
        for (std::uint32_t resource = 0; resource < resources_.size(); ++resource) {
            if (!resources_[resource].imported && resources_[resource].lifetime.firstPass != count) {
                transientResources.push_back(resource);
            }
        }
        std::ranges::sort(transientResources, [this](const std::uint32_t left, const std::uint32_t right) {
            return resources_[left].lifetime.firstPass < resources_[right].lifetime.firstPass;
        });

        std::vector<std::uint32_t> slotsLastUse;
        std::vector<TextureDesc> slotsDesc;
        std::vector<std::uint32_t> slotLastResource;
        imageAliasPredecessor.assign(resources_.size(), std::numeric_limits<std::uint32_t>::max());
        for (const auto resourceIndex: transientResources) {
            auto &resource = resources_[resourceIndex];
            std::uint32_t slot = static_cast<std::uint32_t>(slotsDesc.size());
            for (std::uint32_t candidate = 0; candidate < slotsDesc.size(); ++candidate) {
                if (slotsLastUse[candidate] < resource.lifetime.firstPass && slotsDesc[candidate].
                    compatibleWith(resource.desc)) {
                    slot = candidate;
                    break;
                }
            }
            if (slot == slotsDesc.size()) {
                slotsDesc.push_back(resource.desc);
                slotsLastUse.push_back(resource.lifetime.lastPass);
                slotLastResource.push_back(resourceIndex);
            } else {
                imageAliasPredecessor[resourceIndex] = slotLastResource[slot];
                slotsLastUse[slot] = resource.lifetime.lastPass;
                slotLastResource[slot] = resourceIndex;
            }
            resource.lifetime.allocationSlot = slot;
        }
        allocateTransients(slotsDesc);

        for (auto &resource: buffers_) {
            resource.lifetime = {count, 0, 0};
        }
        for (std::uint32_t ordered = 0; ordered < order_.size(); ++ordered) {
            for (const auto &access: passes_[order_[ordered]].bufferAccesses) {
                auto &lifetime = buffers_[access.buffer.index].lifetime;
                lifetime.firstPass = std::min(lifetime.firstPass, ordered);
                lifetime.lastPass = std::max(lifetime.lastPass, ordered);
            }
        }
        std::vector<std::uint32_t> transientBuffers;
        for (std::uint32_t resource = 0; resource < buffers_.size(); ++resource) {
            if (!buffers_[resource].imported && buffers_[resource].lifetime.firstPass != count) {
                transientBuffers.
                        push_back(resource);
            }
        }
        std::ranges::sort(transientBuffers, [this](const auto left, const auto right) {
            return buffers_[left].lifetime.firstPass < buffers_[right].lifetime.firstPass;
        });
        std::vector<std::uint32_t> bufferSlotsLastUse;
        std::vector<BufferDesc> bufferSlotsDesc;
        std::vector<std::uint32_t> bufferSlotLastResource;
        bufferAliasPredecessor.assign(buffers_.size(), std::numeric_limits<std::uint32_t>::max());
        for (const auto resourceIndex: transientBuffers) {
            auto &resource = buffers_[resourceIndex];
            std::uint32_t slot = static_cast<std::uint32_t>(bufferSlotsDesc.size());
            for (std::uint32_t candidate = 0; candidate < bufferSlotsDesc.size(); ++candidate) {
                if (bufferSlotsLastUse[candidate] < resource.lifetime.firstPass &&
                    bufferSlotsDesc[candidate].compatibleWith(resource.desc)) {
                    slot = candidate;
                    break;
                }
            }
            if (slot == bufferSlotsDesc.size()) {
                bufferSlotsDesc.push_back(resource.desc);
                bufferSlotsLastUse.push_back(resource.lifetime.lastPass);
                bufferSlotLastResource.push_back(resourceIndex);
            } else {
                bufferAliasPredecessor[resourceIndex] = bufferSlotLastResource[slot];
                bufferSlotsLastUse[slot] = resource.lifetime.lastPass;
                bufferSlotLastResource[slot] = resourceIndex;
            }
            resource.lifetime.allocationSlot = slot;
        }
        allocateTransientBuffers(bufferSlotsDesc);
        cachedImageSlotDescs = slotsDesc;
        cachedBufferSlotDescs = bufferSlotsDesc;

        }

        // Upload values are intentionally excluded from CompiledTemplate: an
        // upload can complete at a different timeline value every frame.
        if (templateHit) {
            uploadWaits_.clear();
            const auto addUploadWait = [this](const std::uint64_t timelineValue,
                                               const UploadConsumer& consumer) {
                if (timelineValue == 0 || consumer.stage == VK_PIPELINE_STAGE_2_NONE) return;
                const auto batch = consumer.batch;
                if (batch == std::numeric_limits<std::uint32_t>::max()) return;
                const auto existing = std::ranges::find_if(uploadWaits_, [batch](const UploadWait& wait) {
                    return wait.batch == batch;
                });
                if (existing != uploadWaits_.end()) {
                    existing->timelineValue = std::max(existing->timelineValue, timelineValue);
                    existing->stage |= consumer.stage;
                } else uploadWaits_.push_back({timelineValue, consumer.stage, queueBatches_[batch].queue, batch});
            };
            for (std::uint32_t resource = 0; resource < resources_.size(); ++resource) {
                addUploadWait(resources_[resource].uploadTimeline, textureUploadConsumers[resource]);
            }
            for (std::uint32_t resource = 0; resource < buffers_.size(); ++resource) {
                addUploadWait(buffers_[resource].uploadTimeline, bufferUploadConsumers[resource]);
            }
        }

        if (!templateHit) {
        struct State {
            VkPipelineStageFlags2 stage{};
            VkAccessFlags2 access{};
            VkImageLayout layout{};
            bool write{};
            Queue queue{Queue::Graphics};
            std::int32_t pass{-1};
        };
        std::vector<std::vector<State>> states(resources_.size());
        for (std::uint32_t resource = 0; resource < resources_.size(); ++resource) {
            const State initial = {
                resources_[resource].initialState.stage,
                resources_[resource].initialState.access,
                resources_[resource].initialState.layout,
                resources_[resource].initialState.write
            };
            states[resource].assign(static_cast<std::size_t>(resources_[resource].desc.mipLevels) *
                                    resources_[resource].desc.arrayLayers, initial);
        }
        barriers_.assign(count, {});
        releaseBarriers_.assign(count, {});
        for (std::uint32_t ordered = 0; ordered < order_.size(); ++ordered) {
            for (const Access &access: passes_[order_[ordered]].accesses) {
                const auto next = usageInfo(access.usage);
                const Queue nextQueue = passes_[order_[ordered]].queue;
                const auto &desc = resources_[access.texture.index].desc;
                for (std::uint32_t mip = access.range.baseMipLevel;
                     mip < access.range.baseMipLevel + access.range.levelCount; ++mip) {
                    for (std::uint32_t layer = access.range.baseArrayLayer;
                         layer < access.range.baseArrayLayer + access.range.layerCount; ++layer) {
                auto &state = states[access.texture.index][static_cast<std::size_t>(mip) * desc.arrayLayers + layer];
                const bool crossQueue = state.queue != nextQueue;
                if (state.layout != next.layout || state.write || next.write) {
                    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                    barrier.srcStageMask = state.stage;
                    barrier.srcAccessMask = state.access;
                    if (crossQueue) {
                        // Semaphore wait supplies availability; the acquire barrier only
                        // needs the destination scope. A release is emitted after producer.
                        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                        barrier.srcAccessMask = VK_ACCESS_2_NONE;
                        const auto sourceFamily = queueFamilies_[static_cast<std::uint32_t>(state.queue)];
                        const auto destinationFamily = queueFamilies_[static_cast<std::uint32_t>(nextQueue)];
                        if (state.pass >= 0 && sourceFamily != VK_QUEUE_FAMILY_IGNORED && destinationFamily !=
                            VK_QUEUE_FAMILY_IGNORED && sourceFamily != destinationFamily) {
                            VkImageMemoryBarrier2 release = barrier;
                            release.srcStageMask = state.stage;
                            release.srcAccessMask = state.access;
                            release.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
                            release.dstAccessMask = VK_ACCESS_2_NONE;
                            release.oldLayout = state.layout;
                            release.newLayout = next.layout;
                            release.image = resources_[access.texture.index].image;
                            release.subresourceRange = {desc.aspect, mip, 1, layer, 1};
                            release.srcQueueFamilyIndex = sourceFamily;
                            release.dstQueueFamilyIndex = destinationFamily;
                            releaseBarriers_[state.pass].images.push_back(release);
                            releaseBarriers_[state.pass].imageResources.push_back(access.texture.index);
                            barrier.oldLayout = next.layout;
                            barrier.newLayout = next.layout;
                            barrier.srcQueueFamilyIndex = sourceFamily;
                            barrier.dstQueueFamilyIndex = destinationFamily;
                        }
                    }
                    if (state.stage == VK_PIPELINE_STAGE_2_NONE &&
                        imageAliasPredecessor[access.texture.index] != std::numeric_limits<std::uint32_t>::max()) {
                        // This logical resource starts with an undefined
                        // layout, but its physical image was used earlier in
                        // the frame. Preserve the discard transition while
                        // making that earlier use visible.
                        const State &predecessor = states[imageAliasPredecessor[access.texture.index]][0];
                        barrier.srcStageMask = predecessor.stage;
                        barrier.srcAccessMask = predecessor.access;
                    }
                    barrier.dstStageMask = next.stage;
                    barrier.dstAccessMask = next.access;
                    barrier.oldLayout = state.layout;
                    barrier.newLayout = next.layout;
                    barrier.image = resources_[access.texture.index].image;
                    barrier.subresourceRange = {
                        resources_[access.texture.index].desc.aspect, mip, 1, layer, 1
                    };
                    barriers_[ordered].images.push_back(barrier);
                    barriers_[ordered].imageResources.push_back(access.texture.index);
                }
                state = {
                    next.stage, next.access, next.layout, next.write, nextQueue, static_cast<std::int32_t>(ordered)
                };
                    }
                }
            }
            // Dynamic rendering has no render-pass finalLayout. Emit an explicit
            // post-pass transition before making this the next source state.
            for (const auto &finalState: passes_[order_[ordered]].finalTextureStates) {
                requireValid(finalState.texture);
                const auto &desc = resources_[finalState.texture.index].desc;
                for (std::uint32_t mip = finalState.range.baseMipLevel;
                     mip < finalState.range.baseMipLevel + finalState.range.levelCount; ++mip) {
                    for (std::uint32_t layer = finalState.range.baseArrayLayer;
                         layer < finalState.range.baseArrayLayer + finalState.range.layerCount; ++layer) {
                        auto &state = states[finalState.texture.index][static_cast<std::size_t>(mip) * desc.arrayLayers + layer];
                        if (state.layout != finalState.state.layout || state.write || finalState.state.write) {
                            VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                            barrier.srcStageMask = state.stage;
                            barrier.srcAccessMask = state.access;
                            barrier.dstStageMask = finalState.state.stage;
                            barrier.dstAccessMask = finalState.state.access;
                            barrier.oldLayout = state.layout;
                            barrier.newLayout = finalState.state.layout;
                            barrier.image = resources_[finalState.texture.index].image;
                            barrier.subresourceRange = {desc.aspect, mip, 1, layer, 1};
                            releaseBarriers_[ordered].images.push_back(barrier);
                            releaseBarriers_[ordered].imageResources.push_back(finalState.texture.index);
                        }
                        state = {
                            finalState.state.stage, finalState.state.access, finalState.state.layout, finalState.state.write,
                            passes_[order_[ordered]].queue, static_cast<std::int32_t>(ordered)};
                    }
                }
            }
        }
        for (BarrierBatch &batch: barriers_) mergeImageBarriers(batch.images, batch.imageResources);
        for (BarrierBatch &batch: releaseBarriers_) mergeImageBarriers(batch.images, batch.imageResources);
        struct BufferState {
            VkPipelineStageFlags2 stage{};
            VkAccessFlags2 access{};
            bool write{};
            Queue queue{Queue::Graphics};
            std::int32_t pass{-1};
        };
        std::vector<BufferState> bufferStates(buffers_.size());
        for (std::uint32_t ordered = 0; ordered < order_.size(); ++ordered) {
            {
                for (const auto &access: passes_[order_[ordered]].bufferAccesses) {
                    auto &state = bufferStates[access.buffer.index];
                    const auto next = bufferUsageInfo(access.usage);
                    const Queue nextQueue = passes_[order_[ordered]].queue;
                    const bool crossQueue = state.queue != nextQueue;
                    if (state.write || next.write) {
                        VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                        barrier.srcStageMask = state.stage;
                        barrier.srcAccessMask = state.access;
                        if (crossQueue) {
                            barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                            barrier.srcAccessMask = VK_ACCESS_2_NONE;
                            const auto sourceFamily = queueFamilies_[static_cast<std::uint32_t>(state.queue)];
                            const auto destinationFamily = queueFamilies_[static_cast<std::uint32_t>(nextQueue)];
                            if (state.pass >= 0 && sourceFamily != VK_QUEUE_FAMILY_IGNORED && destinationFamily !=
                                VK_QUEUE_FAMILY_IGNORED && sourceFamily != destinationFamily) {
                                VkBufferMemoryBarrier2 release = barrier;
                                release.srcStageMask = state.stage;
                                release.srcAccessMask = state.access;
                                release.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
                                release.dstAccessMask = VK_ACCESS_2_NONE;
                                release.srcQueueFamilyIndex = sourceFamily;
                                release.dstQueueFamilyIndex = destinationFamily;
                                releaseBarriers_[state.pass].buffers.push_back(release);
                                releaseBarriers_[state.pass].bufferResources.push_back(access.buffer.index);
                                barrier.srcQueueFamilyIndex = sourceFamily;
                                barrier.dstQueueFamilyIndex = destinationFamily;
                            }
                        }
                        barrier.dstStageMask = next.stage;
                        barrier.dstAccessMask = next.access;
                        barrier.buffer = buffers_[access.buffer.index].buffer;
                        barrier.offset = 0;
                        barrier.size = VK_WHOLE_SIZE;
                        barriers_[ordered].buffers.push_back(barrier);
                        barriers_[ordered].bufferResources.push_back(access.buffer.index);
                    }
                    state = {next.stage, next.access, next.write, nextQueue, static_cast<std::int32_t>(ordered)};
                }
            }
        }
        // A reused slot is a real Vulkan alias: its previous logical buffer may
        // have left writes in cache.  The per-resource state above deliberately
        // starts fresh, so add the ownership/memory dependency explicitly.
        for (std::uint32_t resource = 0; resource < buffers_.size(); ++resource) {
            if (bufferAliasPredecessor[resource] == std::numeric_limits<std::uint32_t>::max()) {
                continue;
            }
            for (const auto &access: passes_[order_[buffers_[resource].lifetime.firstPass]].bufferAccesses) {
                if (access.buffer.index != resource) {
                    continue;
                }
                const auto next = bufferUsageInfo(access.usage);
                VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                const BufferState &predecessor = bufferStates[bufferAliasPredecessor[resource]];
                barrier.srcStageMask = predecessor.stage;
                barrier.srcAccessMask = predecessor.access;
                barrier.dstStageMask = next.stage;
                barrier.dstAccessMask = next.access;
                barrier.buffer = buffers_[resource].buffer;
                barrier.size = VK_WHOLE_SIZE;
                barriers_[buffers_[resource].lifetime.firstPass].buffers.push_back(barrier);
                barriers_[buffers_[resource].lifetime.firstPass].bufferResources.push_back(resource);
                break;
            }
        }
        compiledTemplates_.push_back({
            .signature = signature,
            .order = order_,
            .queueDependencies = queueDependencies_,
            .queueBatches = queueBatches_,
            .textureLifetimes = [&] { std::vector<TextureLifetime> values; values.reserve(resources_.size()); for (const auto& resource : resources_) values.push_back(resource.lifetime); return values; }(),
            .bufferLifetimes = [&] { std::vector<BufferLifetime> values; values.reserve(buffers_.size()); for (const auto& resource : buffers_) values.push_back(resource.lifetime); return values; }(),
            .imageSlotDescs = cachedImageSlotDescs,
            .bufferSlotDescs = cachedBufferSlotDescs,
            .imageAliasPredecessors = imageAliasPredecessor,
            .bufferAliasPredecessors = bufferAliasPredecessor,
            .textureUploadConsumers = textureUploadConsumers,
            .bufferUploadConsumers = bufferUploadConsumers,
            .barriers = barriers_,
            .releaseBarriers = releaseBarriers_,
            .valid = true});
        }
        compiled_ = true;
    }

    void RenderGraph::allocateTransients(const std::vector<TextureDesc> &slotDescs) {
        if (device_ == VK_NULL_HANDLE) {
            return; // Metadata-only graphs are useful in tooling and tests.
        }
        transientImageSlots_.clear();
        transientImageSlots_.reserve(slotDescs.size());
        std::vector<bool> leased(transientAllocations_.size(), false);
        try {
            for (const TextureDesc &desc: slotDescs) {
                const auto reusable = std::ranges::find_if(transientAllocations_,
                                                           [&](const TransientAllocation &allocation) {
                                                               const auto index = static_cast<std::size_t>(
                                                                   &allocation - transientAllocations_.data());
                                                               return !leased[index] && allocation.desc.compatibleWith(
                                                                          desc);
                                                           });
                if (reusable != transientAllocations_.end()) {
                    const auto index = static_cast<std::uint32_t>(reusable - transientAllocations_.begin());
                    leased[index] = true;
                    transientImageSlots_.push_back(index);
                    continue;
                }
                const VkImageCreateInfo createInfo{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                    .imageType = desc.extent.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                    .format = desc.format,
                    .extent = desc.extent,
                    .mipLevels = desc.mipLevels,
                    .arrayLayers = desc.arrayLayers,
                    .samples = desc.samples,
                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                    .usage = desc.usage,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
                };
                const VmaAllocationCreateInfo allocationInfo{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
                TransientAllocation allocation{};
                if (vmaCreateImage(allocator_, &createInfo, &allocationInfo, &allocation.image, &allocation.allocation,
                                   nullptr) != VK_SUCCESS) {
                    throw std::runtime_error("Could not allocate a transient RenderGraph image");
                }
                allocation.desc = desc;
                transientAllocations_.push_back(allocation);
                leased.push_back(true);
                transientImageSlots_.push_back(static_cast<std::uint32_t>(transientAllocations_.size() - 1));
            }
        } catch (...) {
            transientImageSlots_.clear();
            throw;
        }
        for (auto &resource: resources_) {
            if (!resource.imported && resource.lifetime.firstPass != passes_.size()) {
                resource.image = transientAllocations_[transientImageSlots_[resource.lifetime.allocationSlot]].image;
            }
        }
    }

    void RenderGraph::allocateTransientBuffers(const std::vector<BufferDesc> &slotDescs) {
        if (device_ == VK_NULL_HANDLE) {
            return;
        }
        transientBufferSlots_.clear();
        transientBufferSlots_.reserve(slotDescs.size());
        std::vector<bool> leased(transientBufferAllocations_.size(), false);
        try {
            for (const BufferDesc &desc: slotDescs) {
                const auto reusable = std::ranges::find_if(transientBufferAllocations_,
                                                           [&](const TransientBufferAllocation &allocation) {
                                                               const auto index = static_cast<std::size_t>(
                                                                   &allocation - transientBufferAllocations_.data());
                                                               return !leased[index] && allocation.desc.compatibleWith(
                                                                          desc);
                                                           });
                if (reusable != transientBufferAllocations_.end()) {
                    const auto index = static_cast<std::uint32_t>(reusable - transientBufferAllocations_.begin());
                    leased[index] = true;
                    transientBufferSlots_.push_back(index);
                    continue;
                }
                const VkBufferCreateInfo createInfo{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .size = desc.size,
                    .usage = desc.usage,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE
                };
                const VmaAllocationCreateInfo allocationInfo{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
                TransientBufferAllocation allocation{};
                if (vmaCreateBuffer(allocator_, &createInfo, &allocationInfo, &allocation.buffer,
                                    &allocation.allocation,
                                    nullptr) != VK_SUCCESS) {
                    throw std::runtime_error("Could not allocate a transient RenderGraph buffer");
                }
                allocation.desc = desc;
                transientBufferAllocations_.push_back(allocation);
                leased.push_back(true);
                transientBufferSlots_.push_back(static_cast<std::uint32_t>(transientBufferAllocations_.size() - 1));
            }
        } catch (...) {
            transientBufferSlots_.clear();
            throw;
        }
        for (auto &resource: buffers_) {
            if (!resource.imported && resource.lifetime.firstPass != passes_.size()) {
                resource.buffer = transientBufferAllocations_[transientBufferSlots_[resource.lifetime.allocationSlot]].
                        buffer;
            }
        }
    }

    void RenderGraph::execute(const VkCommandBuffer commandBuffer) {
        if (!compiled_) {
            compile();
        }
        for (std::uint32_t ordered = 0; ordered < order_.size(); ++ordered) {
            const BarrierBatch &barriers = barriers_[ordered];
            if (!barriers.images.empty() || !barriers.buffers.empty()) {
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(barriers.buffers.size());
                dependency.pBufferMemoryBarriers = barriers.buffers.data();
                dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.images.size());
                dependency.pImageMemoryBarriers = barriers.images.data();
                vkCmdPipelineBarrier2(commandBuffer, &dependency);
            }
            if (const auto &callback = passes_[order_[ordered]].execute) callback(commandBuffer);
            const BarrierBatch &releaseBarriers = releaseBarriers_[ordered];
            if (!releaseBarriers.images.empty() || !releaseBarriers.buffers.empty()) {
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(releaseBarriers.buffers.size());
                dependency.pBufferMemoryBarriers = releaseBarriers.buffers.data();
                dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(releaseBarriers.images.size());
                dependency.pImageMemoryBarriers = releaseBarriers.images.data();
                vkCmdPipelineBarrier2(commandBuffer, &dependency);
            }
        }
    }

    void RenderGraph::execute(const Queue queue, const VkCommandBuffer commandBuffer) {
        if (!compiled_) {
            compile();
        }
        const auto emit = [commandBuffer](const BarrierBatch &batch) {
            if (batch.images.empty() && batch.buffers.empty()) return;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(batch.buffers.size());
            dependency.pBufferMemoryBarriers = batch.buffers.data();
            dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(batch.images.size());
            dependency.pImageMemoryBarriers = batch.images.data();
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
        };
        for (std::uint32_t ordered = 0; ordered < order_.size(); ++ordered) {
            const Pass &pass = passes_[order_[ordered]];
            if (pass.queue != queue) {
                continue;
            }
            emit(barriers_[ordered]);
            if (pass.execute) {
                pass.execute(commandBuffer);
            }
            // Ownership release must be recorded by the producing queue, after
            // its callback and before its timeline value is signalled.
            emit(releaseBarriers_[ordered]);
        }
    }

    void RenderGraph::recordAndSubmit(const SubmissionContext& context) {
        if (!compiled_) compile();
        if (context.commandBuffers.size() != queueBatches_.size()) {
            throw std::invalid_argument("RenderGraph requires one command buffer per queue batch");
        }
        if (context.graphTimeline == VK_NULL_HANDLE || context.nextTimelineValue == nullptr) {
            throw std::invalid_argument("RenderGraph submission requires a timeline semaphore and counter");
        }
        for (const QueueBatch& batch : queueBatches_) {
            if (context.queues[static_cast<std::uint32_t>(batch.queue)] == VK_NULL_HANDLE) {
                throw std::invalid_argument("RenderGraph submission is missing a queue");
            }
        }
        if (!uploadWaits_.empty() && context.uploadTimeline == VK_NULL_HANDLE) {
            throw std::invalid_argument("RenderGraph upload waits require the upload timeline semaphore");
        }

        const auto emit = [](const VkCommandBuffer commandBuffer, const BarrierBatch& batch) {
            if (batch.images.empty() && batch.buffers.empty()) return;
            const VkDependencyInfo dependency{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .bufferMemoryBarrierCount = static_cast<std::uint32_t>(batch.buffers.size()),
                .pBufferMemoryBarriers = batch.buffers.data(),
                .imageMemoryBarrierCount = static_cast<std::uint32_t>(batch.images.size()),
                .pImageMemoryBarriers = batch.images.data()};
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
        };
        std::vector<std::uint32_t> passOrder(passes_.size(), std::numeric_limits<std::uint32_t>::max());
        for (std::uint32_t order = 0; order < order_.size(); ++order) passOrder[order_[order]] = order;
        for (std::uint32_t index = 0; index < queueBatches_.size(); ++index) {
            const VkCommandBuffer commandBuffer = context.commandBuffers[index];
            if (commandBuffer == VK_NULL_HANDLE || vkResetCommandBuffer(commandBuffer, 0) != VK_SUCCESS) {
                throw std::runtime_error("Could not reset RenderGraph batch command buffer");
            }
            const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            if (vkBeginCommandBuffer(commandBuffer, &begin) != VK_SUCCESS) {
                throw std::runtime_error("Could not begin RenderGraph batch command buffer");
            }
            for (const std::uint32_t pass : queueBatches_[index].passes) {
                const std::uint32_t ordered = passOrder[pass];
                emit(commandBuffer, barriers_[ordered]);
                if (passes_[pass].execute) passes_[pass].execute(commandBuffer);
                emit(commandBuffer, releaseBarriers_[ordered]);
            }
            if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
                throw std::runtime_error("Could not end RenderGraph batch command buffer");
            }
        }

        std::vector<std::uint64_t> batchValues(queueBatches_.size());
        for (std::uint32_t index = 0; index < queueBatches_.size(); ++index) {
            const QueueBatch& batch = queueBatches_[index];
            std::vector<VkSemaphoreSubmitInfo> waits;
            for (const std::uint32_t producer : batch.waitBatches) {
                waits.push_back({.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                    .semaphore = context.graphTimeline, .value = batchValues[producer],
                    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT});
            }
            for (const UploadWait& wait : uploadWaits_) if (wait.batch == index) {
                waits.push_back({.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                    .semaphore = context.uploadTimeline, .value = wait.timelineValue, .stageMask = wait.stage});
            }
            for (const ExternalSemaphoreWait& wait : context.externalWaits) if (wait.batch == index) {
                if (wait.semaphore == VK_NULL_HANDLE) throw std::invalid_argument("RenderGraph external wait is null");
                waits.push_back({.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                    .semaphore = wait.semaphore, .value = wait.value, .stageMask = wait.stage});
            }
            batchValues[index] = ++*context.nextTimelineValue;
            const VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .semaphore = context.graphTimeline, .value = batchValues[index],
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
            const VkCommandBufferSubmitInfo command{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .commandBuffer = context.commandBuffers[index]};
            const VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                .waitSemaphoreInfoCount = static_cast<std::uint32_t>(waits.size()), .pWaitSemaphoreInfos = waits.data(),
                .commandBufferInfoCount = 1, .pCommandBufferInfos = &command,
                .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal};
            if (vkQueueSubmit2(context.queues[static_cast<std::uint32_t>(batch.queue)], 1, &submit,
                               VK_NULL_HANDLE) != VK_SUCCESS) {
                throw std::runtime_error("Could not submit RenderGraph queue batch");
            }
        }

        // A frame fence must cover independent async batches too, not merely
        // the last topological batch.  A final empty graphics submission joins
        // all graph timeline values and is the sole owner of completion signals.
        if (context.queues[static_cast<std::uint32_t>(Queue::Graphics)] == VK_NULL_HANDLE) {
            throw std::invalid_argument("RenderGraph completion requires a graphics queue");
        }
        std::vector<VkSemaphoreSubmitInfo> completionWaits;
        for (const std::uint64_t value : batchValues) completionWaits.push_back({
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = context.graphTimeline,
            .value = value, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT});
        std::vector<VkSemaphoreSubmitInfo> completionSignals;
        for (const ExternalSemaphoreSignal& signal : context.completionSignals) {
            if (signal.semaphore == VK_NULL_HANDLE) throw std::invalid_argument("RenderGraph completion signal is null");
            completionSignals.push_back({.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .semaphore = signal.semaphore, .value = signal.value, .stageMask = signal.stage});
        }
        const VkSubmitInfo2 completion{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .waitSemaphoreInfoCount = static_cast<std::uint32_t>(completionWaits.size()),
            .pWaitSemaphoreInfos = completionWaits.data(),
            .signalSemaphoreInfoCount = static_cast<std::uint32_t>(completionSignals.size()),
            .pSignalSemaphoreInfos = completionSignals.data()};
        if (vkQueueSubmit2(context.queues[static_cast<std::uint32_t>(Queue::Graphics)], 1, &completion,
                           context.completionFence) != VK_SUCCESS) {
            throw std::runtime_error("Could not submit RenderGraph completion batch");
        }
    }

    void RenderGraph::destroyTransientPool() noexcept {
        if (allocator_ != VK_NULL_HANDLE) {
            for (const auto &allocation: transientAllocations_) {
                vmaDestroyImage(allocator_, allocation.image, allocation.allocation);
            }
        }
        if (allocator_ != VK_NULL_HANDLE) {
            for (const auto &allocation: transientBufferAllocations_) {
                vmaDestroyBuffer(allocator_, allocation.buffer, allocation.allocation);
            }
        }
        transientAllocations_.clear();
        transientBufferAllocations_.clear();
        transientImageSlots_.clear();
        transientBufferSlots_.clear();
    }

    void RenderGraph::reset() noexcept {
        transientImageSlots_.clear();
        transientBufferSlots_.clear();
        resources_.clear();
        buffers_.clear();
        passes_.clear();
        order_.clear();
        orderNames_.clear();
        barriers_.clear();
        releaseBarriers_.clear();
        queueDependencies_.clear();
        queueBatches_.clear();
        uploadWaits_.clear();
        exportedTextures_.clear();
        exportedBuffers_.clear();
        compiled_ = false;
    }

    void RenderGraph::trimTransientPool() noexcept { destroyTransientPool(); }

    const std::vector<std::string> &RenderGraph::executionOrder() const noexcept { return orderNames_; }
    const std::vector<QueueDependency> &RenderGraph::queueDependencies() const noexcept { return queueDependencies_; }
    const std::vector<QueueBatch> &RenderGraph::queueBatches() const noexcept { return queueBatches_; }

    const TextureLifetime &RenderGraph::lifetime(const TextureHandle texture) const {
        requireValid(texture);
        if (!compiled_) {
            throw std::logic_error("Compile RenderGraph before querying lifetime");
        }
        return resources_[texture.index].lifetime;
    }

    const BufferLifetime &RenderGraph::lifetime(const BufferHandle buffer) const {
        requireValid(buffer);
        if (!compiled_) {
            throw std::logic_error("Compile RenderGraph before querying lifetime");
        }
        return buffers_[buffer.index].lifetime;
    }

    VkImage RenderGraph::image(const TextureHandle texture) const {
        requireValid(texture);
        return resources_[texture.index].image;
    }

    VkBuffer RenderGraph::buffer(const BufferHandle buffer) const {
        requireValid(buffer);
        return buffers_[buffer.index].buffer;
    }
} // namespace Engine::RenderGraph
