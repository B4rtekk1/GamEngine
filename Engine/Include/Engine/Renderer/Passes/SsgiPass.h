#pragma once

#include "Engine/Math/Mat4.h"
#include "Engine/Renderer/RenderConfig.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets {
class AssetManager;
}

/** Per-frame inputs of the SSGI trace. Depth and normals come from the opaque prepass. */
struct SsgiFrameInputs final {
    VkImageView depthView = VK_NULL_HANDLE;
    VkSampler depthSampler = VK_NULL_HANDLE;
    VkImageView viewNormalView = VK_NULL_HANDLE;
    VkSampler viewNormalSampler = VK_NULL_HANDLE;
    Mat4 projection;
    /** Current view space to the clip space of the frame captureRadiance() last stored. */
    Mat4 viewToPreviousClip;
};

/**
 * Screen-space diffuse global illumination without ray tracing. A
 * half-resolution visibility-bitmask trace gathers the previous frame's lit
 * colour, a temporal pass accumulates it, and a depth-aware filter upsamples
 * it for Forward PBR, which adds it to the diffuse IBL term. Lighting the
 * capture of frame N with the GI of frame N-1 gives multiple bounces over time.
 */
class SsgiPass final {
  public:
    ~SsgiPass();
    SsgiPass() = default;
    SsgiPass(const SsgiPass&) = delete;
    SsgiPass& operator=(const SsgiPass&) = delete;

    void create(VkPhysicalDevice physicalDevice, VkDevice device, VkExtent2D fullExtent, VmaAllocator allocator,
                Assets::AssetManager& assets, std::span<const std::uint32_t> sharingFamilies = {});
    void destroy() noexcept;
    /** Discards the GI and radiance histories, e.g. after a camera cut. */
    void reset() noexcept;
    /** Clears every image to black so Forward can sample the result before the first trace. */
    void initialize(VkCommandBuffer commandBuffer) {
        clearImages(commandBuffer, false);
    }
    [[nodiscard]] bool initialized() const noexcept {
        return initialized_;
    }
    /**
     * Records trace, temporal accumulation and the full-resolution resolve.
     * The result is written in GENERAL; the render graph owns its transitions.
     * Before initialize() the graph must import the result as UNDEFINED; the
     * internal images are then cleared here, on the queue that traces.
     */
    void record(VkCommandBuffer commandBuffer, std::uint32_t frameSlot, const SsgiFrameInputs& inputs,
                const SsgiSettings& settings);
    /** Stores the lit HDR colour (SHADER_READ_ONLY) as the radiance the next trace gathers. */
    void captureRadiance(VkCommandBuffer commandBuffer, std::uint32_t frameSlot, VkImageView sceneColor,
                         VkSampler sceneColorSampler);

    [[nodiscard]] VkImage resultImage() const noexcept {
        return full_.image();
    }
    [[nodiscard]] VkImageView resultView() const noexcept {
        return full_.imageView();
    }
    [[nodiscard]] VkSampler resultSampler() const noexcept {
        return full_.sampler();
    }
    [[nodiscard]] VkExtent2D resultExtent() const noexcept {
        return fullExtent_;
    }
    static constexpr VkFormat ResultFormat = HdrBuffer::Format;

  private:
    static constexpr std::uint32_t FramesInFlight = 2;
    static constexpr std::uint32_t MaxBindings = 4;
    // Temporal and resolve sets alternate with the history ping-pong.
    static constexpr std::uint32_t MaxVariants = 2;
    enum class Kernel : std::uint8_t { Capture, Prepare, Trace, Temporal, Resolve };
    static constexpr std::size_t KernelCount = 5;
    struct ComputeKernel final {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::array<VkDescriptorType, MaxBindings> bindingTypes{};
        std::uint32_t bindingCount = 0;
    };
    struct DescriptorSlot final {
        VkDescriptorSet set = VK_NULL_HANDLE;
        std::array<VkDescriptorImageInfo, MaxBindings> written{};
        bool valid = false;
    };

    void clearImages(VkCommandBuffer commandBuffer, bool graphOwnsResultState);
    void createKernel(Kernel kernel, std::span<const VkDescriptorType> bindings, std::uint32_t pushConstantSize,
                      Assets::AssetManager& assets, const char* shader);
    void bindKernel(VkCommandBuffer commandBuffer, Kernel kernel, std::uint32_t frame, std::uint32_t variant,
                    std::span<const VkDescriptorImageInfo> images, const void* pushConstants,
                    std::uint32_t pushConstantSize);
    [[nodiscard]] ComputeKernel& kernel(Kernel kernel) noexcept {
        return kernels_[static_cast<std::size_t>(kernel)];
    }

    VkDevice device_ = VK_NULL_HANDLE;
    PFN_vkCmdBeginDebugUtilsLabelEXT beginLabel_ = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT endLabel_ = nullptr;
    VkExtent2D fullExtent_{};
    VkExtent2D halfExtent_{};
    // Previous frame's lit colour at trace resolution.
    HdrBuffer radiance_;
    // Per-frame trace output; rgb irradiance, a linear depth.
    HdrBuffer raw_;
    // Per-frame trace input: linear depth and packed view normal, R32G32_UINT.
    HdrBuffer geometry_;
    // Accumulated GI ping-pong, same encoding as raw_.
    std::array<HdrBuffer, 2> history_;
    HdrBuffer full_;
    std::array<ComputeKernel, KernelCount> kernels_{};
    std::array<std::array<std::array<DescriptorSlot, MaxVariants>, FramesInFlight>, KernelCount> slots_{};
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::uint32_t historyIndex_ = 0;
    std::uint32_t frameCounter_ = 0;
    bool historyValid_ = false;
    bool radianceValid_ = false;
    bool initialized_ = false;
};
} // namespace Engine
