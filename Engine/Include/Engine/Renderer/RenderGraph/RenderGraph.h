#pragma once

/**
 * @file RenderGraph.h
 * @brief Declarative, single-queue Vulkan frame-graph compiler.
 *
 * A graph owns only the declarations made for one frame. Imported images stay
 * owned by their existing wrappers; transient image allocation is deliberately
 * allocated through VMA and reused when compatible lifetimes do not overlap.
 */

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace Engine::Renderer {

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
        StorageRead,
        StorageWrite,
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
        ~RenderGraph() { reset(); }
        RenderGraph(const RenderGraph&) = delete;
        RenderGraph& operator=(const RenderGraph&) = delete;

        /** Enables physical transient-image allocation for this graph. */
        void initialize(VkDevice device, VmaAllocator allocator);

        [[nodiscard]] TextureHandle importTexture(std::string name, VkImage image,
            const TextureDesc& desc, VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED);
        [[nodiscard]] TextureHandle createTexture(std::string name, const TextureDesc& desc);
        [[nodiscard]] BufferHandle importBuffer(std::string name, VkBuffer buffer, const BufferDesc& desc);
        [[nodiscard]] BufferHandle createBuffer(std::string name, const BufferDesc& desc);

        void addPass(std::string name, const std::function<void(PassBuilder&)>& setup,
                     ExecuteCallback execute);

        /** Validates resource declarations and computes order, lifetimes and barriers. */
        void compile();
        /** Emits automatic barriers immediately before each pass callback. */
        void execute(VkCommandBuffer commandBuffer);
        void reset() noexcept;

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
            VkImageLayout initialLayout{VK_IMAGE_LAYOUT_UNDEFINED};
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
        struct Barrier final { std::uint32_t resource; VkImageMemoryBarrier2 vk; };
        struct BufferBarrier final { std::uint32_t resource; VkBufferMemoryBarrier2 vk; };
        struct TransientAllocation final { VkImage image; VmaAllocation allocation; };
        struct TransientBufferAllocation final { VkBuffer buffer; VmaAllocation allocation; };

        friend class PassBuilder;
        void addAccess(std::uint32_t pass, TextureHandle texture, TextureUsage usage, bool write);
        void addBufferAccess(std::uint32_t pass, BufferHandle buffer, BufferUsage usage, bool write);
        [[nodiscard]] TextureHandle addTransient(std::string name, const TextureDesc& desc,
                                                 std::uint32_t pass, TextureUsage usage);
        void requireValid(TextureHandle texture) const;
        void requireValid(BufferHandle buffer) const;
        void allocateTransients(const std::vector<TextureDesc>& slotDescs);
        void allocateTransientBuffers(const std::vector<BufferDesc>& slotDescs);
        void destroyTransients() noexcept;

        std::vector<Resource> resources_;
        std::vector<BufferResource> buffers_;
        std::vector<Pass> passes_;
        std::vector<std::uint32_t> order_;
        std::vector<std::string> orderNames_;
        std::vector<std::vector<Barrier>> barriers_;
        std::vector<std::vector<BufferBarrier>> bufferBarriers_;
        std::vector<TransientAllocation> transientAllocations_;
        std::vector<TransientBufferAllocation> transientBufferAllocations_;
        VkDevice device_{VK_NULL_HANDLE};
        VmaAllocator allocator_{VK_NULL_HANDLE};
        bool compiled_{};
    };
} // namespace Engine::Renderer
