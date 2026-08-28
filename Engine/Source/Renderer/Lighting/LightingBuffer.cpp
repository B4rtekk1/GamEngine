#include "Engine/Renderer/Lighting/LightingBuffer.h"

#include "Engine/ECS/Registry.h"
#include "Engine/Scene/Components/LightComponent.h"
#include "Engine/ECS/Components/ColorPickerComponent.h"
#include "Engine/ECS/Components/TransformComponent.h"

#include <cstring>
#include <stdexcept>

namespace Engine {

namespace {

std::uint32_t findMemoryType(
    VkPhysicalDevice physicalDevice,
    std::uint32_t typeFilter,
    VkMemoryPropertyFlags properties
) {
    VkPhysicalDeviceMemoryProperties memoryProperties{};

    vkGetPhysicalDeviceMemoryProperties(
        physicalDevice,
        &memoryProperties
    );

    for (
        std::uint32_t i = 0;
        i < memoryProperties.memoryTypeCount;
        ++i
    ) {
        const bool typeMatches =
            (typeFilter & (1u << i)) != 0;

        const bool propertiesMatch =
            (memoryProperties.memoryTypes[i].propertyFlags &
             properties) == properties;

        if (typeMatches && propertiesMatch) {
            return i;
        }
    }

    throw std::runtime_error(
        "Failed to find suitable memory type"
    );
}

}

LightingBuffer::~LightingBuffer() {
    shutdown();
}

void LightingBuffer::initialize(
    VkPhysicalDevice physicalDevice,
    VkDevice device
) {
    shutdown();

    if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
        throw std::invalid_argument("LightingBuffer requires valid Vulkan handles");
    }

    m_physicalDevice = physicalDevice;
    m_device = device;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType =
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;

    bufferInfo.size =
        sizeof(DirectionalLightGPU);

    bufferInfo.usage =
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

    bufferInfo.sharingMode =
        VK_SHARING_MODE_EXCLUSIVE;

    try {
        if (
            vkCreateBuffer(
                m_device,
                &bufferInfo,
                nullptr,
                &m_buffer
            ) != VK_SUCCESS
        ) {
            throw std::runtime_error(
                "Failed to create directional light buffer"
            );
        }

        VkMemoryRequirements memoryRequirements{};

        vkGetBufferMemoryRequirements(
            m_device,
            m_buffer,
            &memoryRequirements
        );

        VkMemoryAllocateInfo allocationInfo{};
        allocationInfo.sType =
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;

        allocationInfo.allocationSize =
            memoryRequirements.size;

        allocationInfo.memoryTypeIndex =
            findMemoryType(
                m_physicalDevice,
                memoryRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
            );

        if (
            vkAllocateMemory(
                m_device,
                &allocationInfo,
                nullptr,
                &m_memory
            ) != VK_SUCCESS
        ) {
            throw std::runtime_error(
                "Failed to allocate directional light memory"
            );
        }

        if (
            vkBindBufferMemory(
                m_device,
                m_buffer,
                m_memory,
                0
            ) != VK_SUCCESS
        ) {
            throw std::runtime_error(
                "Failed to bind directional light memory"
            );
        }

        if (
            vkMapMemory(
                m_device,
                m_memory,
                0,
                sizeof(DirectionalLightGPU),
                0,
                &m_mappedMemory
            ) != VK_SUCCESS
        ) {
            throw std::runtime_error(
                "Failed to map directional light memory"
            );
        }
    } catch (...) {
        shutdown();
        throw;
    }
}

void LightingBuffer::shutdown() {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }

    if (m_mappedMemory != nullptr) {
        vkUnmapMemory(
            m_device,
            m_memory
        );

        m_mappedMemory = nullptr;
    }

    if (m_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(
            m_device,
            m_buffer,
            nullptr
        );

        m_buffer = VK_NULL_HANDLE;
    }

    if (m_memory != VK_NULL_HANDLE) {
        vkFreeMemory(
            m_device,
            m_memory,
            nullptr
        );

        m_memory = VK_NULL_HANDLE;
    }

    m_device = VK_NULL_HANDLE;
    m_physicalDevice = VK_NULL_HANDLE;
}

void LightingBuffer::update(Registry& registry) const {
    if (m_mappedMemory == nullptr) {
        throw std::logic_error("LightingBuffer must be initialized before update");
    }

    DirectionalLightGPU gpuData{};

    gpuData.directionIntensity =
        Vec4{0.0F, -1.0F, 0.0F, 0.0F};

    gpuData.color = Math::Color::white().with_alpha(0.0F);

    bool foundDirectionalLight = false;

    registry.view<
        TransformComponent,
        LightComponent
    >(
        [&](
            const Entity entity,
            const TransformComponent& transform,
            const LightComponent& light
        ) {
            if (foundDirectionalLight) {
                return;
            }

            if (!light.enabled) {
                return;
            }

            if (light.type != LightType::Directional) {
                return;
            }

            const Vec4 rotatedDirection{
                transform.matrix().native() *
                Vec4{0.0F, 0.0F, -1.0F, 0.0F}.native()
            };

            const Vec3 direction = Vec3{
                rotatedDirection.x(),
                rotatedDirection.y(),
                rotatedDirection.z()
            }.normalized();

            gpuData.directionIntensity =
                Vec4{
                    direction.x(),
                    direction.y(),
                    direction.z(),
                    light.intensity
                };

            gpuData.color = (registry.has<ColorPickerComponent>(entity)
                                 ? registry.get<ColorPickerComponent>(entity).color
                                 : light.color).with_alpha(0.0F);

            foundDirectionalLight = true;
        }
    );

    std::memcpy(
        m_mappedMemory,
        &gpuData,
        sizeof(gpuData)
    );
}

}
