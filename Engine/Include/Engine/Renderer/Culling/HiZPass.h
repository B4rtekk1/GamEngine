#pragma once

#include "Engine/Renderer/Culling/HiZBuffer.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace Engine::Culling
{
    class HiZPass
    {
    public:
        HiZPass() = default;
        ~HiZPass();

        HiZPass(const HiZPass&) = delete;
        HiZPass& operator=(const HiZPass&) = delete;

        void create(
            VkDevice device,
            VkDescriptorPool descriptorPool,
            VkPipeline copyPipeline,
            VkPipelineLayout copyPipelineLayout,
            VkDescriptorSetLayout copyDescriptorSetLayout,
            VkPipeline reducePipeline,
            VkPipelineLayout reducePipelineLayout,
            VkDescriptorSetLayout reduceDescriptorSetLayout,
            const HiZBuffer& hiZBuffer,
            VkImageView depthImageView,
            VkSampler depthSampler
        );

        void destroy();

        void updateDepthImage(
            VkImageView depthImageView,
            VkSampler depthSampler
        ) const;

        void record(
            VkCommandBuffer commandBuffer,
            const HiZBuffer& hiZBuffer,
            bool hasPreviousContents
        ) const;

    private:
        static std::uint32_t divideRoundUp(
            std::uint32_t value,
            std::uint32_t divisor
        );

        void allocateDescriptorSets(
            const HiZBuffer& hiZBuffer,
            VkImageView depthImageView,
            VkSampler depthSampler
        );

        VkDevice m_device{VK_NULL_HANDLE};
        VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};

        VkPipeline m_copyPipeline{VK_NULL_HANDLE};
        VkPipelineLayout m_copyPipelineLayout{VK_NULL_HANDLE};
        VkDescriptorSetLayout m_copyDescriptorSetLayout{VK_NULL_HANDLE};

        VkPipeline m_reducePipeline{VK_NULL_HANDLE};
        VkPipelineLayout m_reducePipelineLayout{VK_NULL_HANDLE};
        VkDescriptorSetLayout m_reduceDescriptorSetLayout{VK_NULL_HANDLE};

        VkDescriptorSet m_copyDescriptorSet{VK_NULL_HANDLE};
        std::vector<VkDescriptorSet> m_reduceDescriptorSets;
    };
}
