#pragma once

/**
 * @file GPUCullingPass.h
 * @brief Declares the Vulkan command-recording wrapper for GPU culling.
 */

#include "Engine/Renderer/Culling/HiZBuffer.h"
#include "Engine/Math/Mat4.h"
#include "Engine/Renderer/Vulkan/buffer.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>

namespace Engine::Culling
{
    /// GPU render-work item for one active virtual-shadow page.  The
    /// allocator writes the physical destination and virtual source together;
    /// neither the raster pass nor page-table commit needs to recover that
    /// association from CPU-side allocator state. Keep this layout in sync
    /// with ShadowPageWork in gpu_culling.slang.
    struct ShadowPageWork {
        glm::mat4 viewProjection{1.0F};
        std::uint32_t drawSlot{};
        std::uint32_t clipLevel{};
        std::uint32_t physicalPage{};
        std::uint32_t virtualPage{};
    };
    static_assert(sizeof(ShadowPageWork) == 80);
    /**
     * @brief Records a compute pass that generates indirect draw commands.
     *
     * The pass uses a culling pipeline and descriptor set supplied by the
     * renderer. Its output consists of an indirect draw buffer and a draw
     * count buffer suitable for indirect-count rendering.
     */
    class GPUCullingPass
    {
    public:
        /**
         * @brief Configures the pass and its output buffers.
         * @param device Logical Vulkan device.
         * @param pipeline Compute or graphics pipeline used for culling.
         * @param pipelineLayout Pipeline layout associated with @p pipeline.
         * @param descriptorSet Descriptor set containing culling resources.
         * @param indirectBuffer Buffer receiving generated draw commands.
         * @param drawCountBuffer Buffer receiving the generated command count.
         * @param maxDrawCount Maximum number of draw commands that can be emitted.
         */
        void create(
            VkDevice device,
            VkPipeline pipeline,
            VkPipelineLayout pipelineLayout,
            VkDescriptorSet descriptorSet,
            VkBuffer indirectBuffer,
            VkBuffer drawCountBuffer,
            std::uint32_t maxDrawCount,
            VkBuffer candidateCountBuffer = VK_NULL_HANDLE,
            VkBuffer candidateDispatchBuffer = VK_NULL_HANDLE,
            const Buffer* pageWorkBuffer = nullptr
        );

        /**
         * @brief Records the culling dispatch into a command buffer.
         * @param commandBuffer Command buffer that receives the dispatch.
         * @param objectCount Number of objects to process.
         */
        void record(
            VkCommandBuffer commandBuffer,
            std::uint32_t objectCount,
            const Mat4* viewProjectionOverride = nullptr,
            std::uint32_t drawSlot = 0,
            std::uint32_t shaderFilter = UINT32_MAX
        ) const;

        /**
         * @brief Culls once and appends visible objects to their material bins.
         *
         * The output layout remains [material slot][maxDrawCount], so callers
         * can retain their existing per-material indirect draws.
         */
        void recordBinned(VkCommandBuffer commandBuffer,
                          std::uint32_t objectCount,
                          std::uint32_t binCount) const;

        /// Builds the caster-ID list for one shadow clip level.
        void recordCandidates(VkCommandBuffer commandBuffer, std::uint32_t objectCount,
                              const Mat4& clipMatrix, std::uint32_t clipLevel) const;

        /// Makes all clip-level candidate streams visible to page-culling dispatches.
        void prepareCandidateReads(VkCommandBuffer commandBuffer) const;

        /// Builds the indirect dispatch dimensions for one clip-level candidate stream.
        void recordCandidateDispatchArgs(VkCommandBuffer commandBuffer,
                                         std::uint32_t clipLevel) const;

        /// Culls one virtual page using the candidate list of @p clipLevel.
        void recordCandidatesForPage(VkCommandBuffer commandBuffer, std::uint32_t objectCount,
                                     const Mat4& pageMatrix, std::uint32_t drawSlot,
                                     std::uint32_t clipLevel) const;

        /// Culls every active VSM page in a single 2D dispatch.  The page
        /// dimension selects a ShadowPageWork entry and the X dimension walks
        /// that page's compact clip-level candidate list.
        void recordCandidatesForPages(VkCommandBuffer commandBuffer,
                                      std::uint32_t objectCount,
                                      std::span<const ShadowPageWork> pages) const;

        /** @brief Returns the generated indirect draw-command buffer. */
        [[nodiscard]] VkBuffer indirectBuffer() const noexcept
        {
            return m_indirectBuffer;
        }

        /** @brief Returns the generated draw-count buffer. */
        [[nodiscard]] VkBuffer drawCountBuffer() const noexcept
        {
            return m_drawCountBuffer;
        }

        /** @brief Returns the maximum number of generated draw commands. */
        [[nodiscard]] std::uint32_t maxDrawCount() const noexcept
        {
            return m_maxDrawCount;
        }

    private:
        VkDevice m_device{VK_NULL_HANDLE};

        VkPipeline m_pipeline{VK_NULL_HANDLE};
        VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
        VkDescriptorSet m_descriptorSet{VK_NULL_HANDLE};

        VkBuffer m_indirectBuffer{VK_NULL_HANDLE};
        VkBuffer m_drawCountBuffer{VK_NULL_HANDLE};
        VkBuffer m_candidateCountBuffer{VK_NULL_HANDLE};
        VkBuffer m_candidateDispatchBuffer{VK_NULL_HANDLE};
        const Buffer* m_pageWorkBuffer{};

        std::uint32_t m_maxDrawCount{0};
    };
}
