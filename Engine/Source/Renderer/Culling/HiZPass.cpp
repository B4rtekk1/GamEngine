#include "Engine/Renderer/Culling/HiZPass.h"

#include <array>
#include <stdexcept>
#include <vector>

namespace Engine::Culling
{
    HiZPass::~HiZPass()
    {
        destroy();
    }

    void HiZPass::create(
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
    )
    {
        destroy();

        m_device = device;
        m_descriptorPool = descriptorPool;

        m_copyPipeline = copyPipeline;
        m_copyPipelineLayout = copyPipelineLayout;
        m_copyDescriptorSetLayout = copyDescriptorSetLayout;

        m_reducePipeline = reducePipeline;
        m_reducePipelineLayout = reducePipelineLayout;
        m_reduceDescriptorSetLayout = reduceDescriptorSetLayout;

        allocateDescriptorSets(
            hiZBuffer,
            depthImageView,
            depthSampler
        );
    }

    void HiZPass::destroy()
    {
        m_copyDescriptorSet = VK_NULL_HANDLE;
        m_reduceDescriptorSets.clear();

        m_device = VK_NULL_HANDLE;
        m_descriptorPool = VK_NULL_HANDLE;

        m_copyPipeline = VK_NULL_HANDLE;
        m_copyPipelineLayout = VK_NULL_HANDLE;
        m_copyDescriptorSetLayout = VK_NULL_HANDLE;

        m_reducePipeline = VK_NULL_HANDLE;
        m_reducePipelineLayout = VK_NULL_HANDLE;
        m_reduceDescriptorSetLayout = VK_NULL_HANDLE;
    }

    std::uint32_t HiZPass::divideRoundUp(
        const std::uint32_t value,
        const std::uint32_t divisor
    )
    {
        return (value + divisor - 1u) / divisor;
    }

    void HiZPass::allocateDescriptorSets(
        const HiZBuffer& hiZBuffer,
        VkImageView depthImageView,
        VkSampler depthSampler
    )
    {
        VkDescriptorSetAllocateInfo copyAllocateInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = m_descriptorPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &m_copyDescriptorSetLayout
        };

        if (vkAllocateDescriptorSets(
                m_device,
                &copyAllocateInfo,
                &m_copyDescriptorSet
            ) != VK_SUCCESS)
        {
            throw std::runtime_error(
                "Failed to allocate Hi-Z copy descriptor set"
            );
        }

        VkDescriptorImageInfo sourceDepthInfo{
            .sampler = depthSampler,
            .imageView = depthImageView,
            .imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };

        VkDescriptorImageInfo destinationMipInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = hiZBuffer.mipView(0),
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL
        };

        std::array<VkWriteDescriptorSet, 2> copyWrites{
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = m_copyDescriptorSet,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType =
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &sourceDepthInfo
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = m_copyDescriptorSet,
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType =
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &destinationMipInfo
            }
        };

        vkUpdateDescriptorSets(
            m_device,
            static_cast<std::uint32_t>(copyWrites.size()),
            copyWrites.data(),
            0,
            nullptr
        );

        if (hiZBuffer.mipCount() <= 1)
        {
            return;
        }

        m_reduceDescriptorSets.resize(
            hiZBuffer.mipCount() - 1
        );

        std::vector<VkDescriptorSetLayout> layouts(
            hiZBuffer.mipCount() - 1,
            m_reduceDescriptorSetLayout
        );

        VkDescriptorSetAllocateInfo reduceAllocateInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = m_descriptorPool,
            .descriptorSetCount =
                static_cast<std::uint32_t>(layouts.size()),
            .pSetLayouts = layouts.data()
        };

        if (vkAllocateDescriptorSets(
                m_device,
                &reduceAllocateInfo,
                m_reduceDescriptorSets.data()
            ) != VK_SUCCESS)
        {
            throw std::runtime_error(
                "Failed to allocate Hi-Z reduction descriptor sets"
            );
        }

        for (
            std::uint32_t destinationMip = 1;
            destinationMip < hiZBuffer.mipCount();
            ++destinationMip
        )
        {
            const std::uint32_t descriptorIndex =
                destinationMip - 1;

            VkDescriptorImageInfo sourceMipInfo{
                .sampler = hiZBuffer.sampler(),
                .imageView =
                    hiZBuffer.mipView(destinationMip - 1),
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL
            };

            VkDescriptorImageInfo destinationInfo{
                .sampler = VK_NULL_HANDLE,
                .imageView =
                    hiZBuffer.mipView(destinationMip),
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL
            };

            std::array<VkWriteDescriptorSet, 2> writes{
                VkWriteDescriptorSet{
                    .sType =
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet =
                        m_reduceDescriptorSets[descriptorIndex],
                    .dstBinding = 0,
                    .descriptorCount = 1,
                    .descriptorType =
                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .pImageInfo = &sourceMipInfo
                },
                VkWriteDescriptorSet{
                    .sType =
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet =
                        m_reduceDescriptorSets[descriptorIndex],
                    .dstBinding = 1,
                    .descriptorCount = 1,
                    .descriptorType =
                        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .pImageInfo = &destinationInfo
                }
            };

            vkUpdateDescriptorSets(
                m_device,
                static_cast<std::uint32_t>(writes.size()),
                writes.data(),
                0,
                nullptr
            );
        }
    }

    void HiZPass::updateDepthImage(
        VkImageView depthImageView,
        VkSampler depthSampler
    ) const {
        VkDescriptorImageInfo depthInfo{
            .sampler = depthSampler,
            .imageView = depthImageView,
            .imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };

        VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = m_copyDescriptorSet,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &depthInfo
        };

        vkUpdateDescriptorSets(
            m_device,
            1,
            &write,
            0,
            nullptr
        );
    }

    void HiZPass::record(
        VkCommandBuffer commandBuffer,
        const HiZBuffer& hiZBuffer,
        const bool hasPreviousContents
    ) const
    {
        VkImageMemoryBarrier2 prepareBarrier{
            .sType =
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask =
                VK_PIPELINE_STAGE_2_NONE,
            .srcAccessMask = 0,
            .dstStageMask =
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask =
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout = hasPreviousContents
                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .image = hiZBuffer.image(),
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = hiZBuffer.mipCount(),
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };

        VkDependencyInfo prepareDependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &prepareBarrier
        };

        vkCmdPipelineBarrier2(
            commandBuffer,
            &prepareDependency
        );

        vkCmdBindPipeline(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            m_copyPipeline
        );

        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            m_copyPipelineLayout,
            0,
            1,
            &m_copyDescriptorSet,
            0,
            nullptr
        );

        vkCmdDispatch(
            commandBuffer,
            divideRoundUp(hiZBuffer.width(), 8),
            divideRoundUp(hiZBuffer.height(), 8),
            1
        );

        for (
            std::uint32_t destinationMip = 1;
            destinationMip < hiZBuffer.mipCount();
            ++destinationMip
        )
        {
            VkImageMemoryBarrier2 previousMipBarrier{
                .sType =
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask =
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .srcAccessMask =
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                .dstStageMask =
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .dstAccessMask =
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .image = hiZBuffer.image(),
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = destinationMip - 1,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1
                }
            };

            VkDependencyInfo dependency{
                .sType =
                    VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers =
                    &previousMipBarrier
            };

            vkCmdPipelineBarrier2(
                commandBuffer,
                &dependency
            );

            vkCmdBindPipeline(
                commandBuffer,
                VK_PIPELINE_BIND_POINT_COMPUTE,
                m_reducePipeline
            );

            VkDescriptorSet descriptorSet =
                m_reduceDescriptorSets[destinationMip - 1];

            vkCmdBindDescriptorSets(
                commandBuffer,
                VK_PIPELINE_BIND_POINT_COMPUTE,
                m_reducePipelineLayout,
                0,
                1,
                &descriptorSet,
                0,
                nullptr
            );

            const VkExtent2D extent =
                hiZBuffer.mipExtent(destinationMip);

            vkCmdDispatch(
                commandBuffer,
                divideRoundUp(extent.width, 8),
                divideRoundUp(extent.height, 8),
                1
            );
        }

        VkImageMemoryBarrier2 cullingBarrier{
            .sType =
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask =
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask =
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask =
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask =
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = hiZBuffer.image(),
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = hiZBuffer.mipCount(),
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };

        VkDependencyInfo cullingDependency{
            .sType =
                VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &cullingBarrier
        };

        vkCmdPipelineBarrier2(
            commandBuffer,
            &cullingDependency
        );
    }
}
