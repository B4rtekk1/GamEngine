#include "Engine/Renderer/Passes/ForwardPass.h"

#include "Engine/Renderer/Culling/IndexedIndirectDrawCount.h"
#include "Engine/Renderer/Geometry/Vertex.h"
#include "Engine/Renderer/Vulkan/renderer_types.h"
#include <algorithm>
#include <cstddef>

namespace Engine {

void ForwardPass::create(VkDevice device, const VkFormat colorFormat,
                         const VkFormat depthFormat,
                         const VkSampleCountFlagBits samples,
                         const VkFormat depthResolveFormat,
                         const VkResolveModeFlagBits depthResolveMode,
                         VkDescriptorSetLayout sceneLayout,
                         Assets::AssetManager& assets) {
    GraphicsPipelineOptions options{};
    options.colorFormat = colorFormat;
    options.depthFormat = depthFormat;
    options.samples = samples;
    options.depthResolveFormat = depthResolveFormat;
    options.depthResolveMode = depthResolveMode;
    options.colorFinalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    options.shader = "shaders/forward_pbr.spv";
    options.assetManager = &assets;
    options.cullMode = VK_CULL_MODE_BACK_BIT;
    options.alphaBlendEnable = VK_FALSE;
    options.descriptorSetLayouts = {sceneLayout};
    options.vertexBindings = {
        {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
    };
    options.vertexAttributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, texCoord)},
        {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {8, 0, VK_FORMAT_R32_UINT, offsetof(Vertex, materialIndex)},
        {9, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent)},
    };
    pipeline_.create(device, options);

    GraphicsPipelineOptions foliageOptions = options;
    foliageOptions.existingRenderPass = pipeline_.renderPass();
    foliageOptions.cullMode = VK_CULL_MODE_NONE;
    // This pass is also the transparent stream.  MASK fragments write alpha
    // as one in the shader; BLEND retains alpha and uses standard compositing.
    foliageOptions.alphaBlendEnable = VK_TRUE;
    foliageOptions.depthWriteEnable = VK_FALSE;
    foliagePipeline_.create(device, foliageOptions);
    GraphicsPipelineOptions grassOptions = foliageOptions;
    grassOptions.shader = "shaders/grass_forward.spv";
    // grass_forward.slang does not consume tangents. Keeping the generic
    // mesh tangent attribute here triggers Vulkan validation at location 9.
    grassOptions.vertexAttributes.erase(
        std::remove_if(grassOptions.vertexAttributes.begin(),
                       grassOptions.vertexAttributes.end(),
                       [](const VkVertexInputAttributeDescription& attribute) {
                           return attribute.location == 9;
                       }),
        grassOptions.vertexAttributes.end());
    grassPipeline_.create(device, grassOptions);

    GraphicsPipelineOptions outlineOptions = options;
    outlineOptions.shader = "shaders/selection_outline.spv";
    outlineOptions.existingRenderPass = pipeline_.renderPass();
    outlineOptions.cullMode = VK_CULL_MODE_FRONT_BIT;
    outlineOptions.depthWriteEnable = VK_FALSE;
    outlineOptions.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    outlineOptions.vertexAttributes.erase(
        std::remove_if(outlineOptions.vertexAttributes.begin(),
                       outlineOptions.vertexAttributes.end(),
                       [](const VkVertexInputAttributeDescription& attribute) {
                           // The outline only needs position (0), normal (3),
                           // and the instance model columns (4-7).
                           return attribute.location == 1 ||
                                  attribute.location == 2 ||
                                  attribute.location == 8 ||
                                  attribute.location >= 9;
                       }),
        outlineOptions.vertexAttributes.end());
    outlinePipeline_.create(device, outlineOptions);
}

void ForwardPass::destroy() noexcept {
    outlinePipeline_.destroy();
    foliagePipeline_.destroy();
    grassPipeline_.destroy();
    pipeline_.destroy();
}

void ForwardPass::drawGrass(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                            const Culling::IndexedIndirectDrawCount& indirectDraw) const {
    if (!indirectDraw.valid()) return;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, grassPipeline_.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, grassPipeline_.layout(),
                            0, 1, &descriptorSet, 0, nullptr);
    indirectDraw.record(commandBuffer);
}

void ForwardPass::begin(VkCommandBuffer commandBuffer,
                        VkFramebuffer framebuffer, const VkExtent2D extent,
                        VkDescriptorSet sceneDescriptorSet,
                        VkBuffer vertexBuffer, VkBuffer instanceBuffer,
                        VkBuffer indexBuffer) const {
    (void)instanceBuffer; // Instance data is fetched from descriptor binding 5.
    VkRenderPassBeginInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    passInfo.renderPass = pipeline_.renderPass();
    passInfo.framebuffer = framebuffer;
    passInfo.renderArea.extent = extent;
    VkClearValue clearValues[2]{};
    clearValues[0].color = {{0.02F, 0.02F, 0.05F, 1.0F}};
    clearValues[1].depthStencil = {1.0F, 0};
    passInfo.clearValueCount = std::size(clearValues);
    passInfo.pClearValues = clearValues;
    vkCmdBeginRenderPass(commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_.layout(), 0, 1, &sceneDescriptorSet, 0, nullptr);
    constexpr VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, offsets);
    vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    const VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent.width),
                              static_cast<float>(extent.height), 0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

void ForwardPass::draw(VkCommandBuffer commandBuffer,
                       const Culling::IndexedIndirectDrawCount& indirectDraw) {
    if (indirectDraw.valid()) {
        indirectDraw.record(commandBuffer);
    }
}

void ForwardPass::drawFoliage(
    VkCommandBuffer commandBuffer, const VkDescriptorSet sceneDescriptorSet,
    const Culling::IndexedIndirectDrawCount& indirectDraw) const {
    if (!indirectDraw.valid()) return;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, foliagePipeline_.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            foliagePipeline_.layout(), 0, 1, &sceneDescriptorSet, 0, nullptr);
    indirectDraw.record(commandBuffer);
}

void ForwardPass::end(VkCommandBuffer commandBuffer) {
    vkCmdEndRenderPass(commandBuffer);
}

void ForwardPass::drawOutline(
    VkCommandBuffer commandBuffer,
    const VkDescriptorSet sceneDescriptorSet,
    const Culling::IndexedIndirectDrawCount& indirectDraw) const {
    if (!indirectDraw.valid()) return;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      outlinePipeline_.handle());
    // Sky and particle draws bind their own descriptor sets. Bind the scene
    // set again because the outline pipeline uses the forward-pass layout.
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            outlinePipeline_.layout(), 0, 1,
                            &sceneDescriptorSet, 0, nullptr);
    indirectDraw.record(commandBuffer);
}

} // namespace Engine
