#pragma once

#include "Engine/Renderer/Skybox/Skybox.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <vector>

namespace Engine {
    namespace Assets {
        class AssetManager;
    }

    class SkyPass final {
    public:
        SkyPass() = default;

        ~SkyPass();

        SkyPass(const SkyPass &) = delete;

        SkyPass &operator=(const SkyPass &) = delete;

        void create(VkPhysicalDevice physicalDevice, VkDevice device,
                    VkCommandPool commandPool, VkQueue queue,
                    VkRenderPass renderPass, VkFormat colorFormat,
                    VkSampleCountFlagBits samples,
                    const std::vector<VkBuffer> &uniformBuffers,
                    VkDeviceSize uniformBufferRange, Assets::AssetManager &assets,
                    VmaAllocator allocator);

        void destroy() noexcept;

        void record(VkCommandBuffer commandBuffer, std::uint32_t frameIndex) const;

    private:
        VkDevice device_{VK_NULL_HANDLE};
        VkDescriptorSetLayout descriptorSetLayout_{VK_NULL_HANDLE};
        Skybox skybox_;
    };
} // namespace Engine
