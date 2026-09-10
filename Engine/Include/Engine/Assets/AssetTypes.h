#pragma once

/**
 * @file AssetTypes.h
 * @brief Defines asset identifiers, classifications and common asset data.
 */

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine::Assets {
    /** @brief Stable identifier generated from a normalized asset path. */
    using AssetId = std::uint64_t;

    /** @brief Supported asset categories. */
    enum class AssetType : std::uint8_t {
        Unknown,
        Binary,
        Text,
        Shader,
        Texture2D,
        Cubemap, //NOLINT
        Mesh,
        Material,
        Scene,
        Font,
    };

    /**
     * @brief Metadata describing the source file of an asset.
     */
    struct AssetMetadata {
        /// Identifier generated for the asset.
        AssetId id{};
        /// Category assigned to the asset.
        AssetType type{AssetType::Unknown};
        /// Absolute or resolved path of the source file.
        std::filesystem::path source_path;
        /// Last recorded modification time of the source file.
        std::filesystem::file_time_type last_write_time;
        /// Source-file size in bytes.
        std::uintmax_t source_size{};
    };

    /** @brief Raw binary contents of an asset file. */
    struct BinaryAsset {
        /// Bytes read from the source file.
        std::vector<std::byte> bytes;
    };

    /** CPU-side decoded RGBA texture. The renderer uploads it when needed. */
    enum class TextureFormat : std::uint32_t {
        RGBA8_SRGB,
        RGBA8_UNORM,
        BC4_UNORM,
        BC5_UNORM,
        BC7_UNORM,
        BC7_SRGB,
    };

    /** Byte range and dimensions of one cooked mip level. */
    struct TextureMip {
        std::uint64_t offset{};
        std::uint64_t size{};
        std::uint32_t width{};
        std::uint32_t height{};
    };

    /** GPU-ready texture data. Blocks are never decompressed by the runtime. */
    struct CookedTexture {
        std::uint32_t width{};
        std::uint32_t height{};
        TextureFormat format{TextureFormat::RGBA8_UNORM};
        std::vector<TextureMip> mips;
        std::vector<std::uint8_t> data;
    };

    /** Source or GPU-ready texture data. Prefer cooked when it is present. */
    struct TextureAsset {
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::uint8_t> rgbaPixels;
        std::optional<CookedTexture> cooked;
    };

    /** @brief Text contents of an asset file. */
    struct TextAsset {
        /// Text read from the source file.
        std::string text;
    };

    /** @brief Source code and entry point information for a shader asset. */
    struct ShaderAsset {
        /// Shader source code.
        std::string source;
        /// Shader entry-point function name.
        std::string entry_point{"main"};
    };

    /**
     * @brief Converts an asset type to its readable name.
     * @param type Asset type to convert.
     * @return Null-terminated name of the asset type.
     */
    constexpr std::string_view to_string(AssetType type) noexcept {
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
