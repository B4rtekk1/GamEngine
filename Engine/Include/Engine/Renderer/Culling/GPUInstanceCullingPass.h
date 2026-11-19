#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

namespace Engine::Culling {
/** Records the compact per-instance frustum-culling dispatch. */
class GPUInstanceCullingPass final {
  public:
    void create(VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set, VkBuffer visibleCount,
                VkBuffer visibleInstances);
    void record(VkCommandBuffer commandBuffer, std::uint32_t instanceCount) const;
    /**
     * Split form of record(): clears the visible count without a barrier, so
     * the caller can batch it with other clears behind one transfer -> compute
     * barrier, then dispatches with recordCull(). Both skip the same cases as
     * record().
     */
    void recordClear(VkCommandBuffer commandBuffer, std::uint32_t instanceCount) const;
    void recordCull(VkCommandBuffer commandBuffer, std::uint32_t instanceCount) const;

  private:
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_layout{VK_NULL_HANDLE};
    VkDescriptorSet m_set{VK_NULL_HANDLE};
    VkBuffer m_visibleCount{VK_NULL_HANDLE};
    VkBuffer m_visibleInstances{VK_NULL_HANDLE};
};
} // namespace Engine::Culling
