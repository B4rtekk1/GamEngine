#include "Engine/Renderer/Vulkan/PipelineCache.h"

#include "Engine/Core/Diagnostics.h"

#include <SDL3/SDL.h>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace Engine {
namespace {
std::mutex cacheMutex;
std::unordered_map<VkDevice, VkPipelineCache> caches;
constexpr std::size_t MaxCacheBytes = 64 * 1024 * 1024;
} // namespace

PipelineCache::~PipelineCache() {
    destroy();
}

void PipelineCache::create(const VkPhysicalDevice physical, const VkDevice device,
                           const std::filesystem::path& directory) {
    destroy();
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    std::string uuid;
    constexpr char Hex[] = "0123456789abcdef";
    for (const auto byte : properties.pipelineCacheUUID) {
        uuid += Hex[byte >> 4];
        uuid += Hex[byte & 15];
    }
    path_ = directory /
            (std::to_string(properties.vendorID) + "-" + std::to_string(properties.deviceID) + "-" + uuid + ".bin");
    std::vector<char> bytes;
    std::ifstream input(path_, std::ios::binary | std::ios::ate);
    if (input) {
        const auto size = input.tellg();
        if (size >= 32 && size <= static_cast<std::streamoff>(MaxCacheBytes)) {
            bytes.resize(static_cast<std::size_t>(size));
            input.seekg(0);
            input.read(bytes.data(), size);
            // Read scalars separately: file buffers need not be struct-aligned.
            std::uint32_t header[4]{};
            std::memcpy(header, bytes.data(), sizeof(header));
            if (!input || header[0] < 32 || header[0] > bytes.size() ||
                header[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || header[2] != properties.vendorID ||
                header[3] != properties.deviceID ||
                std::memcmp(bytes.data() + 16, properties.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
                bytes.clear();
            }
        }
    }
    VkPipelineCacheCreateInfo info{.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = bytes.size();
    info.pInitialData = bytes.empty() ? nullptr : bytes.data();
    auto result = vkCreatePipelineCache(device, &info, nullptr, &cache_);
    if (result != VK_SUCCESS && !bytes.empty()) {
        bytes.clear();
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        result = vkCreatePipelineCache(device, &info, nullptr, &cache_);
    }
    if (result != VK_SUCCESS)
        throw std::runtime_error("Could not create Vulkan pipeline cache");
    device_ = device;
    {
        std::lock_guard lock(cacheMutex);
        caches[device_] = cache_;
    }
    Diagnostics::instance().report(DiagnosticSeverity::Info,
                                   "[PipelineCache] Loaded " + std::to_string(bytes.size()) +
                                       " bytes: " + path_.string(),
                                   {.subsystem = "Vulkan"});
}

VkPipelineCache PipelineCache::get(const VkDevice device) {
    // SDL event pumping must stay on the application thread. Leave events
    // queued for the normal editor loop, including close and input events.
    if (SDL_IsMainThread())
        SDL_PumpEvents();
    std::lock_guard lock(cacheMutex);
    const auto found = caches.find(device);
    return found == caches.end() ? VK_NULL_HANDLE : found->second;
}

void PipelineCache::save() const noexcept {
    if (cache_ == VK_NULL_HANDLE)
        return;
    try {
        std::size_t size = 0;
        if (vkGetPipelineCacheData(device_, cache_, &size, nullptr) != VK_SUCCESS || size == 0 || size > MaxCacheBytes)
            return;
        std::vector<char> bytes(size);
        if (vkGetPipelineCacheData(device_, cache_, &size, bytes.data()) != VK_SUCCESS)
            return;
        std::filesystem::create_directories(path_.parent_path());
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(size));
    } catch (...) {
        // An unwritable or damaged cache must never prevent rendering.
    }
}

void PipelineCache::destroy() noexcept {
    if (cache_ == VK_NULL_HANDLE)
        return;
    save();
    {
        std::lock_guard lock(cacheMutex);
        caches.erase(device_);
    }
    vkDestroyPipelineCache(device_, cache_, nullptr);
    cache_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}
} // namespace Engine
