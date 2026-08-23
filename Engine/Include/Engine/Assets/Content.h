#pragma once

#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Renderer/Materials/PBRMaterial.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace Engine::Assets {

class AssetManager;
struct TextureAsset;
struct TextAsset;

/**
 * High-level content facade for game code.
 *
 * Content owns the asset cache and registers the engine's standard loaders.
 * Callers receive immutable shared values and do not need to know about
 * AssetType, AssetHandle or loader registration.
 */
class Content final {
public:
    using ErrorHandler = std::function<void(const std::string&)>;

    explicit Content(std::filesystem::path assetRoot = {});
    ~Content();

    Content(const Content&) = delete;
    Content& operator=(const Content&) = delete;

    [[nodiscard]] std::shared_ptr<const Mesh> mesh(std::filesystem::path path) const;
    [[nodiscard]] std::shared_ptr<const PBRMaterial> material(std::filesystem::path path) const;
    [[nodiscard]] std::shared_ptr<const TextureAsset> texture(std::filesystem::path path) const;
    [[nodiscard]] std::shared_ptr<const TextAsset> text(std::filesystem::path path) const;

    void setAssetRoot(std::filesystem::path root);
    [[nodiscard]] const std::filesystem::path& assetRoot() const noexcept;
    void setErrorHandler(ErrorHandler handler);

    /// Releases cached values that are no longer used by the application.
    void unloadUnused();
    /// Clears the complete content cache.
    void clear();

private:
    std::unique_ptr<AssetManager> manager_;
};

} // namespace Engine::Assets
