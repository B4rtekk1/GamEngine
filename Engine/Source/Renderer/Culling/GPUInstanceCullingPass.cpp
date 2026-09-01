#include "Engine/Renderer/Culling/GPUInstanceCullingPass.h"

namespace Engine::Culling {
    void GPUInstanceCullingPass::create(const VkPipeline pipeline, const VkPipelineLayout layout,
                                        const VkDescriptorSet set, const VkBuffer visibleCount,
                                        const VkBuffer visibleInstances) {
        m_pipeline = pipeline; m_layout = layout; m_set = set;
        m_visibleCount = visibleCount; m_visibleInstances = visibleInstances;
    }

    void GPUInstanceCullingPass::record(const VkCommandBuffer commandBuffer,
                                        const std::uint32_t instanceCount) const {
        if (instanceCount == 0 || m_pipeline == VK_NULL_HANDLE) return;
        vkCmdFillBuffer(commandBuffer, m_visibleCount, 0, sizeof(std::uint32_t), 0);
        const VkBufferMemoryBarrier2 clearBarrier{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .buffer = m_visibleCount, .offset = 0, .size = sizeof(std::uint32_t)};
        const VkDependencyInfo clearDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &clearBarrier};
        vkCmdPipelineBarrier2(commandBuffer, &clearDependency);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &m_set, 0, nullptr);
        vkCmdDispatch(commandBuffer, (instanceCount + 63U) / 64U, 1, 1);
        const VkBufferMemoryBarrier2 outputBarrier{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT, .buffer = m_visibleInstances,
            .offset = 0, .size = VK_WHOLE_SIZE};
        const VkDependencyInfo outputDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &outputBarrier};
        vkCmdPipelineBarrier2(commandBuffer, &outputDependency);
    }
} // namespace Engine::Culling
