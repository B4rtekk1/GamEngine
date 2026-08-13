#pragma once

#include "Engine/Renderer/Lighting/DirectionalLightData.h"

#include <vulkan/vulkan.h>

namespace Engine {

    class Registry;

    class LightingBuffer {
    public:
        LightingBuffer() = default;
        ~LightingBuffer();

        LightingBuffer(const LightingBuffer&) = delete;
        LightingBuffer& operator=(const LightingBuffer&) = delete;

        void initialize(
            VkPhysicalDevice physicalDevice,
            VkDevice device
        );

        void shutdown();

        void update(Registry& registry) const;

        [[nodiscard]] VkBuffer buffer() const { return m_buffer; }

        [[nodiscard]] VkDeviceSize size() const { return sizeof(DirectionalLightGPU); }

    private:
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;

        VkBuffer m_buffer = VK_NULL_HANDLE;
        VkDeviceMemory m_memory = VK_NULL_HANDLE;

        void* m_mappedMemory = nullptr;
    };
}
