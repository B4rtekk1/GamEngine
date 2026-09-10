#pragma once

#include "Engine/Assets/AssetTypes.h"

#include <filesystem>

namespace Engine::Assets {

    /** Reads a version-1 GTEX container and validates every mip range. */
    [[nodiscard]] std::optional<CookedTexture> load_gtex(const std::filesystem::path& path);

    /** Writes a version-1 GTEX container. Intended for the editor/cooker only. */
    bool save_gtex(const std::filesystem::path& path, const CookedTexture& texture);

    /** Checks dimensions, block sizes and ranges before an upload or write. */
    [[nodiscard]] bool valid_cooked_texture(const CookedTexture& texture) noexcept;
}
