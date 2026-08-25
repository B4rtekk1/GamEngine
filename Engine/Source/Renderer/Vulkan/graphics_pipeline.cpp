#include "Engine/Renderer/Vulkan/graphics_pipeline.h"

#include "Engine/Renderer/shader_loader.h"

#include <array>
#include <stdexcept>
#include <vector>

namespace Engine {
    GraphicsPipeline::~GraphicsPipeline() {
        destroy();
    }

    void GraphicsPipeline::create(VkDevice device, const GraphicsPipelineOptions &options) {
        if (device == VK_NULL_HANDLE) {
            throw std::invalid_argument("Could not create graphics pipeline for a null VkDevice");
        }
        if (options.colorFormat == VK_FORMAT_UNDEFINED) {
            throw std::invalid_argument("Graphics pipeline requires a color format");
        }

        destroy();
        device_ = device;
        try {
            if (options.existingRenderPass != VK_NULL_HANDLE) {
                renderPass_ = options.existingRenderPass;
                ownsRenderPass_ = false;
            } else {
                createRenderPass(options);
                ownsRenderPass_ = true;
            }
            createPipelineLayout(options);
            createGraphicsPipeline(options);
        } catch (...) {
            destroy();
            throw;
        }
    }

    void GraphicsPipeline::destroy() noexcept {
        if (device_ != VK_NULL_HANDLE) {
            if (pipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, pipeline_, nullptr);
            }
            if (layout_ != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device_, layout_, nullptr);
            }
            if (ownsRenderPass_ && renderPass_ != VK_NULL_HANDLE) {
                vkDestroyRenderPass(device_, renderPass_, nullptr);
            }
        }
        pipeline_ = VK_NULL_HANDLE;
        layout_ = VK_NULL_HANDLE;
        renderPass_ = VK_NULL_HANDLE;
        ownsRenderPass_ = false;
        device_ = VK_NULL_HANDLE;
    }

    void GraphicsPipeline::createRenderPass(const GraphicsPipelineOptions &options) {
        const bool usesMsaa = options.samples != VK_SAMPLE_COUNT_1_BIT;
        const bool usesDepth = options.depthFormat != VK_FORMAT_UNDEFINED;

        const VkAttachmentDescription color{
            .flags = 0,
            .format = options.colorFormat,
            .samples = options.samples,
            .loadOp = options.colorLoadOp,
            .storeOp = usesMsaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = options.colorInitialLayout,
            .finalLayout = usesMsaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : options.colorFinalLayout,
        };

        // The completed depth buffer is sampled by the following frame's Hi-Z pass.
        const VkAttachmentDescription depth{
            .flags = 0,
            .format = options.depthFormat,
            .samples = options.samples,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        };

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkAttachmentReference resolveRef{
            usesDepth ? 2U : 1U,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pResolveAttachments = usesMsaa ? &resolveRef : nullptr;
        subpass.pDepthStencilAttachment = usesDepth ? &depthRef : nullptr;


        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT) |
                                  (usesDepth
                                       ? static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT)
                                       : VkPipelineStageFlags{0});
        dependency.dstStageMask = dependency.srcStageMask;
        dependency.srcAccessMask =
                options.colorInitialLayout == VK_IMAGE_LAYOUT_UNDEFINED
                    ? 0
                    : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = static_cast<VkAccessFlags>(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT) |
                                   static_cast<VkAccessFlags>(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) |
                                   (usesDepth
                                        ? static_cast<VkAccessFlags>(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
                                        : VkAccessFlags{0});

        std::vector<VkAttachmentDescription> attachments{color};
        if (usesDepth) {
            attachments.push_back(depth);
        }
        if (usesMsaa) {
            const VkAttachmentDescription resolve{
                .flags = 0,
                .format = options.colorFormat,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .finalLayout = options.colorFinalLayout,
            };
            attachments.push_back(resolve);
        }
        VkSubpassDependency sampledDependency{};
        sampledDependency.srcSubpass = 0;
        sampledDependency.dstSubpass = VK_SUBPASS_EXTERNAL;
        sampledDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        sampledDependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        sampledDependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        sampledDependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        std::array dependencies{dependency, sampledDependency};
        const bool sampledAfterPass =
                options.colorFinalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = sampledAfterPass ? 2U : 1U;
        info.pDependencies = dependencies.data();
        const VkResult result = vkCreateRenderPass(device_, &info, nullptr, &renderPass_);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Could not create render pass");
        }
    }

    void GraphicsPipeline::createPipelineLayout(const GraphicsPipelineOptions &options) {
        VkPushConstantRange range{};
        range.stageFlags = options.pushConstantStages;
        range.size = options.pushConstantSize;

        VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        info.setLayoutCount = static_cast<uint32_t>(options.descriptorSetLayouts.size());
        info.pSetLayouts = options.descriptorSetLayouts.data();
        if (range.size != 0) {
            info.pushConstantRangeCount = 1;
            info.pPushConstantRanges = &range;
        }
        if (vkCreatePipelineLayout(device_, &info, nullptr, &layout_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create pipeline layout");
        }
    }

    void GraphicsPipeline::createGraphicsPipeline(const GraphicsPipelineOptions &options) {
        const auto vertexShader = (options.assetManager != nullptr)
                                      ? vkutil::loadShaderModule(device_, *options.assetManager, options.vertexShader)
                                      : vkutil::loadShaderModule(device_, options.vertexShader);
        const auto fragmentShader = (options.assetManager != nullptr)
                                        ? vkutil::loadShaderModule(device_, *options.assetManager,
                                                                   options.fragmentShader)
                                        : vkutil::loadShaderModule(device_, options.fragmentShader);
        const std::array stages{
            VkPipelineShaderStageCreateInfo{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                vertexShader.get(), "main",
            },
            VkPipelineShaderStageCreateInfo{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                fragmentShader.get(), "main",
            },
        };

        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(options.vertexBindings.size());
        vertexInput.pVertexBindingDescriptions = options.vertexBindings.data();
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(options.vertexAttributes.size());
        vertexInput.pVertexAttributeDescriptions = options.vertexAttributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0F;
        rasterizer.cullMode = options.cullMode;
        rasterizer.frontFace = options.frontFace;
        const VkPipelineMultisampleStateCreateInfo multisampling{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .rasterizationSamples = options.samples,
            .sampleShadingEnable = VK_FALSE,
            .minSampleShading = 0.0F,
            .pSampleMask = nullptr,
            .alphaToCoverageEnable = VK_FALSE,
            .alphaToOneEnable = VK_FALSE,
        };
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = options.depthTestEnable;
        depth.depthWriteEnable = options.depthWriteEnable;
        depth.depthCompareOp = options.depthCompareOp;
        VkPipelineColorBlendAttachmentState colorAttachment{};
        colorAttachment.colorWriteMask = options.colorWriteMask;
        colorAttachment.blendEnable = options.alphaBlendEnable;
        colorAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &colorAttachment;
        constexpr std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();

        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.stageCount = static_cast<uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &rasterizer;
        info.pMultisampleState = &multisampling;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = layout_;
        info.renderPass = renderPass_;
        if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create graphics pipeline");
        }
    }
} // namespace Engine
