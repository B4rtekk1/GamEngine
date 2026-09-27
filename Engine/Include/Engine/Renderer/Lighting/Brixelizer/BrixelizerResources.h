#pragma once

#include "Engine/Renderer/Vulkan/buffer.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>

namespace Engine {

/** GPU storage supplied by GamEngine to FidelityFX Brixelizer. */
struct BrixelizerResources final {
    static constexpr std::uint32_t CascadeCount = 6;

    VkImage sdfAtlas{VK_NULL_HANDLE};
    VmaAllocation sdfAtlasAllocation{VK_NULL_HANDLE};
    Buffer brickAabbs;

    struct Cascade final {
        Buffer aabbTree;
        Buffer brickMap;
    };
    std::array<Cascade, CascadeCount> cascades;
    Buffer scratch;
};

} // namespace Engine
