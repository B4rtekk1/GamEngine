#pragma once

#include "Engine/Renderer/Vulkan/shadow_map.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace Engine {

class Buffer;
class Mat4;
struct MeshRenderer;

namespace Culling {
class GPUCullingPass;
class IndexedIndirectDrawCount;
}

class ShadowPass final {
public:
    ShadowPass() = default;
    ~ShadowPass();

    ShadowPass(const ShadowPass&) = delete;
    ShadowPass& operator=(const ShadowPass&) = delete;

    void create(VkPhysicalDevice physicalDevice, VkDevice device,
                const std::vector<VkBuffer>& uniformBuffers,
                VkDeviceSize uniformBufferRange);
    void destroy() noexcept;

    void record(VkCommandBuffer commandBuffer, const Mat4& lightSpace,
                VkBuffer vertexBuffer, VkBuffer instanceBuffer, VkBuffer indexBuffer,
                const MeshRenderer& plane, const MeshRenderer& cubes,
                const Culling::GPUCullingPass& cullingPass,
                const Culling::IndexedIndirectDrawCount& indirectDraw,
                std::uint32_t objectCount) const;

    [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const noexcept {
        return descriptorSetLayout_;
    }

    [[nodiscard]] VkDescriptorSet descriptorSet(std::uint32_t frameIndex) const;

private:
    VkDevice device_{VK_NULL_HANDLE};
    ShadowMap shadowMap_;
    VkDescriptorSetLayout descriptorSetLayout_{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> descriptorSets_;
    VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
};

} // namespace Engine
