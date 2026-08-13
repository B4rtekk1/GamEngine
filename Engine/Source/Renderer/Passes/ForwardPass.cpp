#include "Engine/Renderer/Passes/ForwardPass.h"

#include "Engine/Renderer/Culling/IndexedIndirectDrawCount.h"
#include "Engine/Renderer/Geometry/Vertex.h"
#include "Engine/Renderer/MeshRenderer.h"

#include <glm/glm.hpp>

#include <cstddef>

namespace Engine {
namespace {
struct MaterialPushConstants {
    glm::vec4 baseColorMetallic;
    glm::vec4 roughnessAo;
};

MaterialPushConstants materialConstants(const MeshRenderer& renderer) {
    return {
        glm::vec4{renderer.material.baseColor.native(), renderer.material.metallic},
        glm::vec4{renderer.material.roughness, renderer.material.ambientOcclusion,
                  0.0f, 0.0f},
    };
}
}

void ForwardPass::create(const VkDevice device, const VkFormat colorFormat,
                         const VkFormat depthFormat,
                         const VkSampleCountFlagBits samples,
                         const VkDescriptorSetLayout sceneLayout) {
    GraphicsPipelineOptions options{};
    options.colorFormat = colorFormat;
    options.depthFormat = depthFormat;
    options.samples = samples;
    options.vertexShader = "shaders/pbr.vert.spv";
    options.fragmentShader = "shaders/pbr.frag.spv";
    options.pushConstantSize = sizeof(MaterialPushConstants);
    options.pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    options.cullMode = VK_CULL_MODE_BACK_BIT;
    options.descriptorSetLayouts = {sceneLayout};
    options.vertexBindings = {
        {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(glm::mat4), VK_VERTEX_INPUT_RATE_INSTANCE},
    };
    options.vertexAttributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)},
        {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(glm::vec4)},
        {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(glm::vec4) * 2},
        {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(glm::vec4) * 3},
    };
    pipeline_.create(device, options);
}

void ForwardPass::destroy() noexcept {
    pipeline_.destroy();
}

void ForwardPass::begin(const VkCommandBuffer commandBuffer,
                        const VkFramebuffer framebuffer, const VkExtent2D extent,
                        const VkDescriptorSet sceneDescriptorSet,
                        const VkBuffer vertexBuffer, const VkBuffer instanceBuffer,
                        const VkBuffer indexBuffer) const {
    VkRenderPassBeginInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    passInfo.renderPass = pipeline_.renderPass();
    passInfo.framebuffer = framebuffer;
    passInfo.renderArea.extent = extent;
    VkClearValue clearValues[2]{};
    clearValues[0].color = {{0.02f, 0.02f, 0.05f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};
    passInfo.clearValueCount = std::size(clearValues);
    passInfo.pClearValues = clearValues;
    vkCmdBeginRenderPass(commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_.layout(), 0, 1, &sceneDescriptorSet, 0, nullptr);
    const VkBuffer vertexBuffers[] = {vertexBuffer, instanceBuffer};
    constexpr VkDeviceSize offsets[] = {0, 0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                              static_cast<float>(extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

void ForwardPass::draw(const VkCommandBuffer commandBuffer,
                       const MeshRenderer& plane, const MeshRenderer& cubes,
                       const Culling::IndexedIndirectDrawCount& indirectDraw) const {
    if (plane.hasMesh()) {
        const auto constants = materialConstants(plane);
        vkCmdPushConstants(commandBuffer, pipeline_.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(constants), &constants);
        vkCmdDrawIndexed(commandBuffer, plane.mesh->indexCount(), 1,
                         plane.firstIndex, 0, 0);
    }
    if (cubes.hasMesh()) {
        const auto constants = materialConstants(cubes);
        vkCmdPushConstants(commandBuffer, pipeline_.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(constants), &constants);
        indirectDraw.record(commandBuffer);
    }
}

void ForwardPass::end(const VkCommandBuffer commandBuffer) const {
    vkCmdEndRenderPass(commandBuffer);
}

} // namespace Engine
