#pragma once

#include <cstdint>
#include <cstdio>
#include <vulkan/vulkan.h>

// TEMP-DEBUG: prints every created image's handle in validation (debug)
// builds, so a handle in a validation message can be matched to its owner.
inline void tempLogImage(const char* owner, const VkImage image, const VkImageCreateInfo& info) {
#ifndef NDEBUG
    std::fprintf(stderr, "TEMP-DEBUG image %s 0x%llx %ux%u fmt=%d mips=%u usage=0x%x\n", owner,
                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(image)), info.extent.width,
                 info.extent.height, static_cast<int>(info.format), info.mipLevels, info.usage);
#else
    static_cast<void>(owner);
    static_cast<void>(image);
    static_cast<void>(info);
#endif
}
