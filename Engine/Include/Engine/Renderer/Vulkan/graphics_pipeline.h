#pragma once

/**
 * @file graphics_pipeline(1).h
 * @brief Declares configurable Vulkan graphics-pipeline creation helpers.
 */

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Engine {

namespace Assets { class AssetManager; }

/**
 * @brief Parameters used to create a GraphicsPipeline.
 */
struct GraphicsPipelineOptions {
    /// Color attachment format used by the render pass.
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    /// Existing render pass to use instead of creating one.
    VkRenderPass existingRenderPass = VK_NULL_HANDLE;
    /// Depth attachment format, or VK_FORMAT_UNDEFINED when unused.
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    /// Multisample count used by the color and depth attachments.
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    /// Resolve mode for a multisampled depth attachment. Ignored at 1x.
    VkResolveModeFlagBits depthResolveMode = VK_RESOLVE_MODE_NONE;
    /// Load operation for the color attachment.
    VkAttachmentLoadOp colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    /// Layout expected when the color attachment is first used.
    VkImageLayout colorInitialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    /// Layout required after rendering completes.
    VkImageLayout colorFinalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    /// Vertex shader source path.
    std::filesystem::path vertexShader;
    /// Fragment shader source path.
    std::filesystem::path fragmentShader;
    /// Optional asset manager used to load shader sources.
    Assets::AssetManager* assetManager = nullptr;

    /// Size of the push-constant range in bytes.
    uint32_t pushConstantSize = 0;
    /// Shader stages that may access the push constants.
    VkShaderStageFlags pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT;

    /// Face-culling configuration.
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
    /// Winding order treated as front-facing.
    VkFrontFace frontFace = VK_FRONT_FACE_CLOCKWISE;
    /// Enabled color channels in the color attachment.
    VkColorComponentFlags colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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
 * @brief Owns a Vulkan render pass, pipeline layout and graphics pipeline.
 *
 * Unless an existing render pass is supplied through
 * GraphicsPipelineOptions::existingRenderPass, the class creates and owns a
 * compatible render pass along with the graphics pipeline resources.
 */
class GraphicsPipeline final {
public:
    /// Creates an empty graphics-pipeline wrapper.
    GraphicsPipeline() = default;
    /// Destroys the owned pipeline resources.
    ~GraphicsPipeline();

    /// Copy construction is disabled because the object owns Vulkan handles.
    GraphicsPipeline(const GraphicsPipeline&) = delete;
    /// Copy assignment is disabled because the object owns Vulkan handles.
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    /// Move construction is disabled because Vulkan ownership is not transferable.
    GraphicsPipeline(GraphicsPipeline&&) = delete;
    /// Move assignment is disabled because Vulkan ownership is not transferable.
    GraphicsPipeline& operator=(GraphicsPipeline&&) = delete;

    /**
     * @brief Creates the render pass, pipeline layout and graphics pipeline.
     * @param device Logical Vulkan device used for resource creation.
     * @param options Pipeline and attachment configuration.
     */
    void create(VkDevice device, const GraphicsPipelineOptions& options);
    /// Releases all resources owned by this pipeline.
    void destroy() noexcept;

    /** @brief Returns the render pass used by the pipeline. */
    [[nodiscard]] VkRenderPass renderPass() const noexcept {
        return renderPass_;
    }

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
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    bool ownsRenderPass_ = false;

    /// Creates a render pass when the options do not provide an existing one.
    void createRenderPass(const GraphicsPipelineOptions& options);
    /// Creates the descriptor-set and push-constant pipeline layout.
    void createPipelineLayout(const GraphicsPipelineOptions& options);
    /// Creates shader stages and the graphics pipeline state object.
    void createGraphicsPipeline(const GraphicsPipelineOptions& options);
};

} // namespace Engine
