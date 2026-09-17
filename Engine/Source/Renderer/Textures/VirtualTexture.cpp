#include "Engine/Renderer/Textures/VirtualTexture.h"

#include "Engine/Assets/Gtex.h"
#include "Engine/Renderer/Vulkan/upload_context.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>

namespace Engine {
    namespace {
        VkFormat vkFormat(const Assets::TextureFormat format) {
            switch (format) {
                case Assets::TextureFormat::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
                case Assets::TextureFormat::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
                case Assets::TextureFormat::BC4_UNORM: return VK_FORMAT_BC4_UNORM_BLOCK;
                case Assets::TextureFormat::BC5_UNORM: return VK_FORMAT_BC5_UNORM_BLOCK;
                case Assets::TextureFormat::BC7_UNORM: return VK_FORMAT_BC7_UNORM_BLOCK;
                case Assets::TextureFormat::BC7_SRGB: return VK_FORMAT_BC7_SRGB_BLOCK;
            }
            throw std::invalid_argument("Unknown virtual texture format");
        }
        std::uint32_t blockExtent(const Assets::TextureFormat format) {
            return (format == Assets::TextureFormat::RGBA8_SRGB || format == Assets::TextureFormat::RGBA8_UNORM) ? 1 : 4;
        }
        std::uint32_t bytesPerBlock(const Assets::TextureFormat format) {
            if (format == Assets::TextureFormat::RGBA8_SRGB || format == Assets::TextureFormat::RGBA8_UNORM) return 4;
            return format == Assets::TextureFormat::BC4_UNORM ? 8 : 16;
        }
        void transition(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                        VkAccessFlags2 sourceAccess, VkAccessFlags2 destinationAccess,
                        VkPipelineStageFlags2 sourceStage, VkPipelineStageFlags2 destinationStage) {
            VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barrier.oldLayout = oldLayout; barrier.newLayout = newLayout; barrier.image = image;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barrier.srcAccessMask = sourceAccess; barrier.dstAccessMask = destinationAccess;
            barrier.srcStageMask = sourceStage; barrier.dstStageMask = destinationStage;
            const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
        }
        void hostWriteBarrier(const VkCommandBuffer commandBuffer, const VkBuffer buffer,
                              const VkPipelineStageFlags2 destinationStage,
                              const VkAccessFlags2 destinationAccess) {
            VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
            barrier.dstStageMask = destinationStage;
            barrier.dstAccessMask = destinationAccess;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = buffer;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier};
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
        }
    }

    VirtualTexture::~VirtualTexture() { destroy(); }

    void VirtualTexture::create(const VkPhysicalDevice physicalDevice, const VkDevice device,
                                const VkCommandPool commandPool, const VkQueue queue,
                                const Assets::GtexTexture& source, const VmaAllocator allocator, Config config) {
        if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE || commandPool == VK_NULL_HANDLE ||
            queue == VK_NULL_HANDLE || allocator == VK_NULL_HANDLE || source.mips.empty() ||
            config.pageSize == 0 || config.cacheTilesX == 0 || config.cacheTilesY == 0 ||
            config.maxUploadsPerUpdate == 0 || config.feedbackCapacity == 0 ||
            config.pageSize % blockExtent(source.format) != 0)
            throw std::invalid_argument("Invalid virtual texture configuration");
        if (UploadContext::current() == nullptr)
            throw std::logic_error("VirtualTexture requires an active UploadContext");

        destroy();
        device_ = device; allocator_ = allocator; source_ = source; config_ = config; format_ = vkFormat(source.format);
        const VkPhysicalDeviceProperties properties = [&] {
            VkPhysicalDeviceProperties result{};
            vkGetPhysicalDeviceProperties(physicalDevice, &result);
            return result;
        }();
        const std::uint64_t cacheWidth = static_cast<std::uint64_t>(config.pageSize) * config.cacheTilesX;
        const std::uint64_t cacheHeight = static_cast<std::uint64_t>(config.pageSize) * config.cacheTilesY;
        if (cacheWidth > properties.limits.maxImageDimension2D || cacheHeight > properties.limits.maxImageDimension2D)
            throw std::invalid_argument("Virtual texture cache exceeds the device image-size limit");

        mipPageOffsetsCpu_.reserve(source.mips.size());
        std::uint32_t count = 0;
        std::uint32_t expectedWidth = source.width, expectedHeight = source.height;
        for (const auto& mip : source.mips) {
            if (mip.width == 0 || mip.height == 0 || mip.width != expectedWidth || mip.height != expectedHeight)
                throw std::invalid_argument("Invalid virtual texture mip chain");
            mipPageOffsetsCpu_.push_back(count);
            const std::uint64_t pages = static_cast<std::uint64_t>((mip.width + config.pageSize - 1) / config.pageSize) *
                                        ((mip.height + config.pageSize - 1) / config.pageSize);
            if (pages > std::numeric_limits<std::uint32_t>::max() - count)
                throw std::invalid_argument("Virtual texture page table is too large");
            count += static_cast<std::uint32_t>(pages);
            expectedWidth = std::max(1U, expectedWidth / 2);
            expectedHeight = std::max(1U, expectedHeight / 2);
        }
        pageTableCpu_.assign(count, InvalidPage);
        slots_.assign(static_cast<std::size_t>(config.cacheTilesX) * config.cacheTilesY, {});
        pageTable_.createHostVisible(physicalDevice, device_, sizeof(std::uint32_t) * count,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, allocator_);
        mipOffsets_.createHostVisible(physicalDevice, device_,
                                      sizeof(std::uint32_t) * mipPageOffsetsCpu_.size(),
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, allocator_);
        mipOffsets_.update(mipPageOffsetsCpu_.data(), sizeof(std::uint32_t) * mipPageOffsetsCpu_.size());
        flushPageTable();
        feedback_.createHostVisible(physicalDevice, device_,
                                    sizeof(std::uint32_t) * (config.feedbackCapacity + 1),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, allocator_);
        beginFeedback();

        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D; info.format = format_;
        info.extent = {config.pageSize * config.cacheTilesX, config.pageSize * config.cacheTilesY, 1};
        info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        std::array<std::uint32_t, 3> sharingFamilies{};
        if (const auto* upload = UploadContext::current(); upload->requiresConcurrentSharing()) {
            sharingFamilies = upload->sharingFamilies();
            info.sharingMode = VK_SHARING_MODE_CONCURRENT;
            info.queueFamilyIndexCount = upload->sharingFamilyCount();
            info.pQueueFamilyIndices = sharingFamilies.data();
        }
        VmaAllocationCreateInfo allocationInfo{}; allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(allocator_, &info, &allocationInfo, &cacheImage_, &cacheAllocation_, nullptr) != VK_SUCCESS)
            throw std::runtime_error("Could not allocate virtual texture page cache");

        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = cacheImage_; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format_;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &view, nullptr, &cacheView_) != VK_SUCCESS)
            throw std::runtime_error("Could not create virtual texture cache view");
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        // GTEX v1 has no tile gutters. Nearest sampling avoids filtering into
        // a neighbouring physical tile; enable linear filtering when the
        // cooker starts storing a duplicated border around every page.
        sampler.magFilter = VK_FILTER_NEAREST; sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(device_, &sampler, nullptr, &cacheSampler_) != VK_SUCCESS)
            throw std::runtime_error("Could not create virtual texture cache sampler");

        // A virtual lookup always walks toward the last mip.  Make that mip
        // resident before exposing this object, so a miss has a real fallback
        // from the very first sampled frame.
        const std::uint32_t lastMip = static_cast<std::uint32_t>(source_.mips.size() - 1);
        const auto& tail = source_.mips[lastMip];
        const std::uint32_t pagesX = (tail.width + config_.pageSize - 1) / config_.pageSize;
        const std::uint32_t pagesY = (tail.height + config_.pageSize - 1) / config_.pageSize;
        if (static_cast<std::uint64_t>(pagesX) * pagesY > slots_.size())
            throw std::invalid_argument("Virtual texture cache cannot hold its fallback mip");
        for (std::uint32_t y = 0; y < pagesY; ++y)
            for (std::uint32_t x = 0; x < pagesX; ++x)
                request({lastMip, x, y}, std::numeric_limits<float>::max());
        update();
    }

    void VirtualTexture::destroy() noexcept {
        if (readyTimeline_ != 0) {
            if (UploadContext* const upload = UploadContext::current()) {
                assert(!upload->recording() || readyTimeline_ != upload->pendingTicket().timelineValue);
                if (upload->isSubmitted(readyTimeline_)) upload->wait(readyTimeline_);
            }
        }
        if (device_ != VK_NULL_HANDLE) {
            if (cacheSampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, cacheSampler_, nullptr);
            if (cacheView_ != VK_NULL_HANDLE) vkDestroyImageView(device_, cacheView_, nullptr);
            if (cacheImage_ != VK_NULL_HANDLE) vmaDestroyImage(allocator_, cacheImage_, cacheAllocation_);
        }
        pageTable_.destroy(); mipOffsets_.destroy(); feedback_.destroy(); device_ = VK_NULL_HANDLE; allocator_ = VK_NULL_HANDLE; cacheImage_ = VK_NULL_HANDLE;
        cacheAllocation_ = VK_NULL_HANDLE; cacheView_ = VK_NULL_HANDLE; cacheSampler_ = VK_NULL_HANDLE;
        format_ = VK_FORMAT_UNDEFINED; source_ = {}; mipPageOffsetsCpu_.clear(); pageTableCpu_.clear(); slots_.clear(); requests_.clear();
        readyTimeline_ = 0; frame_ = 0; cacheInitialized_ = false;
    }

    std::uint32_t VirtualTexture::pageIndex(const Page page) const {
        if (page.mip >= source_.mips.size()) throw std::out_of_range("Virtual texture mip index");
        const auto& mip = source_.mips[page.mip];
        const std::uint32_t pagesX = (mip.width + config_.pageSize - 1) / config_.pageSize;
        const std::uint32_t pagesY = (mip.height + config_.pageSize - 1) / config_.pageSize;
        if (page.x >= pagesX || page.y >= pagesY) throw std::out_of_range("Virtual texture page index");
        return mipPageOffsetsCpu_[page.mip] + page.y * pagesX + page.x;
    }

    VirtualTexture::Page VirtualTexture::pageFromIndex(const std::uint32_t index) const {
        if (index >= pageTableCpu_.size()) throw std::out_of_range("Virtual texture page-table index");
        const auto it = std::upper_bound(mipPageOffsetsCpu_.begin(), mipPageOffsetsCpu_.end(), index);
        const std::uint32_t mip = static_cast<std::uint32_t>(std::distance(mipPageOffsetsCpu_.begin(), it) - 1);
        const auto& desc = source_.mips[mip]; const std::uint32_t pagesX = (desc.width + config_.pageSize - 1) / config_.pageSize;
        const std::uint32_t local = index - mipPageOffsetsCpu_[mip];
        return {mip, local % pagesX, local / pagesX};
    }

    void VirtualTexture::request(const Page page, const float priority) {
        const std::uint32_t index = pageIndex(page);
        if (pageTableCpu_[index] != InvalidPage) { slots_[pageTableCpu_[index]].lastUse = frame_; return; }
        requests_[index] = std::max(requests_[index], std::max(priority, 0.0F));
    }

    void VirtualTexture::beginFeedback(const VkCommandBuffer commandBuffer) {
        if (feedback_.handle() == VK_NULL_HANDLE) return;
        const std::uint32_t zero = 0;
        feedback_.update(&zero, sizeof(zero));
        if (commandBuffer != VK_NULL_HANDLE)
            hostWriteBarrier(commandBuffer, feedback_.handle(), VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
                             VK_ACCESS_2_SHADER_WRITE_BIT);
    }

    void VirtualTexture::consumeFeedback(const float priority) {
        if (feedback_.handle() == VK_NULL_HANDLE) return;
        std::uint32_t count{};
        feedback_.read(&count, sizeof(count));
        count = std::min(count, config_.feedbackCapacity);
        if (count == 0) return;
        std::vector<std::uint32_t> indices(count);
        feedback_.read(indices.data(), sizeof(std::uint32_t) * count, sizeof(std::uint32_t));
        for (const std::uint32_t index : indices) {
            if (index < pageTableCpu_.size() && pageTableCpu_[index] == InvalidPage)
                requests_[index] = std::max(requests_[index], std::max(priority, 0.0F));
        }
    }

    void VirtualTexture::flushPageTable() {
        if (!pageTableCpu_.empty()) pageTable_.update(pageTableCpu_.data(), sizeof(std::uint32_t) * pageTableCpu_.size());
    }

    void VirtualTexture::uploadPage(const Page page, const std::uint32_t slot) {
        auto* const upload = UploadContext::current();
        const auto& mip = source_.mips[page.mip]; const std::uint32_t block = blockExtent(source_.format);
        const std::uint32_t bpb = bytesPerBlock(source_.format);
        const std::uint32_t sourceX = page.x * config_.pageSize, sourceY = page.y * config_.pageSize;
        const std::uint32_t width = std::min(config_.pageSize, mip.width - sourceX);
        const std::uint32_t height = std::min(config_.pageSize, mip.height - sourceY);
        const std::uint32_t blocksWide = (mip.width + block - 1) / block;
        const std::uint32_t pageBlocksWide = (width + block - 1) / block;
        const std::uint32_t pageBlocksHigh = (height + block - 1) / block;
        const std::uint32_t firstBlockX = sourceX / block, firstBlockY = sourceY / block;
        const std::uint32_t slotX = (slot % config_.cacheTilesX) * config_.pageSize;
        const std::uint32_t slotY = (slot / config_.cacheTilesX) * config_.pageSize;
        for (std::uint32_t row = 0; row < pageBlocksHigh; ++row) {
            const auto slice = upload->allocate(static_cast<VkDeviceSize>(pageBlocksWide) * bpb, 16);
            auto bytes = std::span<std::uint8_t>{static_cast<std::uint8_t*>(slice.mapped), static_cast<std::size_t>(pageBlocksWide) * bpb};
            const std::uint64_t offset = (static_cast<std::uint64_t>(firstBlockY + row) * blocksWide + firstBlockX) * bpb;
            if (!Assets::readMipRange(source_, page.mip, offset, bytes)) throw std::runtime_error("Could not stream virtual texture page");
            VkBufferImageCopy copy{}; copy.bufferOffset = slice.offset;
            copy.bufferRowLength = pageBlocksWide * block; copy.bufferImageHeight = block;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageOffset = {static_cast<std::int32_t>(slotX), static_cast<std::int32_t>(slotY + row * block), 0};
            copy.imageExtent = {width, std::min(block, height - row * block), 1};
            vkCmdCopyBufferToImage(upload->commandBuffer(), slice.buffer, cacheImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }
    }

    void VirtualTexture::update() {
        ++frame_; if (requests_.empty()) return;
        auto* const upload = UploadContext::current(); if (upload == nullptr) throw std::logic_error("VirtualTexture requires an active UploadContext");
        std::vector<std::pair<std::uint32_t, float>> pending(requests_.begin(), requests_.end()); requests_.clear();
        std::sort(pending.begin(), pending.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        const bool ownsBatch = !upload->recording();
        std::optional<UploadContext::Batch> batch;
        if (ownsBatch) batch.emplace(upload->beginBatch());
        transition(upload->commandBuffer(), cacheImage_,
                   cacheInitialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   cacheInitialized_ ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   cacheInitialized_ ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                   VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        const auto count = std::min<std::size_t>(pending.size(), config_.maxUploadsPerUpdate);
        for (std::size_t i = 0; i < count; ++i) {
            const auto pageIndex = pending[i].first;
            auto victim = std::min_element(slots_.begin(), slots_.end(), [](const Slot& a, const Slot& b) { return a.lastUse < b.lastUse; });
            const auto slot = static_cast<std::uint32_t>(std::distance(slots_.begin(), victim));
            if (victim->page != InvalidPage) pageTableCpu_[victim->page] = InvalidPage;
            uploadPage(pageFromIndex(pageIndex), slot);
            *victim = {pageIndex, frame_}; pageTableCpu_[pageIndex] = slot;
        }
        for (std::size_t i = count; i < pending.size(); ++i) requests_.insert(pending[i]);
        flushPageTable();
        const VkCommandBuffer finalizer = upload->graphicsCommandBuffer();
        hostWriteBarrier(finalizer, pageTable_.handle(), VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        transition(finalizer, cacheImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                   VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
        readyTimeline_ = upload->pendingTicket().timelineValue;
        if (ownsBatch) readyTimeline_ = batch->submit().timelineValue;
        cacheInitialized_ = true;
    }
}
