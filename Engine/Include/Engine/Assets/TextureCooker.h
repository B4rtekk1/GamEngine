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
        /** Aggregate wall-clock durations for successfully processed source textures. */
        std::uint64_t decodeMilliseconds{};
        std::uint64_t mipGenerationMilliseconds{};
        std::uint64_t bc7EncodeMilliseconds{};
        std::uint64_t saveMilliseconds{};
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

    /** Imports every GLB/glTF below @p assetRoot into a sibling .gmesh plus .gtex files. */
    [[nodiscard]] TextureCookSummary cook_all_gltf_meshes(const std::filesystem::path& assetRoot,
                                                           TextureCookProgress* progress = nullptr);
}
