#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

/**
 * @file IndexedIndirectDrawCount.h
 * @brief Declares a wrapper for counted indexed indirect drawing.
 */

namespace Engine::Culling {

/**
 * @brief Records a counted batch of indexed indirect draw commands.
 *
 * The command and count buffers are owned externally. This class stores only
 * the draw configuration required by vkCmdDrawIndexedIndirectCount.
 */
class IndexedIndirectDrawCount final {
public:
    /**
     * @brief Configures the indirect draw operation.
     * @param commandBuffer Buffer containing VkDrawIndexedIndirectCommand values.
     * @param countBuffer Buffer containing the number of draws to execute.
     * @param maxDrawCount Maximum number of commands that may be consumed.
     * @param commandOffset Byte offset of the first command in @p commandBuffer.
     * @param countOffset Byte offset of the draw count in @p countBuffer.
     * @param stride Byte distance between consecutive indirect commands.
     */
    void create(
        VkBuffer commandBuffer,
        VkBuffer countBuffer,
        std::uint32_t maxDrawCount,
        VkDeviceSize commandOffset = 0,
        VkDeviceSize countOffset = 0,
        std::uint32_t stride = sizeof(VkDrawIndexedIndirectCommand));

    /** @brief Resets the stored draw configuration to an invalid state. */
    void destroy() noexcept;

    /**
     * @brief Records the counted indexed indirect draw command.
     * @param commandBuffer Command buffer into which the draw is recorded.
     */
    void record(VkCommandBuffer commandBuffer) const;

    /** @brief Returns whether the operation has valid command and count buffers. */
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