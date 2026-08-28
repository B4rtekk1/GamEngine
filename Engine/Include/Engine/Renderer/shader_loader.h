#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <vector>

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

            ShaderModule(const ShaderModule &) = delete;

            ShaderModule &operator=(const ShaderModule &) = delete;

            ShaderModule(ShaderModule &&other) noexcept;

            ShaderModule &operator=(ShaderModule &&other) noexcept;

            [[nodiscard]] VkShaderModule get() const noexcept { return module_; }
            [[nodiscard]] explicit operator bool() const noexcept { return module_ != VK_NULL_HANDLE; }

        private:
            void reset() noexcept;

            VkDevice device_ = VK_NULL_HANDLE;
            VkShaderModule module_ = VK_NULL_HANDLE;
        };

        [[nodiscard]] std::vector<uint32_t> loadSpirv(
            const std::filesystem::path &path);

        [[nodiscard]] ShaderModule loadShaderModule(
            VkDevice device,
            const std::filesystem::path &path);

        [[nodiscard]] ShaderModule loadShaderModule(
            VkDevice device,
            Assets::AssetManager &assets,
            const std::filesystem::path &path);
    }
} // namespace Engine
