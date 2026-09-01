#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

namespace Engine::Culling {
    /** Records the compact per-instance frustum-culling dispatch. */
    class GPUInstanceCullingPass final {
    public:
        void create(VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set,
                    VkBuffer visibleCount, VkBuffer visibleInstances);
        void record(VkCommandBuffer commandBuffer, std::uint32_t instanceCount) const;

    private:
        VkPipeline m_pipeline{VK_NULL_HANDLE};
        VkPipelineLayout m_layout{VK_NULL_HANDLE};
        VkDescriptorSet m_set{VK_NULL_HANDLE};
        VkBuffer m_visibleCount{VK_NULL_HANDLE};
        VkBuffer m_visibleInstances{VK_NULL_HANDLE};
    };
} // namespace Engine::Culling
