#pragma once

#include "Engine/Assets/AssetManager.h"

#include <filesystem>
#include <functional>
#include <memory>

namespace Engine::Assets {

/**
 * High-level content facade for game code.
 *
 * Content owns the asset cache and registers the engine's standard loaders.
 * Callers receive immutable shared values and do not need to know about
 * AssetType, AssetHandle or loader registration.
 */
class Content final {
public:
    explicit Content(std::filesystem::path assetRoot = {});

    Content(const Content&) = delete;
    Content& operator=(const Content&) = delete;

    [[nodiscard]] std::shared_ptr<const Mesh> mesh(std::filesystem::path path) const;
    [[nodiscard]] std::shared_ptr<const PBRMaterial> material(std::filesystem::path path) const;
    [[nodiscard]] std::shared_ptr<const TextureAsset> texture(std::filesystem::path path) const;
    [[nodiscard]] std::shared_ptr<const TextAsset> text(std::filesystem::path path) const;

    void setAssetRoot(std::filesystem::path root);
    [[nodiscard]] const std::filesystem::path& assetRoot() const noexcept;
    void setErrorHandler(AssetManager::ErrorHandler handler);

    /// Releases cached values that are no longer used by the application.
    void unloadUnused();
    /// Clears the complete content cache.
    void clear();

private:
    mutable AssetManager manager_;
};

} // namespace Engine::Assets
