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
        if (options.colorFormat == VK_FORMAT_UNDEFINED && options.depthFormat == VK_FORMAT_UNDEFINED) {
            throw std::invalid_argument("Graphics pipeline requires a color or depth attachment format");
        }

        destroy();
        device_ = device;
        try {
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
        }
        pipeline_ = VK_NULL_HANDLE;
        layout_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
    }

    namespace {
    // Kept temporarily as source history while the surrounding migration is
    // reviewed; it is deliberately excluded from the build and has no API.
    [[maybe_unused]] void legacyRenderPassImplementation() {
#if 0
        const bool usesMsaa = options.samples != VK_SAMPLE_COUNT_1_BIT;
        const bool usesDepth = options.depthFormat != VK_FORMAT_UNDEFINED;
        const bool usesAdditionalColor = options.additionalColorFormat != VK_FORMAT_UNDEFINED;
        const bool usesThirdColor = options.thirdColorFormat != VK_FORMAT_UNDEFINED;
        if ((usesAdditionalColor || usesThirdColor) && usesMsaa) {
            throw std::invalid_argument("MRT forward attachments are only supported for single-sample rendering");
        }
        const bool resolvesDepth = usesMsaa && usesDepth &&
                                   options.depthResolveFormat != VK_FORMAT_UNDEFINED &&
                                   options.depthResolveMode != VK_RESOLVE_MODE_NONE;

        const VkAttachmentDescription2 color{
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
            .flags = 0,
            .format = options.colorFormat,
            .samples = options.samples,
            .loadOp = options.colorLoadOp,
            // A later compositing pass (water) may LOAD the MSAA attachment.
            // DONT_CARE would make that perfectly valid use undefined.
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = options.colorInitialLayout,
            .finalLayout = usesMsaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : options.colorFinalLayout,
        };

        // The completed depth buffer is sampled by the following frame's Hi-Z pass.
        const VkAttachmentDescription2 depth{
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
            .flags = 0,
            .format = options.depthFormat,
            .samples = options.samples,
            .loadOp = options.depthLoadOp,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = options.depthInitialLayout,
            .finalLayout = options.depthFinalLayout,
        };

        VkAttachmentReference2 colorRef{
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
            .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
        };
        VkAttachmentReference2 additionalColorRef{
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
            .attachment = 1, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
        };
        VkAttachmentReference2 thirdColorRef{
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
            .attachment = 2, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
        };
        std::array colorRefs{colorRef, additionalColorRef, thirdColorRef};
        const uint32_t colorAttachmentCount = 1U + (usesAdditionalColor ? 1U : 0U) + (usesThirdColor ? 1U : 0U);
        VkAttachmentReference2 depthRef{
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, .attachment = colorAttachmentCount,
            .layout = options.depthWriteEnable
                          ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                          : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT
        };
        VkAttachmentReference2 resolveRef{
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
            .attachment = usesDepth ? 2U : 1U,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        };
        VkAttachmentReference2 depthResolveRef{
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
            .attachment = 3U,
            .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
        };
        VkSubpassDescriptionDepthStencilResolve depthResolve{
            .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE,
            .depthResolveMode = options.depthResolveMode,
            .stencilResolveMode = VK_RESOLVE_MODE_NONE,
            .pDepthStencilResolveAttachment = &depthResolveRef,
        };

        VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = colorAttachmentCount;
        subpass.pColorAttachments = colorRefs.data();
        subpass.pResolveAttachments = usesMsaa ? &resolveRef : nullptr;
        subpass.pDepthStencilAttachment = usesDepth ? &depthRef : nullptr;
        subpass.pNext = resolvesDepth ? &depthResolve : nullptr;


        VkSubpassDependency2 dependency{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT) |
                                  (usesDepth
                                       ? static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT)
                                       : VkPipelineStageFlags{0});
        dependency.dstStageMask = dependency.srcStageMask;
        dependency.srcAccessMask =
                options.colorInitialLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
                options.colorInitialLayoutExternallySynchronized
                    ? 0
                    : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        if (usesDepth && options.depthInitialLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
            dependency.srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        }
        dependency.dstAccessMask = static_cast<VkAccessFlags>(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT) |
                                   static_cast<VkAccessFlags>(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) |
                                   (usesDepth
                                        ? static_cast<VkAccessFlags>(options.depthWriteEnable
                                                                         ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                                                         : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT)
                                        : VkAccessFlags{0});

        std::vector<VkAttachmentDescription2> attachments{color};
        if (usesAdditionalColor) {
            attachments.push_back(VkAttachmentDescription2{
                .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
                .format = options.additionalColorFormat,
                .samples = options.samples,
                .loadOp = options.additionalColorLoadOp,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .initialLayout = options.colorInitialLayout,
                .finalLayout = options.colorFinalLayout,
            });
        }
        if (usesThirdColor) {
            attachments.push_back(VkAttachmentDescription2{
                .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
                .format = options.thirdColorFormat,
                .samples = options.samples,
                .loadOp = options.thirdColorLoadOp,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .initialLayout = options.colorInitialLayout,
                .finalLayout = options.colorFinalLayout,
            });
        }
        if (usesDepth) {
            attachments.push_back(depth);
        }
        if (usesMsaa) {
            const VkAttachmentDescription2 resolve{
                .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
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
        if (resolvesDepth) {
            const VkAttachmentDescription2 depthResolveAttachment{
                .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
                .flags = 0,
                .format = options.depthResolveFormat,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            };
            attachments.push_back(depthResolveAttachment);
        }
        VkSubpassDependency2 sampledDependency{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};
        sampledDependency.srcSubpass = 0;
        sampledDependency.dstSubpass = VK_SUBPASS_EXTERNAL;
        // A following fullscreen pass can sample either the color result or
        // the depth written by this pass (GTAO samples the depth prepass).
        sampledDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        sampledDependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        sampledDependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        sampledDependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        std::array dependencies{dependency, sampledDependency};
        const bool sampledAfterPass =
                options.colorFinalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkRenderPassCreateInfo2 info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = sampledAfterPass ? 2U : 1U;
        info.pDependencies = dependencies.data();
        const VkResult result = vkCreateRenderPass2(device_, &info, nullptr, &renderPass_);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Could not create render pass");
        }
 #endif
    }
    } // namespace

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
        const auto shader = (options.assetManager != nullptr)
                                ? Vkutil::loadShaderModule(device_, *options.assetManager, options.shader)
                                : Vkutil::loadShaderModule(device_, options.shader);
        const std::array stages{
            VkPipelineShaderStageCreateInfo{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                shader.get(), "vertexMain",
            },
            VkPipelineShaderStageCreateInfo{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                shader.get(), "fragmentMain",
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
        // The renderer does not enable independentBlend. Keep every MRT blend
        // state identical; channel masks are not needed for float velocity or
        // oct-normal attachments.
        VkPipelineColorBlendAttachmentState additionalColorAttachment = colorAttachment;
        VkPipelineColorBlendAttachmentState thirdColorAttachment = colorAttachment;
        std::array colorAttachments{colorAttachment, additionalColorAttachment, thirdColorAttachment};
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = options.colorFormat == VK_FORMAT_UNDEFINED ? 0U :
            1U + (options.additionalColorFormat != VK_FORMAT_UNDEFINED ? 1U : 0U) +
            (options.thirdColorFormat != VK_FORMAT_UNDEFINED ? 1U : 0U);
        blend.pAttachments = blend.attachmentCount == 0 ? nullptr : colorAttachments.data();
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
        std::array colorFormats{options.colorFormat, options.additionalColorFormat,
                                options.thirdColorFormat};
        const uint32_t colorAttachmentCount = options.colorFormat == VK_FORMAT_UNDEFINED ? 0U :
            1U + (options.additionalColorFormat != VK_FORMAT_UNDEFINED ? 1U : 0U) +
            (options.thirdColorFormat != VK_FORMAT_UNDEFINED ? 1U : 0U);
        VkPipelineRenderingCreateInfo rendering{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = colorAttachmentCount,
            .pColorAttachmentFormats = colorFormats.data(),
            .depthAttachmentFormat = options.depthFormat,
            .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
        };
        // GraphicsPipeline is intentionally render-pass agnostic. Attachment
        // compatibility belongs to the dynamic-rendering declaration, not to
        // a persistent VkRenderPass object.
        info.pNext = &rendering;
        info.layout = layout_;
        info.renderPass = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create graphics pipeline");
        }
    }
} // namespace Engine
