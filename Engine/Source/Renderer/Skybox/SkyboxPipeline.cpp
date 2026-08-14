#include "Engine/Renderer/Skybox/Skyboxpipeline.h"

#include "Engine/Renderer/shader_loader.h"

#include <array>
#include <stdexcept>

namespace Engine {
SkyboxPipeline::~SkyboxPipeline() { destroy(); }

void SkyboxPipeline::create(VkDevice device, VkRenderPass renderPass, VkFormat, VkSampleCountFlagBits samples,
                            VkDescriptorSetLayout descriptorSetLayout,
                            Assets::AssetManager& assets) {
    destroy(); device_ = device;
    try {
        const auto vert = vkutil::loadShaderModule(device_, assets, "shaders/skybox.vert.spv");
        const auto frag = vkutil::loadShaderModule(device_, assets, "shaders/skybox.frag.spv");
        const std::array stages{
            VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert.get(), "main"},
            VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag.get(), "main"},
        };
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount = 1; layout.pSetLayouts = &descriptorSetLayout;
        if (vkCreatePipelineLayout(device_, &layout, nullptr, &layout_) != VK_SUCCESS) throw std::runtime_error("Could not create skybox pipeline layout");
        VkVertexInputBindingDescription binding{0, sizeof(float) * 3, VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; vertexInput.vertexBindingDescriptionCount = 1; vertexInput.pVertexBindingDescriptions = &binding; vertexInput.vertexAttributeDescriptionCount = 1; vertexInput.pVertexAttributeDescriptions = &attribute;
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = 1; viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rasterizer.polygonMode = VK_POLYGON_MODE_FILL; rasterizer.lineWidth = 1.0f; rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT; rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        VkPipelineMultisampleStateCreateInfo multisampling{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; multisampling.rasterizationSamples = samples;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; depth.depthTestEnable = VK_TRUE; depth.depthWriteEnable = VK_FALSE; depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState color{}; color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &color;
        const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = dynamicStates.size(); dynamic.pDynamicStates = dynamicStates.data();
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; info.stageCount = stages.size(); info.pStages = stages.data(); info.pVertexInputState = &vertexInput; info.pInputAssemblyState = &assembly; info.pViewportState = &viewport; info.pRasterizationState = &rasterizer; info.pMultisampleState = &multisampling; info.pDepthStencilState = &depth; info.pColorBlendState = &blend; info.pDynamicState = &dynamic; info.layout = layout_; info.renderPass = renderPass;
        if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_) != VK_SUCCESS) throw std::runtime_error("Could not create skybox pipeline");
    } catch (...) { destroy(); throw; }
}
void SkyboxPipeline::destroy() noexcept { if (device_ != VK_NULL_HANDLE) { if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr); if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr); } pipeline_ = VK_NULL_HANDLE; layout_ = VK_NULL_HANDLE; device_ = VK_NULL_HANDLE; }
} // namespace Engine
