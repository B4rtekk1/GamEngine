#pragma once

#include "AssetHandle.h"

#include <functional>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>

namespace Engine::Assets {

class AssetManager final {
public:
    using ErrorHandler = std::function<void(const std::string&)>;

    explicit AssetManager(std::filesystem::path asset_root = {});
    ~AssetManager() = default;

    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    void set_asset_root(std::filesystem::path root);
    [[nodiscard]] const std::filesystem::path& asset_root() const noexcept { return asset_root_; }
    void set_error_handler(ErrorHandler handler);

    template <typename T>
    using Loader = std::function<std::shared_ptr<const T>(const std::filesystem::path&, const AssetMetadata&)>;

    template <typename T>
    void register_loader(AssetType type, Loader<T> loader) {
        std::unique_lock lock(mutex_);
        loaders_[CacheKey{static_cast<AssetId>(type), std::type_index(typeid(T))}] = [loader = std::move(loader)](
            const std::filesystem::path& path, const AssetMetadata& metadata) {
            return std::static_pointer_cast<const void>(loader(path, metadata));
        };
    }

    template <typename T>
    AssetHandle<T> load(std::filesystem::path path, AssetType type = AssetType::Unknown) {
        const auto key = make_key(path);
        const AssetId id = make_id(key);
        const auto type_index = std::type_index(typeid(T));

        {
            std::scoped_lock lock(mutex_);
            if (const auto it = cache_.find(CacheKey{id, type_index}); it != cache_.end()) {
                return AssetHandle<T>(id, std::static_pointer_cast<const T>(it->second.value));
            }
        }

        const auto absolute_path = resolve(path);
        if (!std::filesystem::is_regular_file(absolute_path)) {
            report("Asset does not exist: " + absolute_path.string());
            return {};
        }

        AssetMetadata metadata;
        metadata.id = id;
        metadata.type = type;
        metadata.source_path = absolute_path;
        metadata.last_write_time = std::filesystem::last_write_time(absolute_path);
        metadata.source_size = std::filesystem::file_size(absolute_path);

        LoaderErased loader;
        bool loader_missing = false;
        {
            std::scoped_lock lock(mutex_);
            const auto it = loaders_.find(CacheKey{static_cast<AssetId>(type), type_index});
            if (it == loaders_.end()) {
                loader_missing = true;
            } else {
                loader = it->second;
            }
        }
        if (loader_missing) {
            report("No loader registered for " + std::string(to_string(type)) +
                   " and type " + typeid(T).name());
            return {};
        }

        auto value = loader(absolute_path, metadata);
        if (!value) {
            report("Loader failed for asset: " + absolute_path.string());
            return {};
        }

        {
            std::scoped_lock lock(mutex_);
            cache_[CacheKey{id, type_index}] = Record{std::move(value), std::move(metadata)};
            return AssetHandle<T>(id, std::static_pointer_cast<const T>(cache_[CacheKey{id, type_index}].value));
        }
    }

    template <typename T>
    AssetHandle<T> load_if_changed(std::filesystem::path path, AssetType type = AssetType::Unknown) {
        const auto key = make_key(path);
        const AssetId id = make_id(key);
        const auto type_index = std::type_index(typeid(T));
        std::unique_lock lock(mutex_);
        const auto it = cache_.find(CacheKey{id, type_index});
        if (it != cache_.end()) {
            const auto current = std::filesystem::last_write_time(resolve(path));
            if (current == it->second.metadata.last_write_time) {
                auto value = std::static_pointer_cast<const T>(it->second.value);
                lock.unlock();
                return AssetHandle<T>(id, std::move(value));
            }
        }
        lock.unlock();
        return load<T>(std::move(path), type);
    }

    template <typename T>
    void unload(AssetId id) {
        std::scoped_lock lock(mutex_);
        cache_.erase(CacheKey{id, std::type_index(typeid(T))});
    }

    void unload_unused();
    void clear();
    [[nodiscard]] bool contains(AssetId id, std::type_index type) const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] static AssetId make_id(std::string_view normalized_path) noexcept;
    [[nodiscard]] static std::string normalize_path(const std::filesystem::path& path);

private:
    struct CacheKey {
        AssetId id;
        std::type_index type;
        CacheKey(AssetId asset_id, std::type_index asset_type) : id(asset_id), type(asset_type) {}
        bool operator==(const CacheKey& other) const noexcept {
            return id == other.id && type == other.type;
        }
    };
    struct CacheKeyHash {
        std::size_t operator()(const CacheKey& key) const noexcept;
    };
    struct Record {
        std::shared_ptr<const void> value;
        AssetMetadata metadata;
    };
    using LoaderErased = std::function<std::shared_ptr<const void>(const std::filesystem::path&, const AssetMetadata&)>;

    [[nodiscard]] std::filesystem::path resolve(const std::filesystem::path& path) const;
    [[nodiscard]] std::string make_key(const std::filesystem::path& path) const;
    void report(const std::string& message) const;

    mutable std::mutex mutex_;
    std::filesystem::path asset_root_;
    ErrorHandler error_handler_;
    std::unordered_map<CacheKey, Record, CacheKeyHash> cache_;
    std::unordered_map<CacheKey, LoaderErased, CacheKeyHash> loaders_;
};

void register_default_asset_loaders(AssetManager& manager);

}