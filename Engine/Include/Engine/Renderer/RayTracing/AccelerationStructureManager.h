#pragma once

#include "Engine/Renderer/Vulkan/buffer.h"

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

namespace Engine {
    /**
     * Owns the acceleration structures used by inline ray queries.  It is
     * intentionally independent of raster shadowing: callers supply compact
     * geometry/instance descriptions harvested from the GPU scene.
     */
    class AccelerationStructureManager final {
    public:
        struct BlasKey final {
            const void* mesh{};
            std::uint32_t firstIndex{};
            std::uint32_t indexCount{};
            bool operator==(const BlasKey&) const noexcept = default;
        };
        struct BlasKeyHash final {
            std::size_t operator()(const BlasKey& key) const noexcept {
                std::size_t hash = std::hash<const void*>{}(key.mesh);
                hash ^= std::hash<std::uint32_t>{}(key.firstIndex) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
                hash ^= std::hash<std::uint32_t>{}(key.indexCount) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
                return hash;
            }
        };
        struct MeshBuildInput final {
            BlasKey key{};
            VkDeviceAddress vertexAddress{};
            VkDeviceAddress indexAddress{};
            std::uint32_t firstVertex{};
            std::uint32_t vertexCount{};
            std::uint32_t indexCount{};
        };
        struct InstanceBuildInput final {
            BlasKey meshKey{};
            // VkTransformMatrixKHR is explicitly row-major 3x4.
            std::array<float, 12> transform{};
            std::uint8_t mask{0x01};
            std::uint32_t customIndex{};
        };

        AccelerationStructureManager() = default;
        ~AccelerationStructureManager();
        AccelerationStructureManager(const AccelerationStructureManager&) = delete;
        AccelerationStructureManager& operator=(const AccelerationStructureManager&) = delete;

        void create(VkPhysicalDevice physicalDevice, VkDevice device, VmaAllocator allocator);
        void destroy() noexcept;
        /** Recreates cached BLASes. Call after heap reallocation or mesh edits. */
        void rebuildBlases(VkCommandBuffer commandBuffer, std::span<const MeshBuildInput> meshes);
        /** Builds/refits the TLAS from current transforms; BLAS geometry is untouched. */
        void updateTlas(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                        std::span<const InstanceBuildInput> instances);
        [[nodiscard]] VkAccelerationStructureKHR tlas(std::uint32_t frameIndex) const noexcept;
        [[nodiscard]] bool ready(std::uint32_t frameIndex) const noexcept;
        /** True only after this frame slot has received a successful build. */
        [[nodiscard]] bool built(std::uint32_t frameIndex) const noexcept;

    private:
        struct Structure final {
            VkAccelerationStructureKHR handle{VK_NULL_HANDLE};
            Buffer storage;
            VkDeviceAddress address{};
        };
        void destroyStructure(Structure& structure) noexcept;
        void createStructure(Structure& structure, VkAccelerationStructureTypeKHR type, VkDeviceSize bytes);
        [[nodiscard]] VkDeviceAddress meshAddress(const BlasKey& key) const noexcept;
        [[nodiscard]] VkDeviceAddress ensureScratch(Buffer& buffer, VkDeviceSize requiredSize);

        VkDevice device_{VK_NULL_HANDLE};
        VmaAllocator allocator_{VK_NULL_HANDLE};
        PFN_vkCreateAccelerationStructureKHR createAccelerationStructure_{};
        PFN_vkDestroyAccelerationStructureKHR destroyAccelerationStructure_{};
        PFN_vkGetAccelerationStructureBuildSizesKHR getBuildSizes_{};
        PFN_vkCmdBuildAccelerationStructuresKHR cmdBuild_{};
        PFN_vkGetAccelerationStructureDeviceAddressKHR getAddress_{};
        std::unordered_map<BlasKey, Structure, BlasKeyHash> blases_;
        static constexpr std::uint32_t FramesInFlight = 2;
        struct FrameResources final {
            Structure tlas;
            Buffer instances;
            Buffer scratch;
            VkDeviceSize tlasAllocatedSize{};
            std::uint32_t instanceCapacity{};
            std::uint32_t builtInstanceCount{};
            bool tlasBuilt{};
        };
        Buffer blasScratch_;
        std::array<FrameResources, FramesInFlight> frames_{};
        VkDeviceSize scratchAlignment_{1};
    };
} // namespace Engine
