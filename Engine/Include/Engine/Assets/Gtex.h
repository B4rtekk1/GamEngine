#pragma once

#include "Engine/Assets/AssetTypes.h"

#include <filesystem>
#include <span>

namespace Engine::Assets {

    /** Reads only a version-1 GTEX header and mip table; payload remains on disk. */
    [[nodiscard]] std::optional<GtexTexture> load_gtex(const std::filesystem::path& path);

    /** Reads part of one mip straight into caller-owned memory (normally the upload ring). */
    [[nodiscard]] bool readMipRange(const GtexTexture& texture, std::uint32_t mip,
                                    std::uint64_t offset, std::span<std::uint8_t> destination);

    /** Writes a version-1 GTEX container. Intended for the editor/cooker only. */
    bool save_gtex(const std::filesystem::path& path, const CookedTexture& texture);

    /** Checks dimensions, block sizes and ranges before an upload or write. */
    [[nodiscard]] bool valid_cooked_texture(const CookedTexture& texture) noexcept;
}
