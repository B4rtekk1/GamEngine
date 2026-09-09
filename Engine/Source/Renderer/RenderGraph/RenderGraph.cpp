#include "Engine/Renderer/RenderGraph/RenderGraph.h"

#include <algorithm>
#include <queue>
#include <stdexcept>
#include <unordered_set>

namespace Engine::Renderer {
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
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false
                    };
                case TextureUsage::StorageRead: return {
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, false
                    };
                case TextureUsage::StorageWrite: return {
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, true
                    };
                case TextureUsage::ColorAttachment: return {
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true
                    };
                case TextureUsage::DepthAttachment: return {
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, true
                    };
                case TextureUsage::TransferRead: return {
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false
                    };
                case TextureUsage::TransferWrite: return {
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true
                    };
                case TextureUsage::Present: return {
                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, false
                    };
            }
            throw std::logic_error("Unknown texture usage");
        }
    }

    bool TextureDesc::compatibleWith(const TextureDesc &other) const noexcept {
        return extent.width == other.extent.width && extent.height == other.extent.height &&
               extent.depth == other.extent.depth && format == other.format && usage == other.usage &&
               aspect == other.aspect && mipLevels == other.mipLevels && arrayLayers == other.arrayLayers &&
               samples == other.samples;
    }

    void PassBuilder::read(const TextureHandle texture, const TextureUsage usage) {
        graph_.addAccess(pass_, texture, usage, false);
    }

    void PassBuilder::write(const TextureHandle texture, const TextureUsage usage) {
        graph_.addAccess(pass_, texture, usage, true);
    }

    TextureHandle PassBuilder::writeTexture(std::string name, const TextureDesc &desc, const TextureUsage usage) {
        return graph_.addTransient(std::move(name), desc, pass_, usage);
    }

    TextureHandle RenderGraph::importTexture(std::string name, const VkImage image, const TextureDesc &desc,
                                             const VkImageLayout initialLayout) {
        if (image == VK_NULL_HANDLE) throw std::invalid_argument("Cannot import a null image into RenderGraph");
        if (compiled_) throw std::logic_error("Reset RenderGraph before adding resources");
        resources_.push_back({std::move(name), desc, image, initialLayout, true});
        return {static_cast<std::uint32_t>(resources_.size() - 1)};
    }

    TextureHandle RenderGraph::createTexture(std::string name, const TextureDesc &desc) {
        if (compiled_) throw std::logic_error("Reset RenderGraph before adding resources");
        resources_.push_back({std::move(name), desc});
        return {static_cast<std::uint32_t>(resources_.size() - 1)};
    }

    void RenderGraph::initialize(const VkDevice device, const VmaAllocator allocator) {
        if (compiled_ || !transientAllocations_.empty()) throw std::logic_error(
            "Reset RenderGraph before changing its allocator");
        if (device == VK_NULL_HANDLE || allocator == VK_NULL_HANDLE) throw std::invalid_argument(
            "RenderGraph requires a valid Vulkan device and VMA allocator");
        device_ = device;
        allocator_ = allocator;
    }

    void RenderGraph::addPass(std::string name, const std::function<void(PassBuilder &)> &setup,
                              ExecuteCallback execute) {
        if (compiled_) throw std::logic_error("Reset RenderGraph before adding passes");
        passes_.push_back({std::move(name), {}, std::move(execute)});
        PassBuilder builder{*this, static_cast<std::uint32_t>(passes_.size() - 1)};
        setup(builder);
    }

    void RenderGraph::requireValid(const TextureHandle texture) const {
        if (!texture || texture.index >= resources_.size()) throw std::out_of_range(
            "Invalid RenderGraph texture handle");
    }

    void RenderGraph::addAccess(const std::uint32_t pass, const TextureHandle texture, const TextureUsage usage,
                                const bool write) {
        requireValid(texture);
        if (pass >= passes_.size()) throw std::logic_error("Invalid RenderGraph pass");
        if (usageInfo(usage).write != write && !(usage == TextureUsage::ColorAttachment && write) && !(
                usage == TextureUsage::DepthAttachment && write))
            throw std::invalid_argument("Texture usage does not match read/write declaration");
        passes_[pass].accesses.push_back({texture, usage, write});
    }

    TextureHandle RenderGraph::addTransient(std::string name, const TextureDesc &desc, const std::uint32_t pass,
                                            const TextureUsage usage) {
        const TextureHandle texture = createTexture(std::move(name), desc);
        addAccess(pass, texture, usage, true);
        return texture;
    }

    void RenderGraph::compile() {
        const auto count = static_cast<std::uint32_t>(passes_.size());
        std::vector<std::unordered_set<std::uint32_t> > edges(count);
        std::vector<std::uint32_t> indegree(count);
        std::vector<std::int32_t> lastWriter(resources_.size(), -1);
        std::vector<std::vector<std::uint32_t> > readers(resources_.size());
        for (std::uint32_t pass = 0; pass < count; ++pass) {
            for (const Access &access: passes_[pass].accesses) {
                const auto resource = access.texture.index;
                if (!access.write) {
                    if (lastWriter[resource] >= 0) edges[static_cast<std::uint32_t>(lastWriter[resource])].insert(pass);
                    else if (!resources_[resource].imported) throw std::logic_error(
                        "Transient texture read before it is written: " + resources_[resource].name);
                    readers[resource].push_back(pass);
                } else {
                    if (lastWriter[resource] >= 0) edges[static_cast<std::uint32_t>(lastWriter[resource])].insert(pass);
                    for (const auto reader: readers[resource]) edges[reader].insert(pass);
                    readers[resource].clear();
                    lastWriter[resource] = static_cast<std::int32_t>(pass);
                }
            }
        }
        for (const auto &sources: edges) for (const auto target: sources) ++indegree[target];
        std::queue<std::uint32_t> ready;
        for (std::uint32_t pass = 0; pass < count; ++pass) if (indegree[pass] == 0) ready.push(pass);
        order_.clear();
        orderNames_.clear();
        while (!ready.empty()) {
            const auto pass = ready.front();
            ready.pop();
            order_.push_back(pass);
            orderNames_.push_back(passes_[pass].name);
            for (const auto target: edges[pass]) if (--indegree[target] == 0) ready.push(target);
        }
        if (order_.size() != passes_.size()) throw std::logic_error("RenderGraph contains a dependency cycle");

        for (auto &resource: resources_) resource.lifetime = {count, 0, 0};
        for (std::uint32_t ordered = 0; ordered < count; ++ordered)
            for (const auto &access: passes_[order_[ordered]].accesses) {
                auto &lifetime = resources_[access.texture.index].lifetime;
                lifetime.firstPass = std::min(lifetime.firstPass, ordered);
                lifetime.lastPass = std::max(lifetime.lastPass, ordered);
            }
        std::vector<std::uint32_t> transientResources;
        for (std::uint32_t resource = 0; resource < resources_.size(); ++resource)
            if (!resources_[resource].imported && resources_[resource].lifetime.firstPass != count)
                transientResources.push_back(resource);
        std::ranges::sort(transientResources, [this](const std::uint32_t left, const std::uint32_t right) {
            return resources_[left].lifetime.firstPass < resources_[right].lifetime.firstPass;
        });

        std::vector<std::uint32_t> slotsLastUse;
        std::vector<TextureDesc> slotsDesc;
        for (const auto resourceIndex: transientResources) {
            auto &resource = resources_[resourceIndex];
            std::uint32_t slot = static_cast<std::uint32_t>(slotsDesc.size());
            for (std::uint32_t candidate = 0; candidate < slotsDesc.size(); ++candidate)
                if (slotsLastUse[candidate] < resource.lifetime.firstPass && slotsDesc[candidate].
                    compatibleWith(resource.desc)) {
                    slot = candidate;
                    break;
                }
            if (slot == slotsDesc.size()) {
                slotsDesc.push_back(resource.desc);
                slotsLastUse.push_back(resource.lifetime.lastPass);
            } else slotsLastUse[slot] = resource.lifetime.lastPass;
            resource.lifetime.allocationSlot = slot;
        }
        allocateTransients(slotsDesc);

        struct State {
            VkPipelineStageFlags2 stage{};
            VkAccessFlags2 access{};
            VkImageLayout layout{};
            bool write{};
        };
        std::vector<State> states(resources_.size());
        for (std::uint32_t resource = 0; resource < resources_.size(); ++resource)
            states[resource].layout = resources_[resource].initialLayout;
        barriers_.assign(count, {});
        for (std::uint32_t ordered = 0; ordered < count; ++ordered)
            for (const Access &access: passes_[order_[ordered]].accesses) {
                auto &state = states[access.texture.index];
                const auto next = usageInfo(access.usage);
                if (state.layout != next.layout || state.write || next.write) {
                    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                    barrier.srcStageMask = state.stage;
                    barrier.srcAccessMask = state.access;
                    barrier.dstStageMask = next.stage;
                    barrier.dstAccessMask = next.access;
                    barrier.oldLayout = state.layout;
                    barrier.newLayout = next.layout;
                    barrier.image = resources_[access.texture.index].image;
                    barrier.subresourceRange = {
                        resources_[access.texture.index].desc.aspect, 0,
                        resources_[access.texture.index].desc.mipLevels, 0,
                        resources_[access.texture.index].desc.arrayLayers
                    };
                    barriers_[ordered].push_back({access.texture.index, barrier});
                }
                state = {next.stage, next.access, next.layout, next.write};
            }
        compiled_ = true;
    }

    void RenderGraph::allocateTransients(const std::vector<TextureDesc> &slotDescs) {
        if (device_ == VK_NULL_HANDLE) return; // Metadata-only graphs are useful in tooling and tests.
        transientAllocations_.reserve(slotDescs.size());
        try {
            for (const TextureDesc &desc: slotDescs) {
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
                                   nullptr) != VK_SUCCESS)
                    throw std::runtime_error("Could not allocate a transient RenderGraph image");
                transientAllocations_.push_back(allocation);
            }
        } catch (...) {
            destroyTransients();
            throw;
        }
        for (auto &resource: resources_)
            if (!resource.imported && resource.lifetime.firstPass != passes_.size())
                resource.image = transientAllocations_[resource.lifetime.allocationSlot].image;
    }

    void RenderGraph::execute(const VkCommandBuffer commandBuffer) {
        if (!compiled_) compile();
        for (std::uint32_t ordered = 0; ordered < order_.size(); ++ordered) {
            std::vector<VkImageMemoryBarrier2> barriers;
            for (const auto &planned: barriers_[ordered]) {
                if (planned.vk.image == VK_NULL_HANDLE) throw std::logic_error(
                    "Transient RenderGraph allocation is not bound: " + resources_[planned.resource].name);
                barriers.push_back(planned.vk);
            }
            if (!barriers.empty()) {
                const VkDependencyInfo dependency{
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size()),
                    .pImageMemoryBarriers = barriers.data()
                };
                vkCmdPipelineBarrier2(commandBuffer, &dependency);
            }
            if (const auto &callback = passes_[order_[ordered]].execute) callback(commandBuffer);
        }
    }

    void RenderGraph::destroyTransients() noexcept {
        if (allocator_ != VK_NULL_HANDLE)
            for (const auto &allocation: transientAllocations_)
                vmaDestroyImage(allocator_, allocation.image, allocation.allocation);
        transientAllocations_.clear();
    }

    void RenderGraph::reset() noexcept {
        destroyTransients();
        resources_.clear();
        passes_.clear();
        order_.clear();
        orderNames_.clear();
        barriers_.clear();
        compiled_ = false;
    }

    const std::vector<std::string> &RenderGraph::executionOrder() const noexcept { return orderNames_; }

    const TextureLifetime &RenderGraph::lifetime(const TextureHandle texture) const {
        requireValid(texture);
        if (!compiled_) throw std::logic_error("Compile RenderGraph before querying lifetime");
        return resources_[texture.index].lifetime;
    }

    VkImage RenderGraph::image(const TextureHandle texture) const {
        requireValid(texture);
        return resources_[texture.index].image;
    }
} // namespace Engine::Renderer
