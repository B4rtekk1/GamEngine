#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets {
class AssetManager;
}

namespace Vkutil {
class ShaderModule final {
  public:
    ShaderModule() = default;

    ShaderModule(VkDevice device, VkShaderModule module) noexcept;

    ~ShaderModule();

    ShaderModule(const ShaderModule&) = delete;

    ShaderModule& operator=(const ShaderModule&) = delete;

    ShaderModule(ShaderModule&& other) noexcept;

    ShaderModule& operator=(ShaderModule&& other) noexcept;

    [[nodiscard]] VkShaderModule get() const noexcept {
        return module_;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return module_ != VK_NULL_HANDLE;
    }

    /**
     * Input locations the vertex entry point reads, or std::nullopt when they
     * could not be determined (matrix/array/struct inputs, missing entry point).
     */
    [[nodiscard]] const std::optional<std::vector<uint32_t>>& vertexInputLocations() const noexcept {
        return vertexInputLocations_;
    }
    /** Drops attributes the vertex entry point does not read (avoids validation warnings). */
    [[nodiscard]] std::vector<VkVertexInputAttributeDescription>
    filterVertexAttributes(std::span<const VkVertexInputAttributeDescription> attributes) const {
        std::vector<VkVertexInputAttributeDescription> result(attributes.begin(), attributes.end());
        if (vertexInputLocations_.has_value()) {
            std::erase_if(result, [&](const VkVertexInputAttributeDescription& attribute) {
                return std::ranges::find(*vertexInputLocations_, attribute.location) == vertexInputLocations_->end();
            });
        }
        return result;
    }
    void setVertexInputLocations(std::optional<std::vector<uint32_t>> locations) {
        vertexInputLocations_ = std::move(locations);
    }

  private:
    void reset() noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VkShaderModule module_ = VK_NULL_HANDLE;
    std::optional<std::vector<uint32_t>> vertexInputLocations_;
};

/** Reflects the Input locations consumed by @p entryPoint (a vertex entry point) in @p code. */
[[nodiscard]] std::optional<std::vector<uint32_t>> reflectVertexInputLocations(const std::vector<uint32_t>& code,
                                                                                std::string_view entryPoint);

[[nodiscard]] std::vector<uint32_t> loadSpirv(const std::filesystem::path& path);

[[nodiscard]] ShaderModule loadShaderModule(VkDevice device, const std::filesystem::path& path);

[[nodiscard]] ShaderModule loadShaderModule(VkDevice device, Assets::AssetManager& assets,
                                            const std::filesystem::path& path);
} // namespace Vkutil
} // namespace Engine
