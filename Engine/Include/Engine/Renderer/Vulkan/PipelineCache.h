#pragma once

#include <filesystem>
#include <vulkan/vulkan.h>

namespace Engine {
class PipelineCache final {
  public:
    PipelineCache() = default;
    ~PipelineCache();
    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    void create(VkPhysicalDevice physical, VkDevice device, const std::filesystem::path& directory);
    void save() const noexcept;
    void destroy() noexcept;

    // Also lets the window system handle pings between driver compilations.
    [[nodiscard]] static VkPipelineCache get(VkDevice device);

  private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    std::filesystem::path path_;
};
} // namespace Engine
