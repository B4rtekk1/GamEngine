#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace Engine::Culling {

/**
 * Records a counted batch of indexed indirect draw commands.
 *
 * The command and count buffers are owned externally. This class only keeps
 * the draw configuration used by vkCmdDrawIndexedIndirectCount.
 */
class IndexedIndirectDrawCount final {
public:
    void create(
        VkBuffer commandBuffer,
        VkBuffer countBuffer,
        std::uint32_t maxDrawCount,
        VkDeviceSize commandOffset = 0,
        VkDeviceSize countOffset = 0,
        std::uint32_t stride = sizeof(VkDrawIndexedIndirectCommand));

    void destroy() noexcept;

    void record(VkCommandBuffer commandBuffer) const;

    [[nodiscard]] bool valid() const noexcept;

private:
    VkBuffer commandBuffer_{VK_NULL_HANDLE};
    VkBuffer countBuffer_{VK_NULL_HANDLE};
    VkDeviceSize commandOffset_{0};
    VkDeviceSize countOffset_{0};
    std::uint32_t maxDrawCount_{0};
    std::uint32_t stride_{sizeof(VkDrawIndexedIndirectCommand)};
};

} // namespace Engine::Culling
