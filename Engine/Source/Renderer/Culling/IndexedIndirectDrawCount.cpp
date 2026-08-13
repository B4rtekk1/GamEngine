#include "Engine/Renderer/Culling/IndexedIndirectDrawCount.h"

#include <stdexcept>

namespace Engine::Culling {

void IndexedIndirectDrawCount::create(
    const VkBuffer commandBuffer,
    const VkBuffer countBuffer,
    const std::uint32_t maxDrawCount,
    const VkDeviceSize commandOffset,
    const VkDeviceSize countOffset,
    const std::uint32_t stride) {
    if (commandBuffer == VK_NULL_HANDLE || countBuffer == VK_NULL_HANDLE) {
        throw std::invalid_argument("Indirect draw buffers cannot be null");
    }
    if (maxDrawCount == 0) {
        throw std::invalid_argument("Indirect draw count must be greater than zero");
    }
    if (stride < sizeof(VkDrawIndexedIndirectCommand) || stride % 4 != 0) {
        throw std::invalid_argument("Indirect draw stride is invalid");
    }
    if (commandOffset % 4 != 0 || countOffset % 4 != 0) {
        throw std::invalid_argument("Indirect draw offsets must be 4-byte aligned");
    }

    commandBuffer_ = commandBuffer;
    countBuffer_ = countBuffer;
    commandOffset_ = commandOffset;
    countOffset_ = countOffset;
    maxDrawCount_ = maxDrawCount;
    stride_ = stride;
}

void IndexedIndirectDrawCount::destroy() noexcept {
    commandBuffer_ = VK_NULL_HANDLE;
    countBuffer_ = VK_NULL_HANDLE;
    commandOffset_ = 0;
    countOffset_ = 0;
    maxDrawCount_ = 0;
    stride_ = sizeof(VkDrawIndexedIndirectCommand);
}

void IndexedIndirectDrawCount::record(const VkCommandBuffer commandBuffer) const {
    if (commandBuffer == VK_NULL_HANDLE) {
        throw std::invalid_argument("Command buffer cannot be null");
    }
    if (!valid()) {
        throw std::logic_error("IndexedIndirectDrawCount is not initialized");
    }

    vkCmdDrawIndexedIndirectCount(
        commandBuffer,
        commandBuffer_,
        commandOffset_,
        countBuffer_,
        countOffset_,
        maxDrawCount_,
        stride_);
}

bool IndexedIndirectDrawCount::valid() const noexcept {
    return commandBuffer_ != VK_NULL_HANDLE &&
           countBuffer_ != VK_NULL_HANDLE &&
           maxDrawCount_ != 0;
}

} // namespace Engine::Culling
