#pragma once

#include "Engine/Math/AABB.h"

#include <glm/mat4x4.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <memory>
#include <span>

namespace Engine {

/** Owns the FidelityFX 1.1.4 Vulkan Brixelizer context and its external SDF resources. */
class BrixelizerSystem final {
public:
    struct StaticMesh final {
        AABB worldBounds{};
        glm::mat4 transform{1.0F};
        std::uint32_t firstVertex{};
        std::uint32_t vertexCount{};
        std::uint32_t firstIndex{};
        std::uint32_t indexCount{};
    };

    enum class DebugView : std::uint8_t { Off, Distance, Gradient, BrickId, CascadeId };

    BrixelizerSystem();
    ~BrixelizerSystem();
    BrixelizerSystem(const BrixelizerSystem&) = delete;
    BrixelizerSystem& operator=(const BrixelizerSystem&) = delete;

    void create(VkPhysicalDevice physicalDevice, VkDevice device, VmaAllocator allocator,
                VkExtent2D extent, std::uint32_t framesInFlight);
    void destroy() noexcept;
    void resize(VkExtent2D extent);

    /** Call only after all GPU work referencing the previous scene or buffers has retired. */
    void setStaticMeshes(VkBuffer vertices, VkDeviceSize vertexBytes,
                         VkBuffer indices, VkDeviceSize indexBytes,
                         std::span<const StaticMesh> meshes);

    /** The caller must wait for the frame slot's fence before reusing its scratch buffer. */
    void update(VkCommandBuffer commandBuffer, const float cameraPosition[3],
                std::uint32_t frameIndex, std::uint32_t frameSlot,
                DebugView debugView = DebugView::Off,
                const glm::mat4& inverseView = glm::mat4(1.0F),
                const glm::mat4& inverseProjection = glm::mat4(1.0F));

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool hasStaticMeshes() const noexcept;
    [[nodiscard]] VkImage debugImage() const noexcept;
    [[nodiscard]] VkExtent2D extent() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Engine
