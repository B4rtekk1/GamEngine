#include "Engine/Renderer/Passes/WaterPass.h"

#include "Engine/Renderer/Culling/IndexedIndirectDrawCount.h"
#include "Engine/Renderer/Geometry/GpuVertex.h"

#include <cstddef>
#include <stdexcept>

namespace Engine {
void WaterPass::create(const VkDevice device, const VkFormat colorFormat, const VkFormat depthFormat,
                       const VkSampleCountFlagBits samples, const VkFormat depthResolveFormat,
                       const VkResolveModeFlagBits depthResolveMode, const VkDescriptorSetLayout sceneLayout,
                       Assets::AssetManager& assets, const bool velocity,
                       const VkDescriptorImageInfo& opaqueColor, const VkDescriptorImageInfo& opaqueDepth) {
    if (opaqueColor.imageView == VK_NULL_HANDLE || opaqueDepth.imageView == VK_NULL_HANDLE) {
        throw std::invalid_argument("WaterPass requires valid opaque scene color/depth views");
    }
    device_ = device;
    hasVelocity_ = velocity;
    GraphicsPipelineOptions options{};
    options.colorFormat = colorFormat;
    options.additionalColorFormat = velocity ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_UNDEFINED;
    options.additionalColorLoadOp = velocity ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    options.depthFormat = depthFormat;
    options.samples = samples;
    options.depthResolveFormat = depthResolveFormat;
    options.depthResolveMode = depthResolveMode;
    // The opaque lighting pass has already produced both attachments.
    options.colorLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    options.depthLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    options.colorInitialLayout = samples == VK_SAMPLE_COUNT_1_BIT
        ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    options.colorFinalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    options.depthInitialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    options.depthFinalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    options.depthWriteEnable = VK_FALSE;
    options.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    options.shader = velocity ? "shaders/forward_water.spv" : "shaders/forward_water_no_velocity.spv";
    options.assetManager = &assets;
    options.cullMode = VK_CULL_MODE_NONE;
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    const VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        nullptr, 0, static_cast<std::uint32_t>(std::size(bindings)), bindings};
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &sceneTextureLayout_) != VK_SUCCESS)
        throw std::runtime_error("Could not create water scene descriptor layout");
    options.descriptorSetLayouts = {sceneLayout, sceneTextureLayout_};
    options.vertexBindings = {{0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    options.vertexAttributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, px)},
        {1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(GpuVertex, color)},
        {2, 0, VK_FORMAT_R16G16_SFLOAT, offsetof(GpuVertex, texCoord)},
        // forward_water's vertex entry point consumes the complete GpuVertex
        // layout.  Every declared input needs an attribute on devices without
        // vertex attribute robustness/maintenance9, even when a variant does
        // not visibly use a value in its final fragment output.
        {4, 0, VK_FORMAT_R16G16_SFLOAT, offsetof(GpuVertex, texCoord1)},
        {3, 0, VK_FORMAT_A2B10G10R10_SNORM_PACK32, offsetof(GpuVertex, normal)},
        {8, 0, VK_FORMAT_R32_UINT, offsetof(GpuVertex, materialIndex)},
        {9, 0, VK_FORMAT_A2B10G10R10_SNORM_PACK32, offsetof(GpuVertex, tangent)},
    };
    pipeline_.create(device, options);
    createSceneDescriptors(opaqueColor, opaqueDepth);
}

void WaterPass::createSceneDescriptors(const VkDescriptorImageInfo& opaqueColor,
                                       const VkDescriptorImageInfo& opaqueDepth) {
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FramesInFlight * 2U};
    const VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0,
        FramesInFlight, 1, &size};
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS)
        throw std::runtime_error("Could not create water scene descriptor pool");
    std::array<VkDescriptorSetLayout, FramesInFlight> layouts{}; layouts.fill(sceneTextureLayout_);
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptorPool_; allocate.descriptorSetCount = FramesInFlight;
    allocate.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device_, &allocate, sceneTextureSets_.data()) != VK_SUCCESS)
        throw std::runtime_error("Could not allocate water scene descriptor sets");
    for (const VkDescriptorSet set : sceneTextureSets_) {
        const VkWriteDescriptorSet writes[] = {
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &opaqueColor, nullptr, nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &opaqueDepth, nullptr, nullptr},
        };
        vkUpdateDescriptorSets(device_, std::size(writes), writes, 0, nullptr);
    }
}

void WaterPass::destroy() noexcept {
    pipeline_.destroy();
    if (device_ != VK_NULL_HANDLE) {
        if (descriptorPool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        if (sceneTextureLayout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, sceneTextureLayout_, nullptr);
    }
    device_ = VK_NULL_HANDLE; descriptorPool_ = VK_NULL_HANDLE; sceneTextureLayout_ = VK_NULL_HANDLE;
    sceneTextureSets_.fill(VK_NULL_HANDLE); hasVelocity_ = false;
}

void WaterPass::begin(const VkCommandBuffer commandBuffer, const VkFramebuffer framebuffer,
                      const VkExtent2D extent, const VkDescriptorSet descriptorSet,
                      const std::uint32_t frameIndex, const VkBuffer vertexBuffer, const VkBuffer indexBuffer) const {
    VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    info.renderPass = pipeline_.renderPass(); info.framebuffer = framebuffer; info.renderArea.extent = extent;
    vkCmdBeginRenderPass(commandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    const std::array sets{descriptorSet, sceneTextureSets_.at(frameIndex % FramesInFlight)};
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(), 0,
                            static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
    constexpr VkDeviceSize offset{};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &offset);
    vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    const VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport); vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

void WaterPass::draw(const VkCommandBuffer commandBuffer, const VkDescriptorSet descriptorSet,
                     const Culling::IndexedIndirectDrawCount& indirectDraw,
                     const VkDeviceSize commandOffset, const VkDeviceSize countOffset) const {
    if (!indirectDraw.valid()) return;
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(), 0, 1, &descriptorSet, 0, nullptr);
    indirectDraw.record(commandBuffer, commandOffset, countOffset);
}
void WaterPass::end(const VkCommandBuffer commandBuffer) { vkCmdEndRenderPass(commandBuffer); }
} // namespace Engine
