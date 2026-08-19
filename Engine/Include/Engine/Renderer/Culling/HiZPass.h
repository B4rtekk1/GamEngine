#pragma once

#include "Engine/Renderer/Culling/HiZBuffer.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

/**
 * @file HiZPass.h
 * @brief Declares the Vulkan pass used to build and maintain a hierarchical Z-buffer.
 */

namespace Engine::Culling
{
    /**
     * @brief Copies the depth buffer and generates reduced hierarchical-Z mip levels.
     *
     * The pass uses one compute pipeline to copy the depth image into the first
     * Hi-Z level and another compute pipeline to reduce each subsequent level.
     * Vulkan handles supplied to this class are borrowed and must remain valid
     * until the pass is destroyed or explicitly released with destroy().
     */
    class HiZPass
    {
    public:
        /** @brief Creates an empty pass object. */
        HiZPass() = default;

        /** @brief Releases resources owned by the pass. */
        ~HiZPass();

        /** @brief Disables copying because the pass owns descriptor-set state. */
        HiZPass(const HiZPass&) = delete;

        /** @brief Disables copy assignment. */
        HiZPass& operator=(const HiZPass&) = delete;

        /**
         * @brief Initializes the pass and allocates its descriptor sets.
         * @param device Vulkan logical device used by the pass.
         * @param descriptorPool Descriptor pool from which descriptor sets are allocated.
         * @param copyPipeline Compute pipeline that copies the source depth image.
         * @param copyPipelineLayout Pipeline layout used by @p copyPipeline.
         * @param copyDescriptorSetLayout Descriptor-set layout used by the copy pipeline.
         * @param reducePipeline Compute pipeline that generates reduced Hi-Z levels.
         * @param reducePipelineLayout Pipeline layout used by @p reducePipeline.
         * @param reduceDescriptorSetLayout Descriptor-set layout used by the reduction pipeline.
         * @param hiZBuffer Destination hierarchical Z-buffer and its mip-level metadata.
         * @param depthImageView Image view of the source depth buffer.
         * @param depthSampler Sampler used to read the source depth buffer.
         */
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

        /** @brief Destroys or releases all pass-owned Vulkan state. */
        void destroy();

        /**
         * @brief Updates the depth-image descriptors used by the copy stage.
         * @param depthImageView Image view of the current source depth buffer.
         * @param depthSampler Sampler used to read the current source depth buffer.
         */
        void updateDepthImage(
            VkImageView depthImageView,
            VkSampler depthSampler
        ) const;

        /**
         * @brief Records commands that build the hierarchical Z-buffer.
         * @param commandBuffer Command buffer receiving the compute commands.
         * @param hiZBuffer Destination Hi-Z buffer and its mip-level information.
         * @param hasPreviousContents Whether the Hi-Z image already contains valid contents.
         *
         * If previous contents exist, the recorded commands can use them when
         * selecting the appropriate image-layout transitions and synchronization.
         */
        void record(
            VkCommandBuffer commandBuffer,
            const HiZBuffer& hiZBuffer,
            bool hasPreviousContents
        ) const;

    private:
        /** @brief Returns ceil(@p value / @p divisor) using integer arithmetic. */
        static std::uint32_t divideRoundUp(
            std::uint32_t value,
            std::uint32_t divisor
        );

        /** @brief Allocates and initializes descriptor sets for all Hi-Z levels. */
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