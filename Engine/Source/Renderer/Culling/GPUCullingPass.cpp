#include "Engine/Renderer/Culling/GPUCullingPass.h"

#include <stdexcept>

namespace {
struct CullingPushConstants {
    glm::mat4 viewProjectionOverride{1.0F};
    std::uint32_t drawSlot{};
    std::uint32_t padding[3]{};
};
static_assert(sizeof(CullingPushConstants) == 80);
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
        const std::uint32_t maxDrawCount
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
        m_maxDrawCount = maxDrawCount;
    }

    void GPUCullingPass::record(
        const VkCommandBuffer commandBuffer,
        const std::uint32_t objectCount,
        const Mat4* const viewProjectionOverride,
        const std::uint32_t drawSlot
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
            .offset = sizeof(std::uint32_t) * drawSlot,
                .size = VK_WHOLE_SIZE
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
                .offset = 0,
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
}
