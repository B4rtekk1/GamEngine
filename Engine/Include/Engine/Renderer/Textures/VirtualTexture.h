#pragma once

#include "Engine/Assets/AssetTypes.h"
#include "Engine/Renderer/Vulkan/buffer.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace Engine {
    /** A GPU page cache backed by a mip-tiled GTEX source.  Page-table entries
     * are either 0xffffffff or a physical cache-tile index. */
    class VirtualTexture final {
    public:
        static constexpr std::uint32_t InvalidPage = 0xffffffffU;
        struct Config {
            std::uint32_t pageSize{128}; // Must be a multiple of compressed-format blocks.
            std::uint32_t cacheTilesX{32};
            std::uint32_t cacheTilesY{32};
            std::uint32_t maxUploadsPerUpdate{32};
            std::uint32_t feedbackCapacity{4096};
        };
        struct Page { std::uint32_t mip{}; std::uint32_t x{}; std::uint32_t y{}; };

        VirtualTexture() = default;
        ~VirtualTexture();
        VirtualTexture(const VirtualTexture&) = delete;
        VirtualTexture& operator=(const VirtualTexture&) = delete;
        VirtualTexture(VirtualTexture&&) = delete;
        VirtualTexture& operator=(VirtualTexture&&) = delete;

        void create(VkPhysicalDevice physicalDevice, VkDevice device, VkCommandPool commandPool, VkQueue queue,
                    const Assets::GtexTexture& source, VmaAllocator allocator, Config config = {});
        void destroy() noexcept;

        /** Queue a page requested by GPU feedback. Larger priority wins. */
        void request(Page page, float priority);
        /**
         * Clears the GPU feedback counter before recording a frame.  When a
         * command buffer is supplied, it also records the host-write to
         * shader-write dependency required before the feedback shader runs.
         */
        void beginFeedback(VkCommandBuffer commandBuffer = VK_NULL_HANDLE);
        /**
         * Consume packed page IDs after the GPU work that writes feedback has
         * completed.  The first uint in the feedback buffer is the counter;
         * remaining uints are page-table indices.
         */
        void consumeFeedback(float priority = 1.0F);
        /** Upload queued pages through the active UploadContext, then publish their table entries. */
        void update();

        [[nodiscard]] VkImage cacheImage() const noexcept { return cacheImage_; }
        [[nodiscard]] VkImageView cacheImageView() const noexcept { return cacheView_; }
        [[nodiscard]] VkSampler cacheSampler() const noexcept { return cacheSampler_; }
        [[nodiscard]] VkBuffer pageTableBuffer() const noexcept { return pageTable_.handle(); }
        [[nodiscard]] VkBuffer mipOffsetsBuffer() const noexcept { return mipOffsets_.handle(); }
        [[nodiscard]] VkBuffer feedbackBuffer() const noexcept { return feedback_.handle(); }
        [[nodiscard]] std::uint32_t feedbackCapacity() const noexcept { return config_.feedbackCapacity; }
        [[nodiscard]] std::uint32_t pageTableEntries() const noexcept {
            return static_cast<std::uint32_t>(pageTableCpu_.size());
        }
        [[nodiscard]] std::uint32_t pageSize() const noexcept { return config_.pageSize; }
        [[nodiscard]] std::uint32_t cacheTilesX() const noexcept { return config_.cacheTilesX; }
        [[nodiscard]] std::uint32_t cacheTilesY() const noexcept { return config_.cacheTilesY; }
        [[nodiscard]] std::uint32_t mipCount() const noexcept {
            return static_cast<std::uint32_t>(source_.mips.size());
        }
        [[nodiscard]] bool valid() const noexcept { return cacheImage_ != VK_NULL_HANDLE; }
        [[nodiscard]] const std::vector<std::uint32_t>& mipPageOffsets() const noexcept { return mipPageOffsetsCpu_; }
        [[nodiscard]] std::uint64_t readyTimeline() const noexcept { return readyTimeline_; }

    private:
        struct Slot { std::uint32_t page{InvalidPage}; std::uint64_t lastUse{}; };
        [[nodiscard]] std::uint32_t pageIndex(Page page) const;
        [[nodiscard]] Page pageFromIndex(std::uint32_t index) const;
        void uploadPage(Page page, std::uint32_t slot);
        void flushPageTable();

        VkDevice device_{VK_NULL_HANDLE};
        VmaAllocator allocator_{VK_NULL_HANDLE};
        VkImage cacheImage_{VK_NULL_HANDLE};
        VmaAllocation cacheAllocation_{VK_NULL_HANDLE};
        VkImageView cacheView_{VK_NULL_HANDLE};
        VkSampler cacheSampler_{VK_NULL_HANDLE};
        VkFormat format_{VK_FORMAT_UNDEFINED};
        Assets::GtexTexture source_{};
        Config config_{};
        Buffer pageTable_;
        Buffer feedback_;
        Buffer mipOffsets_;
        std::vector<std::uint32_t> mipPageOffsetsCpu_;
        std::vector<std::uint32_t> pageTableCpu_;
        std::vector<Slot> slots_;
        std::unordered_map<std::uint32_t, float> requests_;
        std::uint64_t frame_{};
        std::uint64_t readyTimeline_{};
        bool cacheInitialized_{};
    };
}
