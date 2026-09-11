#pragma once

/**
 * @file RenderGraph.h
 * @brief Declarative, single-queue Vulkan frame-graph compiler.
 *
 * A graph owns only the declarations made for one frame. Imported images stay
 * owned by their existing wrappers.  Physical transient allocations belong to
 * a persistent pool: reset() releases logical declarations, never GPU memory.
 */

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace Engine::RenderGraph {

    struct TextureHandle final {
        std::uint32_t index{std::numeric_limits<std::uint32_t>::max()};
        [[nodiscard]] explicit operator bool() const noexcept {
            return index != std::numeric_limits<std::uint32_t>::max();
        }
        friend bool operator==(TextureHandle, TextureHandle) = default;
    };
    struct BufferHandle final {
        std::uint32_t index{std::numeric_limits<std::uint32_t>::max()};
        [[nodiscard]] explicit operator bool() const noexcept { return index != std::numeric_limits<std::uint32_t>::max(); }
        friend bool operator==(BufferHandle, BufferHandle) = default;
    };

    enum class TextureUsage : std::uint8_t {
        SampledRead,
        SampledReadVertex,
        SampledReadFragment,
        SampledReadCompute,
        StorageRead,
        StorageWrite,
        StorageReadCompute,
        StorageWriteCompute,
        ColorAttachment,
        DepthAttachment,
        TransferRead,
        TransferWrite,
        Present,
    };

    struct TextureDesc final {
        VkExtent3D extent{1, 1, 1};
        VkFormat format{VK_FORMAT_UNDEFINED};
        VkImageUsageFlags usage{};
        VkImageAspectFlags aspect{VK_IMAGE_ASPECT_COLOR_BIT};
        std::uint32_t mipLevels{1};
        std::uint32_t arrayLayers{1};
        VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};

        [[nodiscard]] bool compatibleWith(const TextureDesc& other) const noexcept;
    };

    /** The state in which an imported image enters this graph. */
    struct TextureState final {
        VkPipelineStageFlags2 stage{VK_PIPELINE_STAGE_2_NONE};
        VkAccessFlags2 access{VK_ACCESS_2_NONE};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
        bool write{};
    };

    struct TextureLifetime final {
        std::uint32_t firstPass{};
        std::uint32_t lastPass{};
        std::uint32_t allocationSlot{};
    };
    enum class BufferUsage : std::uint8_t { UniformRead, StorageRead, StorageWrite, VertexRead, IndexRead, IndirectRead, TransferRead, TransferWrite };
    struct BufferDesc final {
        VkDeviceSize size{};
        VkBufferUsageFlags usage{};
        [[nodiscard]] bool compatibleWith(const BufferDesc& other) const noexcept;
    };
    struct BufferLifetime final { std::uint32_t firstPass{}; std::uint32_t lastPass{}; std::uint32_t allocationSlot{}; };

    class RenderGraph;

    class PassBuilder final {
    public:
        void read(TextureHandle texture, TextureUsage usage = TextureUsage::SampledRead);
        void write(TextureHandle texture, TextureUsage usage);
        [[nodiscard]] TextureHandle writeTexture(std::string name, const TextureDesc& desc,
                                                  TextureUsage usage = TextureUsage::StorageWrite);
        void read(BufferHandle buffer, BufferUsage usage = BufferUsage::StorageRead);
        void write(BufferHandle buffer, BufferUsage usage = BufferUsage::StorageWrite);
        [[nodiscard]] BufferHandle writeBuffer(std::string name, const BufferDesc& desc,
                                                BufferUsage usage = BufferUsage::StorageWrite);

    private:
        friend class RenderGraph;
        PassBuilder(RenderGraph& graph, std::uint32_t pass) : graph_(graph), pass_(pass) {}
        RenderGraph& graph_;
        std::uint32_t pass_;
    };

    /** A compiled graph is valid only until reset() or the next addPass(). */
    class RenderGraph final {
    public:
        using ExecuteCallback = std::function<void(VkCommandBuffer)>;

        RenderGraph() = default;
        ~RenderGraph() { destroyTransientPool(); }
        RenderGraph(const RenderGraph&) = delete;
        RenderGraph& operator=(const RenderGraph&) = delete;

        /** Enables physical transient allocation and its persistent pool. */
        void initialize(VkDevice device, VmaAllocator allocator);

        [[nodiscard]] TextureHandle importTexture(std::string name, VkImage image,
            const TextureDesc& desc, TextureState initialState = {});
        [[nodiscard]] TextureHandle importTexture(std::string name, VkImage image,
            const TextureDesc& desc, VkImageLayout initialLayout) {
            return importTexture(std::move(name), image, desc, {.layout = initialLayout});
        }
        [[nodiscard]] TextureHandle createTexture(std::string name, const TextureDesc& desc);
        [[nodiscard]] BufferHandle importBuffer(std::string name, VkBuffer buffer, const BufferDesc& desc);
        [[nodiscard]] BufferHandle createBuffer(std::string name, const BufferDesc& desc);

        void addPass(std::string name, const std::function<void(PassBuilder&)>& setup,
                     ExecuteCallback execute);

        /** Validates resource declarations and computes order, lifetimes and barriers. */
        void compile();
        /** Emits automatic barriers immediately before each pass callback. */
        void execute(VkCommandBuffer commandBuffer);
        /**
         * Starts a new logical frame. The caller must ensure commands using
         * the previous graph have completed before reusing the pool.
         */
        void reset() noexcept;
        /** Explicitly releases all cached transient physical allocations. */
        void trimTransientPool() noexcept;

        [[nodiscard]] const std::vector<std::string>& executionOrder() const noexcept;
        [[nodiscard]] const TextureLifetime& lifetime(TextureHandle texture) const;
        [[nodiscard]] const BufferLifetime& lifetime(BufferHandle buffer) const;
        [[nodiscard]] VkImage image(TextureHandle texture) const;
        [[nodiscard]] VkBuffer buffer(BufferHandle buffer) const;

    private:
        struct Access final { TextureHandle texture; TextureUsage usage; bool write; };
        struct BufferAccess final { BufferHandle buffer; BufferUsage usage; bool write; };
        struct Resource final {
            std::string name;
            TextureDesc desc;
            VkImage image{VK_NULL_HANDLE};
            TextureState initialState{};
            bool imported{};
            TextureLifetime lifetime{};
        };
        struct BufferResource final { std::string name; BufferDesc desc; VkBuffer buffer{VK_NULL_HANDLE}; bool imported{}; BufferLifetime lifetime{}; };
        struct Pass final {
            std::string name;
            std::vector<Access> accesses;
            std::vector<BufferAccess> bufferAccesses;
            ExecuteCallback execute;
        };
        // These are complete, physical Vulkan barriers after compile().  Keeping
        // them contiguous means execute() only borrows their storage; it never
        // builds per-pass vectors on the render thread.
        struct BarrierBatch final {
            std::vector<VkImageMemoryBarrier2> images;
            std::vector<VkBufferMemoryBarrier2> buffers;
        };
        struct TransientAllocation final { TextureDesc desc; VkImage image; VmaAllocation allocation; };
        struct TransientBufferAllocation final { BufferDesc desc; VkBuffer buffer; VmaAllocation allocation; };
        struct TopologyCache final {
            std::uint64_t signature{};
            std::vector<std::uint32_t> order;
            bool valid{};
        };

        friend class PassBuilder;
        void addAccess(std::uint32_t pass, TextureHandle texture, TextureUsage usage, bool write);
        void addBufferAccess(std::uint32_t pass, BufferHandle buffer, BufferUsage usage, bool write);
        [[nodiscard]] TextureHandle addTransient(std::string name, const TextureDesc& desc,
                                                 std::uint32_t pass, TextureUsage usage);
        void requireValid(TextureHandle texture) const;
        void requireValid(BufferHandle buffer) const;
        void allocateTransients(const std::vector<TextureDesc>& slotDescs);
        void allocateTransientBuffers(const std::vector<BufferDesc>& slotDescs);
        void destroyTransientPool() noexcept;

        std::vector<Resource> resources_;
        std::vector<BufferResource> buffers_;
        std::vector<Pass> passes_;
        std::vector<std::uint32_t> order_;
        std::vector<std::string> orderNames_;
        std::vector<BarrierBatch> barriers_;
        // Pools outlive a logical graph. The index arrays lease one pool item
        // per allocation slot of the currently compiled graph.
        std::vector<TransientAllocation> transientAllocations_;
        std::vector<TransientBufferAllocation> transientBufferAllocations_;
        std::vector<std::uint32_t> transientImageSlots_;
        std::vector<std::uint32_t> transientBufferSlots_;
        // reset() deliberately retains these compilation artifacts. Resource
        // handles may change every frame; the declarative topology does not.
        std::vector<TopologyCache> topologyCaches_;
        VkDevice device_{VK_NULL_HANDLE};
        VmaAllocator allocator_{VK_NULL_HANDLE};
        bool compiled_{};
    };
} // namespace Engine::RenderGraph
