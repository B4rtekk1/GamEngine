#pragma once

#include "Engine/Assets/AssetTypes.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Engine::Assets::CookCache {
    // Bump these when encoder behavior or mip/normal processing changes.
    inline constexpr float compressionQuality = 0.10F;
    inline constexpr std::uint32_t cookerVersion = 4;
    inline constexpr std::uint32_t mipPolicy = 2; // Full chain; sRGB and normal mips use semantic filtering.
    inline constexpr std::uint32_t normalPolicy = 1; // Normalize tangent-space vectors before encoding RG.

    struct Hash64 final {
        std::uint64_t value{14695981039346656037ULL};
        void add(std::span<const std::uint8_t> bytes) noexcept;
        void add(std::string_view text) noexcept;
        void add(std::uint64_t number) noexcept;
    };

    struct TextureCookKey final {
        std::uint64_t sourceHash{};
        TextureFormat format{TextureFormat::BC7_UNORM};
        bool srgb{};
        std::uint32_t cookerVersion{CookCache::cookerVersion};
        std::uint32_t mipPolicy{CookCache::mipPolicy};
        std::uint32_t normalPolicy{CookCache::normalPolicy};
        float quality{compressionQuality};
    };

    [[nodiscard]] std::optional<std::uint64_t> hash_file(const std::filesystem::path &path);
    [[nodiscard]] std::uint64_t texture_key(const TextureCookKey &key) noexcept;
    [[nodiscard]] std::uint64_t texture_settings_key() noexcept;
    [[nodiscard]] std::string hex_key(std::uint64_t key);
    [[nodiscard]] std::filesystem::path root_for_source(const std::filesystem::path &source);
    [[nodiscard]] std::filesystem::path texture_path(const std::filesystem::path &root, std::uint64_t key);
    [[nodiscard]] bool artifact_has_key(const std::filesystem::path &path, std::uint64_t key);
    [[nodiscard]] bool write_artifact_key(const std::filesystem::path &path, std::uint64_t key);
    [[nodiscard]] bool publish(const std::filesystem::path &cached, const std::filesystem::path &output,
                               std::uint64_t key);
} // namespace Engine::Assets::CookCache
