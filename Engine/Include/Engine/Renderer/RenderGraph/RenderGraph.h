#pragma once

/**
 * @file RenderGraph.h
 * @brief Declarative Vulkan frame-graph compiler with queue-aware dependencies.
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

    enum class Queue : std::uint8_t { Graphics, AsyncCompute, Transfer };

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
        /** Sample a depth image without losing its depth/stencil read-only layout. */
        DepthReadFragment,
        DepthReadCompute,
        StorageRead,
        StorageWrite,
        StorageReadCompute,
        StorageWriteCompute,
        ColorAttachment,
        DepthAttachment,
        TransferRead,
        TransferWrite,
        Present,
        /** A legacy pass consumes this imported image at an unknown stage. */
        ExternalRead,
    };

    struct TextureDesc final {
        VkExtent3D extent{1, 1, 1};
        VkFormat format{VK_FORMAT_UNDEFINED};
        VkImageUsageFlags usage{};
        VkImageAspectFlags aspect{VK_IMAGE_ASPECT_COLOR_BIT};
        std::uint32_t mipLevels{1};
        std::uint32_t arrayLayers{1};
        VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
        bool concurrentSharing{};

        [[nodiscard]] bool compatibleWith(const TextureDesc& other) const noexcept;
    };

    /** The state in which an imported image enters this graph. */
    struct TextureState final {
        VkPipelineStageFlags2 stage{VK_PIPELINE_STAGE_2_NONE};
        VkAccessFlags2 access{VK_ACCESS_2_NONE};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
        bool write{};
    };

    /** A normalized mip/layer slice. A zero count means "through the end". */
    struct TextureSubresourceRange final {
        std::uint32_t baseMipLevel{};
        std::uint32_t levelCount{};
        std::uint32_t baseArrayLayer{};
        std::uint32_t layerCount{};
    };

    struct TextureLifetime final {
        std::uint32_t firstPass{};
        std::uint32_t lastPass{};
        std::uint32_t allocationSlot{};
    };
    enum class BufferUsage : std::uint8_t {
        UniformRead, StorageRead, StorageWrite,
        StorageReadCompute, StorageWriteCompute,
        StorageReadFragment, StorageWriteFragment,
        VertexRead, IndexRead, MeshRead, IndirectRead, TransferRead, TransferWrite,
        /** A legacy pass consumes this imported buffer at an unknown shader stage. */
        ExternalRead
    };
    struct BufferDesc final {
        VkDeviceSize size{};
        VkBufferUsageFlags usage{};
        [[nodiscard]] bool compatibleWith(const BufferDesc& other) const noexcept;
    };
    struct BufferLifetime final { std::uint32_t firstPass{}; std::uint32_t lastPass{}; std::uint32_t allocationSlot{}; };

    /** A semaphore dependency that must be honoured when passes use different queues. */
    struct QueueDependency final {
        std::uint32_t producerPass{};
        std::uint32_t consumerPass{};
        Queue producerQueue{Queue::Graphics};
        Queue consumerQueue{Queue::Graphics};
    };

    /** A submission-sized run of passes for one hardware queue. */
    struct QueueBatch final {
        Queue queue{Queue::Graphics};
        std::vector<std::uint32_t> passes;
        std::vector<std::uint32_t> waitBatches;
    };

    /**
     * The earliest pass that consumes pre-existing contents of an imported
     * resource.  This is the destination execution scope for an external
     * producer, such as an UploadContext timeline semaphore.
     */
    struct FirstConsumer final {
        VkPipelineStageFlags2 stage{VK_PIPELINE_STAGE_2_NONE};
        Queue queue{Queue::Graphics};

        [[nodiscard]] explicit operator bool() const noexcept {
            return stage != VK_PIPELINE_STAGE_2_NONE;
        }
    };

    /** A timeline value and destination scope required by one graph submission batch. */
    struct UploadWait final {
        std::uint64_t timelineValue{};
        VkPipelineStageFlags2 stage{VK_PIPELINE_STAGE_2_NONE};
        Queue queue{Queue::Graphics};
        std::uint32_t batch{};
    };

    /** An external semaphore wait attached to a compiled submission batch. */
    struct ExternalSemaphoreWait final {
        VkSemaphore semaphore{VK_NULL_HANDLE};
        std::uint64_t value{}; // zero for a binary semaphore
        VkPipelineStageFlags2 stage{VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        std::uint32_t batch{};
    };

    /** A semaphore signalled after every graph batch has completed. */
    struct ExternalSemaphoreSignal final {
        VkSemaphore semaphore{VK_NULL_HANDLE};
        std::uint64_t value{}; // zero for a binary semaphore
        VkPipelineStageFlags2 stage{VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    };

    /**
     * Runtime services required to turn the compiled schedule into Vulkan
     * command buffers and queue submissions.  Command buffers are one-to-one
     * with queueBatches(), and are reset and recorded by RenderGraph.
     */
    struct SubmissionContext final {
        VkQueue queues[3]{VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
        std::vector<VkCommandBuffer> commandBuffers;
        VkSemaphore queueTimelines[3]{VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
        std::uint64_t* nextQueueTimelineValues[3]{};
        VkSemaphore uploadTimeline{VK_NULL_HANDLE};
        std::vector<ExternalSemaphoreWait> externalWaits;
        std::vector<ExternalSemaphoreSignal> completionSignals;
        VkFence completionFence{VK_NULL_HANDLE};
    };

    class RenderGraph;

    class PassBuilder final {
    public:
        void read(TextureHandle texture, TextureUsage usage = TextureUsage::SampledRead);
        void read(TextureHandle texture, TextureUsage usage, TextureSubresourceRange range);
        void write(TextureHandle texture, TextureUsage usage);
        void write(TextureHandle texture, TextureUsage usage, TextureSubresourceRange range);
        /**
         * Transitions the texture after the pass callback and establishes the
         * resulting state as the source of the next dependency.
         */
        void setFinalTextureState(TextureHandle texture, TextureState state);
        void setFinalTextureState(TextureHandle texture, TextureState state, TextureSubresourceRange range);
        [[nodiscard]] TextureHandle writeTexture(std::string name, const TextureDesc& desc,
                                                  TextureUsage usage = TextureUsage::StorageWrite);
        void read(BufferHandle buffer, BufferUsage usage = BufferUsage::StorageRead);
        void write(BufferHandle buffer, BufferUsage usage = BufferUsage::StorageWrite);
        [[nodiscard]] BufferHandle writeBuffer(std::string name, const BufferDesc& desc,
                                                BufferUsage usage = BufferUsage::StorageWrite);
        /**
         * Keeps this pass alive when pass culling is enabled. Use this only
         * for work observed outside graph resources (for example a legacy
         * callback during migration, query writes, or an external encoder).
         */
        void setSideEffect();
        /** Starts a new submission batch at this pass, even when its queue matches the previous pass. */
        void startNewBatch();

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

        /**
         * Marks a resource as externally observable. When pass culling is
         * enabled, only producers that contribute to an exported resource and
         * their dependencies remain in the compiled graph.
         */
        void exportTexture(TextureHandle texture);
        void exportBuffer(BufferHandle buffer);
        void enablePassCulling(bool enabled = true) noexcept;
        /** Associates an imported resource with the UploadContext timeline value that made it ready. */
        void markUploaded(TextureHandle texture, std::uint64_t timelineValue);
        void markUploaded(BufferHandle buffer, std::uint64_t timelineValue);

        void addPass(std::string name, const std::function<void(PassBuilder&)>& setup,
                     ExecuteCallback execute);
        void addPass(std::string name, Queue queue, const std::function<void(PassBuilder&)>& setup,
                     ExecuteCallback execute);

        /** Sets queue-family indices used for ownership transfers. Call before compile(). */
        void setQueueFamily(Queue queue, std::uint32_t family) noexcept;

        /** Validates resource declarations and computes order, lifetimes and barriers. */
        void compile();
        /** Emits automatic barriers immediately before each pass callback. */
        void execute(VkCommandBuffer commandBuffer);
        /** Emits only work assigned to queue. Cross-queue acquire/release barriers are included. */
        void execute(Queue queue, VkCommandBuffer commandBuffer);
        /** Records and submits every compiled queue batch using synchronization2. */
        void recordAndSubmit(const SubmissionContext& context);
        /**
         * Starts a new logical frame. The caller must ensure commands using
         * the previous graph have completed before reusing the pool.
         */
        void reset() noexcept;
        /** Explicitly releases all cached transient physical allocations. */
        void trimTransientPool() noexcept;

        [[nodiscard]] const std::vector<std::string>& executionOrder() const noexcept;
        [[nodiscard]] const std::vector<QueueDependency>& queueDependencies() const noexcept;
        [[nodiscard]] const std::vector<QueueBatch>& queueBatches() const noexcept;
        /** Waits grouped by submission batch, at the first actual consumer stages. */
        [[nodiscard]] const std::vector<UploadWait>& uploadWaits() const noexcept;
        /**
         * Returns the first read of imported contents in compiled execution
         * order. Writes alone deliberately do not count: they can overwrite
         * an upload and therefore do not need to wait for it.
         */
        [[nodiscard]] FirstConsumer firstConsumer(TextureHandle texture) const;
        [[nodiscard]] FirstConsumer firstConsumer(BufferHandle buffer) const;
        [[nodiscard]] const TextureLifetime& lifetime(TextureHandle texture) const;
        [[nodiscard]] const BufferLifetime& lifetime(BufferHandle buffer) const;
        [[nodiscard]] VkImage image(TextureHandle texture) const;
        [[nodiscard]] VkBuffer buffer(BufferHandle buffer) const;

    private:
        struct Access final { TextureHandle texture; TextureUsage usage; TextureSubresourceRange range; bool write; };
        struct BufferAccess final { BufferHandle buffer; BufferUsage usage; bool write; };
        struct Resource final {
            std::string name;
            TextureDesc desc;
            VkImage image{VK_NULL_HANDLE};
            TextureState initialState{};
            bool imported{};
            std::uint64_t uploadTimeline{};
            TextureLifetime lifetime{};
        };
        struct BufferResource final { std::string name; BufferDesc desc; VkBuffer buffer{VK_NULL_HANDLE}; bool imported{}; std::uint64_t uploadTimeline{}; BufferLifetime lifetime{}; };
        struct Pass final {
            std::string name;
            Queue queue{Queue::Graphics};
            std::vector<Access> accesses;
            struct FinalTextureState final { TextureHandle texture; TextureState state; TextureSubresourceRange range; };
            std::vector<FinalTextureState> finalTextureStates;
            std::vector<BufferAccess> bufferAccesses;
            bool sideEffect{};
            bool startsNewBatch{};
            ExecuteCallback execute;
        };
        // These are complete, physical Vulkan barriers after compile().  Keeping
        // them contiguous means execute() only borrows their storage; it never
        // builds per-pass vectors on the render thread.
        struct BarrierBatch final {
            std::vector<VkImageMemoryBarrier2> images;
            std::vector<VkBufferMemoryBarrier2> buffers;
            // Logical bindings remain alongside the precomputed Vulkan barrier
            // plan. They make rebinding independent of the physical handles
            // captured when a template was first compiled.
            std::vector<std::uint32_t> imageResources;
            std::vector<std::uint32_t> bufferResources;
        };
        struct TransientAllocation final { TextureDesc desc; VkImage image; VmaAllocation allocation; };
        struct TransientBufferAllocation final { BufferDesc desc; VkBuffer buffer; VmaAllocation allocation; };
        struct TopologyCache final {
            std::uint64_t signature{};
            std::vector<std::uint32_t> order;
            bool valid{};
        };
        // The point at which an imported resource first needs uploaded
        // contents.  Timeline values are deliberately not cached: they are a
        // frame-local binding, whereas this consumer location is structural.
        struct UploadConsumer final {
            std::uint32_t batch{std::numeric_limits<std::uint32_t>::max()};
            VkPipelineStageFlags2 stage{VK_PIPELINE_STAGE_2_NONE};
        };
        /**
         * Immutable scheduling/allocation plan for one declarative graph shape.
         * Vulkan objects, upload timeline values and callbacks deliberately do
         * not live here: they are frame-local bindings.
         */
        struct CompiledTemplate final {
            std::uint64_t signature{};
            std::vector<std::uint32_t> order;
            std::vector<QueueDependency> queueDependencies;
            std::vector<QueueBatch> queueBatches;
            std::vector<TextureLifetime> textureLifetimes;
            std::vector<BufferLifetime> bufferLifetimes;
            std::vector<TextureDesc> imageSlotDescs;
            std::vector<BufferDesc> bufferSlotDescs;
            std::vector<std::uint32_t> imageAliasPredecessors;
            std::vector<std::uint32_t> bufferAliasPredecessors;
            std::vector<UploadConsumer> textureUploadConsumers;
            std::vector<UploadConsumer> bufferUploadConsumers;
            std::vector<BarrierBatch> barriers;
            std::vector<BarrierBatch> releaseBarriers;
            bool valid{};
        };

        friend class PassBuilder;
        void addAccess(std::uint32_t pass, TextureHandle texture, TextureUsage usage, TextureSubresourceRange range, bool write);
        void addFinalTextureState(std::uint32_t pass, TextureHandle texture, TextureState state, TextureSubresourceRange range);
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
        std::vector<BarrierBatch> releaseBarriers_;
        std::vector<QueueDependency> queueDependencies_;
        std::vector<QueueBatch> queueBatches_;
        std::vector<UploadWait> uploadWaits_;
        std::vector<TextureHandle> exportedTextures_;
        std::vector<BufferHandle> exportedBuffers_;
        // Pools outlive a logical graph. The index arrays lease one pool item
        // per allocation slot of the currently compiled graph.
        std::vector<TransientAllocation> transientAllocations_;
        std::vector<TransientBufferAllocation> transientBufferAllocations_;
        std::vector<std::uint32_t> transientImageSlots_;
        std::vector<std::uint32_t> transientBufferSlots_;
        // reset() deliberately retains these compilation artifacts. Resource
        // handles may change every frame; the declarative topology does not.
        std::vector<TopologyCache> topologyCaches_;
        // Unlike topologyCaches_, this retains every CPU-only result needed to
        // compile an identical frame graph. reset() intentionally preserves it.
        std::vector<CompiledTemplate> compiledTemplates_;
        VkDevice device_{VK_NULL_HANDLE};
        VmaAllocator allocator_{VK_NULL_HANDLE};
        std::uint32_t queueFamilies_[3]{VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED};
        bool passCullingEnabled_{};
        bool compiled_{};
    };
} // namespace Engine::RenderGraph
