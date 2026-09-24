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

    /** Generates a mip chain and encodes it as BC4, BC5, or BC7. */
    [[nodiscard]] CookedTexture cook_texture(std::span<const std::uint8_t> rgbaPixels,
                                             std::uint32_t width, std::uint32_t height,
                                             TextureFormat format);

    struct TexturePhaseTimings {
        double sourceHashMilliseconds{};
        double decodeMilliseconds{};
        double mipGenerationMilliseconds{};
        double blockEncodeMilliseconds{};
        double writeMilliseconds{};
    };

    struct GltfPhaseTimings {
        double sourceScanMilliseconds{};
        double parseMilliseconds{};
        double geometryImportMilliseconds{};
        double meshletBuildMilliseconds{};
        double meshWriteMilliseconds{};
    };

    struct TextureCookSummary {
        std::uint32_t discovered{};
        std::uint32_t cooked{};
        std::uint32_t skipped{};
        std::uint32_t failed{};
        TexturePhaseTimings standalone;
        TexturePhaseTimings gltfTextures;
        GltfPhaseTimings gltf;
        std::string errors;
    };

    enum class TextureCookResult { Failed, Reused, Cooked };

    /** Filename-based fallback used when a source image has no material semantics. */
    [[nodiscard]] TextureFormat default_texture_format(const std::filesystem::path &source);

    /** Validates a source image's cooked sidecar against its current content and settings. */
    [[nodiscard]] bool current_source_texture(const std::filesystem::path &source,
                                              const std::filesystem::path &output, TextureFormat format);

    /** Resolves one external source image through the content-addressed texture cache. */
    [[nodiscard]] TextureCookResult cook_source_texture_cached(const std::filesystem::path &source,
                                                               const std::filesystem::path &output,
                                                               TextureFormat format,
                                                               const std::filesystem::path &cacheRoot,
                                                               TexturePhaseTimings *timings = nullptr);

    /** Resolves one embedded RGBA image through the same cache. */
    [[nodiscard]] TextureCookResult cook_image_texture_cached(std::span<const std::uint8_t> rgbaPixels,
                                                              std::uint32_t width, std::uint32_t height,
                                                              const std::filesystem::path &output,
                                                              TextureFormat format,
                                                              const std::filesystem::path &cacheRoot,
                                                              TexturePhaseTimings *timings = nullptr);

    /** Thread-safe progress counters for a background texture-cook job. */
    struct TextureCookProgress final {
        std::atomic<std::uint32_t> discovered;
        std::atomic<std::uint32_t> completed;
    };

    /** Cooks every supported source image below @p assetRoot to a sibling .gtex file. */
    [[nodiscard]] TextureCookSummary cook_all_textures(const std::filesystem::path &assetRoot,
                                                       TextureCookProgress *progress = nullptr);

    /** Imports every GLB/glTF below @p assetRoot into a sibling .gmesh, reusing external .gtex files. */
    [[nodiscard]] TextureCookSummary cook_all_gltf_meshes(const std::filesystem::path &assetRoot,
                                                          TextureCookProgress *progress = nullptr);
}
