#include "Engine/Renderer/Passes/WaterPass.h"

#include "Engine/Renderer/Culling/IndexedIndirectDrawCount.h"
#include "Engine/Renderer/Geometry/GpuVertex.h"

#include <cstddef>

namespace Engine {
void WaterPass::create(const VkDevice device, const VkFormat colorFormat, const VkFormat depthFormat,
                       const VkSampleCountFlagBits samples, const VkFormat depthResolveFormat,
                       const VkResolveModeFlagBits depthResolveMode, const VkDescriptorSetLayout sceneLayout,
                       Assets::AssetManager& assets, const bool velocity) {
    hasVelocity_ = velocity;
    GraphicsPipelineOptions options{};
    options.colorFormat = colorFormat;
    options.additionalColorFormat = velocity ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_UNDEFINED;
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
    options.descriptorSetLayouts = {sceneLayout};
    options.vertexBindings = {{0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    options.vertexAttributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, px)},
        {2, 0, VK_FORMAT_R16G16_SFLOAT, offsetof(GpuVertex, texCoord)},
        {8, 0, VK_FORMAT_R32_UINT, offsetof(GpuVertex, materialIndex)},
    };
    pipeline_.create(device, options);
}

void WaterPass::destroy() noexcept { pipeline_.destroy(); hasVelocity_ = false; }

void WaterPass::begin(const VkCommandBuffer commandBuffer, const VkFramebuffer framebuffer,
                      const VkExtent2D extent, const VkDescriptorSet descriptorSet,
                      const VkBuffer vertexBuffer, const VkBuffer indexBuffer) const {
    VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    info.renderPass = pipeline_.renderPass(); info.framebuffer = framebuffer; info.renderArea.extent = extent;
    vkCmdBeginRenderPass(commandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(), 0, 1, &descriptorSet, 0, nullptr);
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
