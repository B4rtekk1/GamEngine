#pragma once

#include "Engine/Renderer/Culling/HiZBuffer.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace Engine::Culling
{
    class GPUCullingPass
    {
    public:
        void create(
            VkDevice device,
            VkPipeline pipeline,
            VkPipelineLayout pipelineLayout,
            VkDescriptorSet descriptorSet,
            VkBuffer indirectBuffer,
            VkBuffer drawCountBuffer,
            std::uint32_t maxDrawCount
        );

        void record(
            VkCommandBuffer commandBuffer,
            std::uint32_t objectCount
        ) const;

        [[nodiscard]] VkBuffer indirectBuffer() const noexcept
        {
            return m_indirectBuffer;
        }

        [[nodiscard]] VkBuffer drawCountBuffer() const noexcept
        {
            return m_drawCountBuffer;
        }

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

        std::uint32_t m_maxDrawCount{0};
    };
}
