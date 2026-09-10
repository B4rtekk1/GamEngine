#include "Engine/Renderer/Culling/GPUCullingPass.h"

#include <stdexcept>

namespace {
struct CullingPushConstants {
    glm::mat4 viewProjectionOverride{1.0F};
    std::uint32_t drawSlot{};
    std::uint32_t sourceCount{};
    std::uint32_t candidateLevel{};
    std::uint32_t mode{};
    std::uint32_t shaderFilter{UINT32_MAX};
};
static_assert(sizeof(CullingPushConstants) == 84);
}

namespace Engine::Culling
{
    void GPUCullingPass::create(
        const VkDevice device,
        const VkPipeline pipeline,
        const VkPipelineLayout pipelineLayout,
        const VkDescriptorSet descriptorSet,
        const VkBuffer indirectBuffer,
        const VkBuffer drawCountBuffer,
        const std::uint32_t maxDrawCount,
        const VkBuffer candidateCountBuffer,
        const VkBuffer candidateDispatchBuffer,
        const Buffer* const pageWorkBuffer
    )
    {
        if (
            device == VK_NULL_HANDLE ||
            pipeline == VK_NULL_HANDLE ||
            pipelineLayout == VK_NULL_HANDLE ||
            descriptorSet == VK_NULL_HANDLE ||
            indirectBuffer == VK_NULL_HANDLE ||
            drawCountBuffer == VK_NULL_HANDLE ||
            maxDrawCount == 0
        )
        {
            throw std::invalid_argument(
                "Invalid GPUCullingPass arguments"
            );
        }

        m_device = device;
        m_pipeline = pipeline;
        m_pipelineLayout = pipelineLayout;
        m_descriptorSet = descriptorSet;
        m_indirectBuffer = indirectBuffer;
        m_drawCountBuffer = drawCountBuffer;
        m_candidateCountBuffer = candidateCountBuffer;
        m_candidateDispatchBuffer = candidateDispatchBuffer;
        m_pageWorkBuffer = pageWorkBuffer;
        m_maxDrawCount = maxDrawCount;
    }

    void GPUCullingPass::record(
        const VkCommandBuffer commandBuffer,
        const std::uint32_t objectCount,
        const Mat4* const viewProjectionOverride,
        const std::uint32_t drawSlot,
        const std::uint32_t shaderFilter
    ) const
    {
        if (objectCount == 0)
        {
            return;
        }

        vkCmdFillBuffer(
            commandBuffer,
            m_drawCountBuffer,
            sizeof(std::uint32_t) * drawSlot,
            sizeof(std::uint32_t),
            0
        );

        const VkBufferMemoryBarrier2 clearBarrier{
            .sType =
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask =
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask =
                VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask =
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask =
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .buffer = m_drawCountBuffer,
            .offset = sizeof(std::uint32_t) * drawSlot,
            .size = sizeof(std::uint32_t)
        };

        VkDependencyInfo clearDependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &clearBarrier
        };

        vkCmdPipelineBarrier2(
            commandBuffer,
            &clearDependency
        );

        vkCmdBindPipeline(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            m_pipeline
        );

        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            m_pipelineLayout,
            0,
            1,
            &m_descriptorSet,
            0,
            nullptr
        );

        CullingPushConstants pushConstants{};
        if (viewProjectionOverride != nullptr)
            pushConstants.viewProjectionOverride = viewProjectionOverride->native();
        pushConstants.drawSlot = drawSlot;
        pushConstants.sourceCount = objectCount;
        pushConstants.shaderFilter = shaderFilter;
        vkCmdPushConstants(commandBuffer, m_pipelineLayout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(pushConstants), &pushConstants);

        constexpr std::uint32_t workgroupSize = 64;

        const std::uint32_t groupCount =
            (objectCount + workgroupSize - 1) /
            workgroupSize;

        vkCmdDispatch(
            commandBuffer,
            groupCount,
            1,
            1
        );

        const VkDeviceSize indirectOffset =
            static_cast<VkDeviceSize>(drawSlot) * m_maxDrawCount *
            sizeof(VkDrawIndexedIndirectCommand);
        const VkDeviceSize indirectSize =
            static_cast<VkDeviceSize>(m_maxDrawCount) *
            sizeof(VkDrawIndexedIndirectCommand);
        const VkDeviceSize drawCountOffset =
            static_cast<VkDeviceSize>(drawSlot) * sizeof(std::uint32_t);

        VkBufferMemoryBarrier2 barriers[2]{
            {
                .sType =
                    VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask =
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .srcAccessMask =
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                .dstStageMask =
                    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                .dstAccessMask =
                    VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
                .buffer = m_indirectBuffer,
                .offset = indirectOffset,
                .size = indirectSize
            },
            {
                .sType =
                    VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask =
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .srcAccessMask =
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                .dstStageMask =
                    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                .dstAccessMask =
                    VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
                .buffer = m_drawCountBuffer,
                .offset = drawCountOffset,
                .size = sizeof(std::uint32_t)
            }
        };

        VkDependencyInfo drawDependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 2,
            .pBufferMemoryBarriers = barriers
        };

        vkCmdPipelineBarrier2(
            commandBuffer,
            &drawDependency
        );
    }

    void GPUCullingPass::recordBinned(const VkCommandBuffer commandBuffer,
                                      const std::uint32_t objectCount,
                                      const std::uint32_t binCount) const
    {
        if (objectCount == 0 || binCount == 0)
            return;
        const VkDeviceSize countsSize = static_cast<VkDeviceSize>(binCount) * sizeof(std::uint32_t);
        vkCmdFillBuffer(commandBuffer, m_drawCountBuffer, 0, countsSize, 0);

        const VkBufferMemoryBarrier2 clearBarrier{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .buffer = m_drawCountBuffer,
            .offset = 0,
            .size = countsSize};
        const VkDependencyInfo clearDependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &clearBarrier};
        vkCmdPipelineBarrier2(commandBuffer, &clearDependency);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

        CullingPushConstants pushConstants{};
        pushConstants.sourceCount = objectCount;
        // Mode 3 performs the visibility work once, then bins by object.shader.
        pushConstants.mode = 3;
        vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(pushConstants), &pushConstants);
        vkCmdDispatch(commandBuffer, (objectCount + 63U) / 64U, 1, 1);

        VkBufferMemoryBarrier2 barriers[2]{
            {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
             .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
             .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
             .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
             .buffer = m_indirectBuffer,
             .offset = 0,
             .size = static_cast<VkDeviceSize>(m_maxDrawCount) * binCount *
                     sizeof(VkDrawIndexedIndirectCommand)},
            {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
             .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
             .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
             .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
             .buffer = m_drawCountBuffer,
             .offset = 0,
             .size = countsSize}};
        const VkDependencyInfo drawDependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 2,
            .pBufferMemoryBarriers = barriers};
        vkCmdPipelineBarrier2(commandBuffer, &drawDependency);
    }

    void GPUCullingPass::recordCandidates(const VkCommandBuffer commandBuffer,
                                          const std::uint32_t objectCount,
                                          const Mat4& clipMatrix,
                                          const std::uint32_t clipLevel) const
    {
        if (objectCount == 0 || m_candidateCountBuffer == VK_NULL_HANDLE) return;

        const VkDeviceSize countOffset = sizeof(std::uint32_t) * clipLevel;
        vkCmdFillBuffer(commandBuffer, m_candidateCountBuffer, countOffset,
                        sizeof(std::uint32_t), 0);
        const VkBufferMemoryBarrier2 clearBarrier{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .buffer = m_candidateCountBuffer, .offset = countOffset, .size = sizeof(std::uint32_t)};
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &clearBarrier};
        vkCmdPipelineBarrier2(commandBuffer, &dependency);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                                0, 1, &m_descriptorSet, 0, nullptr);
        CullingPushConstants pushConstants{};
        pushConstants.viewProjectionOverride = clipMatrix.native();
        pushConstants.sourceCount = objectCount;
        pushConstants.candidateLevel = clipLevel;
        pushConstants.mode = 1;
        vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(pushConstants), &pushConstants);
        vkCmdDispatch(commandBuffer, (objectCount + 63U) / 64U, 1, 1);
    }

    void GPUCullingPass::prepareCandidateReads(const VkCommandBuffer commandBuffer) const
    {
        const VkMemoryBarrier2 candidateBarrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        const VkDependencyInfo candidateDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1, .pMemoryBarriers = &candidateBarrier};
        vkCmdPipelineBarrier2(commandBuffer, &candidateDependency);
    }

    void GPUCullingPass::recordCandidateDispatchArgs(const VkCommandBuffer commandBuffer,
                                                      const std::uint32_t clipLevel) const
    {
        if (m_candidateCountBuffer == VK_NULL_HANDLE ||
            m_candidateDispatchBuffer == VK_NULL_HANDLE) return;

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                                0, 1, &m_descriptorSet, 0, nullptr);
        CullingPushConstants pushConstants{};
        pushConstants.candidateLevel = clipLevel;
        pushConstants.mode = 4;
        vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(pushConstants), &pushConstants);
        vkCmdDispatch(commandBuffer, 1, 1, 1);

        const VkBufferMemoryBarrier2 dispatchBarrier{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
            .buffer = m_candidateDispatchBuffer,
            .offset = static_cast<VkDeviceSize>(clipLevel) * sizeof(VkDispatchIndirectCommand),
            .size = sizeof(VkDispatchIndirectCommand)};
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &dispatchBarrier};
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    }

    void GPUCullingPass::recordCandidatesForPage(const VkCommandBuffer commandBuffer,
                                                  const std::uint32_t objectCount,
                                                  const Mat4& pageMatrix,
                                                  const std::uint32_t drawSlot,
                                                  const std::uint32_t clipLevel) const
    {
        if (objectCount == 0 || m_candidateCountBuffer == VK_NULL_HANDLE) return;

        vkCmdFillBuffer(commandBuffer, m_drawCountBuffer, sizeof(std::uint32_t) * drawSlot,
                        sizeof(std::uint32_t), 0);
        const VkBufferMemoryBarrier2 clearBarrier{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .buffer = m_drawCountBuffer,
            .offset = sizeof(std::uint32_t) * drawSlot, .size = sizeof(std::uint32_t)};
        const VkDependencyInfo clearDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &clearBarrier};
        vkCmdPipelineBarrier2(commandBuffer, &clearDependency);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                                0, 1, &m_descriptorSet, 0, nullptr);
        CullingPushConstants pushConstants{};
        pushConstants.viewProjectionOverride = pageMatrix.native();
        pushConstants.drawSlot = drawSlot;
        pushConstants.sourceCount = objectCount;
        pushConstants.candidateLevel = clipLevel;
        pushConstants.mode = 2;
        vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(pushConstants), &pushConstants);
        if (m_candidateDispatchBuffer != VK_NULL_HANDLE) {
            vkCmdDispatchIndirect(commandBuffer, m_candidateDispatchBuffer,
                                  static_cast<VkDeviceSize>(clipLevel) * sizeof(VkDispatchIndirectCommand));
        } else {
            vkCmdDispatch(commandBuffer, (objectCount + 63U) / 64U, 1, 1);
        }

        const VkDeviceSize indirectOffset = static_cast<VkDeviceSize>(drawSlot) * m_maxDrawCount *
                                            sizeof(VkDrawIndexedIndirectCommand);
        VkBufferMemoryBarrier2 barriers[2]{{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, .buffer = m_indirectBuffer,
            .offset = indirectOffset, .size = static_cast<VkDeviceSize>(m_maxDrawCount) * sizeof(VkDrawIndexedIndirectCommand)},
            {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, .buffer = m_drawCountBuffer,
            .offset = sizeof(std::uint32_t) * drawSlot, .size = sizeof(std::uint32_t)}};
        const VkDependencyInfo drawDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 2, .pBufferMemoryBarriers = barriers};
        vkCmdPipelineBarrier2(commandBuffer, &drawDependency);
    }

    void GPUCullingPass::recordCandidatesForPages(const VkCommandBuffer commandBuffer,
                                                   const std::uint32_t objectCount,
                                                   const std::span<const ShadowPageWork> pages) const
    {
        if (objectCount == 0 || pages.empty() || m_candidateCountBuffer == VK_NULL_HANDLE ||
            m_pageWorkBuffer == nullptr) return;

        // Page work is host-visible and frame-local.  Clear every active
        // indirect counter in one transfer operation, then publish both the
        // work list and counters to the sole page-culling dispatch.
        m_pageWorkBuffer->update(pages.data(), sizeof(ShadowPageWork) * pages.size());
        vkCmdFillBuffer(commandBuffer, m_drawCountBuffer, 0,
                        sizeof(std::uint32_t) * pages.size(), 0);
        const VkBufferMemoryBarrier2 barriers[2]{{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
            .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .buffer = m_pageWorkBuffer->handle(), .offset = 0,
            .size = sizeof(ShadowPageWork) * pages.size()},
            {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .buffer = m_drawCountBuffer, .offset = 0,
            .size = sizeof(std::uint32_t) * pages.size()}};
        const VkDependencyInfo clearDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 2, .pBufferMemoryBarriers = barriers};
        vkCmdPipelineBarrier2(commandBuffer, &clearDependency);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                                0, 1, &m_descriptorSet, 0, nullptr);
        CullingPushConstants pushConstants{};
        pushConstants.sourceCount = objectCount;
        pushConstants.mode = 5;
        vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(pushConstants), &pushConstants);
        vkCmdDispatch(commandBuffer, (objectCount + 63U) / 64U,
                      static_cast<std::uint32_t>(pages.size()), 1);

        const VkBufferMemoryBarrier2 drawBarriers[2]{{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, .buffer = m_indirectBuffer,
            .offset = 0, .size = static_cast<VkDeviceSize>(pages.size()) * m_maxDrawCount * sizeof(VkDrawIndexedIndirectCommand)},
            {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, .buffer = m_drawCountBuffer,
            .offset = 0, .size = sizeof(std::uint32_t) * pages.size()}};
        const VkDependencyInfo drawDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 2, .pBufferMemoryBarriers = drawBarriers};
        vkCmdPipelineBarrier2(commandBuffer, &drawDependency);
    }
}
