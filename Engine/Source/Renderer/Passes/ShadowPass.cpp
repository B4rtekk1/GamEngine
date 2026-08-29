#include "Engine/Renderer/Passes/ShadowPass.h"

#include "Engine/Math/Mat4.h"
#include "Engine/Renderer/Culling/GPUCullingPass.h"
#include "Engine/Renderer/Culling/IndexedIndirectDrawCount.h"
#include "Engine/Renderer/Geometry/Vertex.h"
#include "Engine/Renderer/Materials/MaterialBuffer.h"
#include "Engine/Renderer/shader_loader.h"
#include "Engine/Renderer/Vulkan/renderer_types.h"

#include <glm/glm.hpp>

#include <array>
#include <stdexcept>

namespace Engine {
namespace {
constexpr float DepthBiasConstant = 0.15F;
constexpr float DepthBiasSlope = 0.35F;
}

ShadowPass::~ShadowPass() {
    destroy();
}

void ShadowPass::create(VkPhysicalDevice physicalDevice, VkDevice device,
                        const std::vector<VkBuffer>& uniformBuffers,
                        const std::vector<VkBuffer>& materialBuffers,
                        const std::vector<VkDescriptorImageInfo>& materialTextures,
                        const VkDeviceSize uniformBufferRange,
                        Assets::AssetManager& assets) {
    destroy();
    device_ = device;

    try {
        shadowMap_.create(physicalDevice, device_);

        if (materialBuffers.size() != uniformBuffers.size() ||
            materialTextures.size() != MaxMaterialTextures) {
            throw std::invalid_argument("Invalid material descriptor resources");
        }

        VkDescriptorSetLayoutBinding bindings[4]{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MaxMaterialTextures,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        const VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
            4, bindings};
        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr,
                                        &descriptorSetLayout_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create shadow descriptor-set layout");
        }

        const std::uint32_t frameCount = static_cast<std::uint32_t>(uniformBuffers.size());
        const VkDescriptorPoolSize poolSizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, frameCount},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frameCount},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frameCount},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, frameCount * MaxMaterialTextures},
        };
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = frameCount;
        poolInfo.poolSizeCount = std::size(poolSizes);
        poolInfo.pPoolSizes = poolSizes;
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create shadow descriptor pool");
        }

        std::vector<VkDescriptorSetLayout> layouts(frameCount, descriptorSetLayout_);
        descriptorSets_.resize(frameCount);
        VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocateInfo.descriptorPool = descriptorPool_;
        allocateInfo.descriptorSetCount = frameCount;
        allocateInfo.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(device_, &allocateInfo, descriptorSets_.data()) != VK_SUCCESS) {
            throw std::runtime_error("Could not allocate shadow descriptor sets");
        }

        const VkDescriptorImageInfo imageInfo{
            shadowMap_.sampler(), shadowMap_.imageView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
            const VkDescriptorBufferInfo bufferInfo{
                uniformBuffers[frame], 0, uniformBufferRange};
            const VkDescriptorBufferInfo materialInfo{
                materialBuffers[frame], 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet writes[4]{};
            writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                         descriptorSets_[frame], 0, 0, 1,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo, nullptr, nullptr};
            writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                         descriptorSets_[frame], 1, 0, 1,
                         VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bufferInfo, nullptr};
            writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                         descriptorSets_[frame], 2, 0, 1,
                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &materialInfo, nullptr};
            writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                         descriptorSets_[frame], 3, 0, MaxMaterialTextures,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, materialTextures.data(), nullptr};
            vkUpdateDescriptorSets(device_, std::size(writes), writes, 0, nullptr);
        }

        const auto shader = Vkutil::loadShaderModule(device_, assets, "shaders/shadow_map.spv");
        const std::array stages{
            VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_VERTEX_BIT, shader.get(), "vertexMain", nullptr},
            VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_FRAGMENT_BIT, shader.get(), "fragmentMain", nullptr},
        };
        const VkPushConstantRange pushConstantRange{
            VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout_;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
        if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr,
                                   &pipelineLayout_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create shadow pipeline layout");
        }

        const VkVertexInputBindingDescription vertexBindings[] = {
            {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
            {1, sizeof(RendererInstanceData), VK_VERTEX_INPUT_RATE_INSTANCE},
        };
        const VkVertexInputAttributeDescription attributes[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, texCoord)},
            {8, 0, VK_FORMAT_R32_UINT, offsetof(Vertex, materialIndex)},
            {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
            {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(glm::vec4)},
            {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(glm::vec4) * 2},
            {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(glm::vec4) * 3},
            {10, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RendererInstanceData, normalColumn0)},
            {11, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RendererInstanceData, normalColumn1)},
            {13, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RendererInstanceData, grassDeformation)},
        };
        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = std::size(vertexBindings);
        vertexInput.pVertexBindingDescriptions = vertexBindings;
        vertexInput.vertexAttributeDescriptionCount = std::size(attributes);
        vertexInput.pVertexAttributeDescriptions = attributes;
        VkPipelineInputAssemblyStateCreateInfo assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rasterizer{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        // The main pass renders glTF double-sided foliage. Its transparent
        // pixels are already discarded by shadow_map::fragmentMain, so it must also cast
        // a shadow when the light sees the back of a leaf card.
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rasterizer.lineWidth = 1.0F;
        rasterizer.depthBiasEnable = VK_TRUE;
        VkPipelineMultisampleStateCreateInfo multisampling{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;
        constexpr VkDynamicState dynamicStates[] = {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_DEPTH_BIAS};
        VkPipelineDynamicStateCreateInfo dynamic{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = std::size(dynamicStates);
        dynamic.pDynamicStates = dynamicStates;
        VkGraphicsPipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.stageCount = std::size(stages);
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &assembly;
        pipelineInfo.pViewportState = &viewport;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depth;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = pipelineLayout_;
        pipelineInfo.renderPass = shadowMap_.renderPass();
        if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo,
                                      nullptr, &pipeline_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create shadow pipeline");
        }
    } catch (...) {
        destroy();
        throw;
    }
}

void ShadowPass::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (pipelineLayout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (descriptorPool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        if (descriptorSetLayout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
    }
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    descriptorSetLayout_ = VK_NULL_HANDLE;
    descriptorSets_.clear();
    shadowMap_.destroy();
    atlasInitialized_ = false;
    atlasContentValid_ = false;
    device_ = VK_NULL_HANDLE;
}

VkDescriptorSet ShadowPass::descriptorSet(const std::uint32_t frameIndex) const {
    return descriptorSets_.at(frameIndex);
}

void ShadowPass::record(const VkCommandBuffer commandBuffer,
                        const std::array<Mat4, ShadowMap::ClipLevelCount>& clipMatrices,
                        std::uint32_t updateMask,
                        const VkBuffer vertexBuffer, const VkBuffer instanceBuffer,
                        const VkBuffer indexBuffer, const VkDescriptorSet sceneDescriptorSet,
                        const Culling::GPUCullingPass& cullingPass,
                        const Culling::IndexedIndirectDrawCount& indirectDraw,
                        const std::uint32_t objectCount) const {
    if (objectCount == 0) {
        updateMask = 0;
        atlasContentValid_ = false;
    } else if (!atlasContentValid_) {
        updateMask = (1u << ShadowMap::ClipLevelCount) - 1u;
    }
    if (objectCount != 0 && updateMask != 0) {
        cullingPass.record(commandBuffer, objectCount);
    }

    VkImageMemoryBarrier2 atlasBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    atlasBarrier.srcStageMask = atlasInitialized_ ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                                  : VK_PIPELINE_STAGE_2_NONE;
    atlasBarrier.srcAccessMask = atlasInitialized_ ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0;
    atlasBarrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    atlasBarrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    atlasBarrier.oldLayout = atlasInitialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                               : VK_IMAGE_LAYOUT_UNDEFINED;
    atlasBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    atlasBarrier.image = shadowMap_.image();
    atlasBarrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo atlasDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    atlasDependency.imageMemoryBarrierCount = 1;
    atlasDependency.pImageMemoryBarriers = &atlasBarrier;
    vkCmdPipelineBarrier2(commandBuffer, &atlasDependency);

    VkRenderPassBeginInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    passInfo.renderPass = shadowMap_.renderPass();
    passInfo.framebuffer = shadowMap_.framebuffer();
    passInfo.renderArea.extent = {ShadowMap::Resolution, ShadowMap::Resolution};
    vkCmdBeginRenderPass(commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &sceneDescriptorSet, 0, nullptr);
    const VkBuffer vertexBuffers[] = {vertexBuffer, instanceBuffer};
    constexpr VkDeviceSize offsets[] = {0, 0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdSetDepthBias(commandBuffer, DepthBiasConstant, 0.0F, DepthBiasSlope);
    for (std::uint32_t level = 0; level < ShadowMap::ClipLevelCount; ++level) {
        if ((updateMask & (1u << level)) == 0) continue;
        const std::int32_t x = static_cast<std::int32_t>((level & 1u) * ShadowMap::TileResolution);
        const std::int32_t y = static_cast<std::int32_t>(((level >> 1u) & 1u) * ShadowMap::TileResolution);
        const VkRect2D tile{{x, y}, {ShadowMap::TileResolution, ShadowMap::TileResolution}};
        const VkClearAttachment clear{VK_IMAGE_ASPECT_DEPTH_BIT, 0, {.depthStencil = {1.0F, 0}}};
        const VkClearRect clearRect{tile, 0, 1};
        vkCmdClearAttachments(commandBuffer, 1, &clear, 1, &clearRect);
        const VkViewport viewport{static_cast<float>(x), static_cast<float>(y),
            static_cast<float>(ShadowMap::TileResolution), static_cast<float>(ShadowMap::TileResolution),
            0.0F, 1.0F};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &tile);
        vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(Mat4), &clipMatrices[level]);
        if (objectCount != 0 && indirectDraw.valid()) indirectDraw.record(commandBuffer);
    }
    vkCmdEndRenderPass(commandBuffer);
    atlasInitialized_ = true;
    if (objectCount != 0) atlasContentValid_ = true;
}

} // namespace Engine
