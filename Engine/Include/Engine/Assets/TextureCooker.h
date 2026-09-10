#pragma once

#include "Engine/Assets/AssetTypes.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace Engine::Assets {
    /** Generates an RGBA mip chain and encodes every level as BC7. */
    [[nodiscard]] CookedTexture cook_bc7(
        std::span<const std::uint8_t> rgbaPixels,
        std::uint32_t width,
        std::uint32_t height,
        bool srgb);

    struct TextureCookSummary {
        std::uint32_t discovered{};
        std::uint32_t cooked{};
        std::uint32_t skipped{};
        std::uint32_t failed{};
        std::string errors;
    };

    /** Thread-safe progress counters for a background texture-cook job. */
    struct TextureCookProgress final {
        std::atomic<std::uint32_t> discovered{};
        std::atomic<std::uint32_t> completed{};
    };

    /** Cooks every supported source image below @p assetRoot to a sibling .gtex file. */
    [[nodiscard]] TextureCookSummary cook_all_textures(const std::filesystem::path& assetRoot,
                                                        TextureCookProgress* progress = nullptr);
}
