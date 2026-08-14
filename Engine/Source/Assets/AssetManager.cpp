#include "Engine/Assets/AssetManager.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace Engine::Assets {

AssetManager::AssetManager(std::filesystem::path asset_root) : asset_root_(std::move(asset_root)) {}

void AssetManager::set_asset_root(std::filesystem::path root) {
    std::scoped_lock lock(mutex_);
    asset_root_ = std::move(root);
}

void AssetManager::set_error_handler(ErrorHandler handler) {
    std::scoped_lock lock(mutex_);
    error_handler_ = std::move(handler);
}

std::filesystem::path AssetManager::resolve(const std::filesystem::path& path) const {
    if (path.is_absolute() || asset_root_.empty()) return path.lexically_normal();
    return (asset_root_ / path).lexically_normal();
}

std::string AssetManager::normalize_path(const std::filesystem::path& path) {
    auto result = path.lexically_normal().generic_string();
    std::ranges::transform(result, result.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string AssetManager::make_key(const std::filesystem::path& path) const {
    return normalize_path(resolve(path));
}

AssetId AssetManager::make_id(std::string_view value) noexcept {
    // FNV-1a is stable between runs and platforms, unlike std::hash.
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto c : value) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 1099511628211ull;
    }
    return hash == 0 ? 1 : hash;
}

std::size_t AssetManager::CacheKeyHash::operator()(const CacheKey& key) const noexcept {
    const auto a = std::hash<AssetId>{}(key.id);
    const auto b = key.type.hash_code();
    return a ^ (b + 0x9e3779b9u + (a << 6u) + (a >> 2u));
}

void AssetManager::report(const std::string& message) const {
    ErrorHandler handler;
    {
        std::scoped_lock lock(mutex_);
        handler = error_handler_;
    }
    if (handler) handler(message);
}

void AssetManager::unload_unused() {
    std::scoped_lock lock(mutex_);
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it->second.value.use_count() == 1) it = cache_.erase(it);
        else ++it;
    }
}

void AssetManager::clear() {
    std::scoped_lock lock(mutex_);
    cache_.clear();
}

bool AssetManager::contains(AssetId id, std::type_index type) const {
    std::scoped_lock lock(mutex_);
    return cache_.contains(CacheKey{id, type});
}

std::size_t AssetManager::size() const {
    std::scoped_lock lock(mutex_);
    return cache_.size();
}

void register_default_asset_loaders(AssetManager& manager) {
    const auto text_loader = [](const std::filesystem::path& path, const AssetMetadata&) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::shared_ptr<const TextAsset>{};
        std::ostringstream stream;
        stream << file.rdbuf();
        return std::make_shared<const TextAsset>(TextAsset{stream.str()});
    };

    const auto binary_loader = [](const std::filesystem::path& path, const AssetMetadata&) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return std::shared_ptr<const BinaryAsset>{};
        const auto size = file.tellg();
        if (size < 0) return std::shared_ptr<const BinaryAsset>{};
        BinaryAsset asset;
        asset.bytes.resize(static_cast<std::size_t>(size));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(asset.bytes.data()), size);
        return std::make_shared<const BinaryAsset>(std::move(asset));
    };

    manager.register_loader<TextAsset>(AssetType::Text, text_loader);
    manager.register_loader<ShaderAsset>(AssetType::Shader, [text_loader](const auto& path, const auto& metadata) {
        auto text = text_loader(path, metadata);
        if (!text) return std::shared_ptr<const ShaderAsset>{};
        return std::make_shared<const ShaderAsset>(ShaderAsset{text->text, "main"});
    });
    manager.register_loader<BinaryAsset>(AssetType::Binary, binary_loader);
}

}