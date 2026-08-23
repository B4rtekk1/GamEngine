#include "Engine/Assets/Content.h"
#include "Engine/Assets/AssetManager.h"

namespace Engine::Assets {

Content::Content(std::filesystem::path assetRoot)
    : manager_(std::make_unique<AssetManager>(std::move(assetRoot))) {
    register_default_asset_loaders(*manager_);
}

Content::~Content() = default;

std::shared_ptr<const Mesh> Content::mesh(std::filesystem::path path) const {
    return manager_->loadMesh(std::move(path)).shared();
}

std::shared_ptr<const PBRMaterial> Content::material(std::filesystem::path path) const {
    return manager_->loadMaterial(std::move(path)).shared();
}

std::shared_ptr<const TextureAsset> Content::texture(std::filesystem::path path) const {
    return manager_->loadTexture(std::move(path)).shared();
}

std::shared_ptr<const TextAsset> Content::text(std::filesystem::path path) const {
    return manager_->loadText(std::move(path)).shared();
}

void Content::setAssetRoot(std::filesystem::path root) {
    manager_->set_asset_root(std::move(root));
}

const std::filesystem::path& Content::assetRoot() const noexcept {
    return manager_->asset_root();
}

void Content::setErrorHandler(ErrorHandler handler) {
    manager_->set_error_handler(std::move(handler));
}

void Content::unloadUnused() {
    manager_->unload_unused();
}

void Content::clear() {
    manager_->clear();
}

} // namespace Engine::Assets
