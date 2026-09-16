#pragma once

/**
 * @file graphics_pipeline.h
 * @brief Declares configurable Vulkan graphics-pipeline creation helpers.
 */

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Engine {
    namespace Assets {
        class AssetManager;
    }

    /**
     * @brief Parameters used to create a GraphicsPipeline.
     */
    struct GraphicsPipelineOptions {
        /// Color attachment format used by the render pass.
        VkFormat colorFormat = VK_FORMAT_UNDEFINED;
        /// Optional second color attachment used by MRT pipelines.
        VkFormat additionalColorFormat = VK_FORMAT_UNDEFINED;
        /// Optional third color attachment (used by the depth prepass view normals).
        VkFormat thirdColorFormat = VK_FORMAT_UNDEFINED;
        /// Load operation for the optional MRT attachment.
        VkAttachmentLoadOp additionalColorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkAttachmentLoadOp thirdColorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        /// Pipelines are always created for Vulkan 1.3 dynamic rendering.
        /// Attachment compatibility is declared from the formats below.
        /// Retained temporarily so existing pass setup remains source-compatible;
        /// GraphicsPipeline always uses dynamic rendering regardless of value.
        bool dynamicRendering = true;
        /// Depth attachment format, or VK_FORMAT_UNDEFINED when unused.
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        /// Optional single-sample target populated by a multisampled depth resolve.
        VkFormat depthResolveFormat = VK_FORMAT_UNDEFINED;
        /// Resolve operation for depth; VK_RESOLVE_MODE_NONE disables the target.
        VkResolveModeFlagBits depthResolveMode = VK_RESOLVE_MODE_NONE;
        /// Multisample count used by the color and depth attachments.
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        /// Load operation for the color attachment.
        VkAttachmentLoadOp colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkAttachmentLoadOp depthLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        /// Layout expected when the color attachment is first used.
        VkImageLayout colorInitialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        /// The caller supplies external synchronization for colorInitialLayout.
        /// This retains render-pass compatibility with an UNDEFINED-entry pass.
        bool colorInitialLayoutExternallySynchronized = false;
        /// Layout required after rendering completes.
        VkImageLayout colorFinalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkImageLayout depthInitialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout depthFinalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        /// SPIR-V module containing both vertexMain and fragmentMain.
        std::filesystem::path shader;
        /// Optional asset manager used to load shader sources.
        Assets::AssetManager *assetManager = nullptr;

        /// Size of the push-constant range in bytes.
        uint32_t pushConstantSize = 0;
        /// Shader stages that may access the push constants.
        VkShaderStageFlags pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT;

        /// Face-culling configuration.
        VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
        /// Winding order treated as front-facing.
        VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        /// Enabled color channels in the color attachment.
        VkColorComponentFlags colorWriteMask = static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_R_BIT) |
                                               static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_G_BIT) |
                                               static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_B_BIT) |
                                               static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_A_BIT);
        /// Enabled color channels in the optional second MRT attachment.
        VkColorComponentFlags additionalColorWriteMask = static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_R_BIT) |
                                                         static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_G_BIT) |
                                                         static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_B_BIT) |
                                                         static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_A_BIT);
        VkColorComponentFlags thirdColorWriteMask = static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_R_BIT) |
                                                     static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_G_BIT);
        /// Enables writing depth values.
        VkBool32 depthWriteEnable = VK_TRUE;
        /// Enables depth testing.
        VkBool32 depthTestEnable = VK_TRUE;
        /// Comparison operation used by the depth test.
        VkCompareOp depthCompareOp = VK_COMPARE_OP_LESS;
        /// Enables alpha blending in the color blend state.
        VkBool32 alphaBlendEnable = VK_FALSE;

        /// Vertex-buffer binding descriptions.
        std::vector<VkVertexInputBindingDescription> vertexBindings;
        /// Vertex attribute descriptions.
        std::vector<VkVertexInputAttributeDescription> vertexAttributes;
        /// Descriptor-set layouts included in the pipeline layout.
        std::vector<VkDescriptorSetLayout> descriptorSetLayouts;
    };

    /**
     * @brief Owns a dynamic-rendering pipeline layout and graphics pipeline.
     */
    class GraphicsPipeline final {
    public:
        /// Creates an empty graphics-pipeline wrapper.
        GraphicsPipeline() = default;

        /// Destroys the owned pipeline resources.
        ~GraphicsPipeline();

        /// Copy construction is disabled because the object owns Vulkan handles.
        GraphicsPipeline(const GraphicsPipeline &) = delete;

        /// Copy assignment is disabled because the object owns Vulkan handles.
        GraphicsPipeline &operator=(const GraphicsPipeline &) = delete;

        /// Move construction is disabled because Vulkan ownership is not transferable.
        GraphicsPipeline(GraphicsPipeline &&) = delete;

        /// Move assignment is disabled because Vulkan ownership is not transferable.
        GraphicsPipeline &operator=(GraphicsPipeline &&) = delete;

        /**
         * @brief Creates the pipeline layout and graphics pipeline.
         * @param device Logical Vulkan device used for resource creation.
         * @param options Pipeline and attachment configuration.
         */
        void create(VkDevice device, const GraphicsPipelineOptions &options);

        /// Releases all resources owned by this pipeline.
        void destroy() noexcept;

        /** @brief Returns the pipeline layout handle. */
        [[nodiscard]] VkPipelineLayout layout() const noexcept {
            return layout_;
        }

        /** @brief Returns the graphics pipeline handle. */
        [[nodiscard]] VkPipeline handle() const noexcept {
            return pipeline_;
        }

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPipelineLayout layout_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;

        /// Creates the descriptor-set and push-constant pipeline layout.
        void createPipelineLayout(const GraphicsPipelineOptions &options);

        /// Creates shader stages and the graphics pipeline state object.
        void createGraphicsPipeline(const GraphicsPipelineOptions &options);
    };
} // namespace Engine
