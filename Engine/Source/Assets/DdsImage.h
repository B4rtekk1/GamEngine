#pragma once

#include "Engine/Assets/AssetTypes.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace Engine::Assets {
/** Block compression of a DDS file's payload. */
enum class DdsBlockFormat : std::uint8_t { BC1, BC3, BC5 };

/** Header of a block-compressed DDS: its payload can be uploaded as is. */
struct DdsBlockInfo {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t mipCount{};
    DdsBlockFormat format{};
    /// Whether mipCount reaches 1x1 (the chain .gtex and the GPU sample).
    bool completeMipChain{};
};

/** Reads only the header; nothing for files that are not BC1/BC3/BC5 DDS. */
[[nodiscard]] std::optional<DdsBlockInfo> read_dds_block_info(const std::filesystem::path& path);

/**
 * Copies the compressed mip chain of a BC1/BC3/BC5 DDS unchanged into a
 * cooked texture of @p format, which must hold the same blocks (BC1_*,
 * BC3_* or BC5_UNORM). Nothing when the file is truncated or incomplete.
 */
[[nodiscard]] std::optional<CookedTexture> read_dds_blocks(const std::filesystem::path& path, TextureFormat format);

/** Decode the top mip of a BC1, BC3, or BC5 DDS into RGBA8. */
[[nodiscard]] bool decode_dds_image(const std::filesystem::path& path, std::vector<std::uint8_t>& rgba,
                                    std::uint32_t& width, std::uint32_t& height);
} // namespace Engine::Assets
