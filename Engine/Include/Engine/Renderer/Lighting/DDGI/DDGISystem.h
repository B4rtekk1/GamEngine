#pragma once

#include "Engine/Renderer/Lighting/DDGI/DDGIResources.h"
#include "Engine/Renderer/Lighting/DDGI/DDGIVolume.h"

#include <array>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace Engine {
    namespace Assets { class AssetManager; }

    class DDGISystem final {
    public:
        DDGISystem() = default;
        ~DDGISystem();
        DDGISystem(const DDGISystem&) = delete;
        DDGISystem& operator=(const DDGISystem&) = delete;

        void create(VkPhysicalDevice physical, VkDevice device, VmaAllocator allocator,
                    VkDescriptorSetLayout sceneLayout, Assets::AssetManager& assets);
        void destroy() noexcept;
        void record(VkCommandBuffer commandBuffer, std::uint32_t frameSlot,
                    std::uint32_t frameIndex, VkDescriptorSet sceneSet,
                    VkAccelerationStructureKHR tlas, VkBuffer instances,
                    VkBuffer meshes, VkBuffer materials, VkBuffer vertices,
                    VkBuffer indices, const std::array<float, 3>& cameraPosition);
        [[nodiscard]] bool ready() const noexcept;
        [[nodiscard]] const DDGIResources& resources() const noexcept { return resources_; }
        [[nodiscard]] const DDGIVolume& volume(std::uint32_t cascade) const noexcept { return volumes_[cascade]; }
        [[nodiscard]] bool created() const noexcept { return tracePipeline_ != VK_NULL_HANDLE; }

    private:
        VkDevice device_{VK_NULL_HANDLE};
        std::array<DDGIVolume, 3> volumes_{};
        DDGIResources resources_{};
        VkDescriptorSetLayout layout_{VK_NULL_HANDLE};
        VkDescriptorPool pool_{VK_NULL_HANDLE};
        std::array<std::array<VkDescriptorSet, 2>, 3> sets_{};
        VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
        VkPipeline tracePipeline_{VK_NULL_HANDLE};
        VkPipeline schedulePipeline_{VK_NULL_HANDLE};
        VkPipeline finishPipeline_{VK_NULL_HANDLE};
        VkPipeline validatePipeline_{VK_NULL_HANDLE};
        VkPipeline relocatePipeline_{VK_NULL_HANDLE};
        VkPipeline classifyPipeline_{VK_NULL_HANDLE};
        VkPipeline scrollResetPipeline_{VK_NULL_HANDLE};
        VkPipeline irradiancePipeline_{VK_NULL_HANDLE};
        VkPipeline distancePipeline_{VK_NULL_HANDLE};
        struct CascadeState final {
            std::array<std::int32_t, 3> originCell{};
            std::array<std::int32_t, 3> scrollOffset{};
            bool initialized{};
        };
        std::array<CascadeState, 3> cascadeStates_{};
    };
}
