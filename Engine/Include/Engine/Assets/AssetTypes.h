#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Engine::Assets {
    using AssetId = std::uint64_t;

    enum class AssetType : std::uint8_t {
        Unknown,
        Binary,
        Text,
        Shader,
        Texture2D,
        Cubemap,
        Mesh,
        Material,
        Scene,
        Font,
    };

    struct AssetMetadata {
        AssetId id{};
        AssetType type{AssetType::Unknown};
        std::filesystem::path source_path;
        std::filesystem::file_time_type last_write_time{};
        std::uintmax_t source_size{};
    };

    struct BinaryAsset {
        std::vector<std::byte> bytes;
    };

    struct TextAsset {
        std::string text;
    };

    struct ShaderAsset {
        std::string source;
        std::string entry_point{"main"};
    };

    inline constexpr std::string_view to_string(AssetType type) noexcept {
        switch (type) {
            case AssetType::Binary: return "Binary";
            case AssetType::Text: return "Text";
            case AssetType::Shader: return "Shader";
            case AssetType::Texture2D: return "Texture2D";
            case AssetType::Cubemap: return "Cubemap";
            case AssetType::Mesh: return "Mesh";
            case AssetType::Material: return "Material";
            case AssetType::Scene: return "Scene";
            case AssetType::Font: return "Font";
            default: return "Unknown";
        }
    }
}
