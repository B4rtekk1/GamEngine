#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Engine {

struct GraphicsPipelineOptions {
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

    std::filesystem::path vertexShader;
    std::filesystem::path fragmentShader;

    uint32_t pushConstantSize = 0;
    VkShaderStageFlags pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT;

    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
    VkFrontFace frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkVertexInputBindingDescription vertexBinding{};
    std::vector<VkVertexInputAttributeDescription> vertexAttributes;
    std::vector<VkDescriptorSetLayout> descriptorSetLayouts;
};

class GraphicsPipeline final {
public:
    GraphicsPipeline() = default;
    ~GraphicsPipeline();

    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    GraphicsPipeline(GraphicsPipeline&&) = delete;
    GraphicsPipeline& operator=(GraphicsPipeline&&) = delete;

    void create(VkDevice device, const GraphicsPipelineOptions& options);
    void destroy() noexcept;

    [[nodiscard]] VkRenderPass renderPass() const noexcept {
        return renderPass_;
    }

    [[nodiscard]] VkPipelineLayout layout() const noexcept {
        return layout_;
    }

    [[nodiscard]] VkPipeline handle() const noexcept {
        return pipeline_;
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    void createRenderPass(const GraphicsPipelineOptions& options);
    void createPipelineLayout(const GraphicsPipelineOptions& options);
    void createGraphicsPipeline(const GraphicsPipelineOptions& options);
};

} // namespace Engine
