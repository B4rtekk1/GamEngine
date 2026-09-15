#pragma once

/**
 * @file AssetManager.h
 * @brief Declares the thread-safe asset loading and caching manager.
 */

#include "AssetHandle.h"
#include "Engine/Renderer/Materials/PBRMaterial.h"
#include "Engine/Renderer/Geometry/Mesh.h"

#include <functional>
#include "Engine/Core/TaskScheduler.h"

#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>

namespace Engine::Assets {
    /**
     * @brief Loads, caches, reloads and unloads typed assets.
     *
     * Asset loaders are registered by asset type and C++ value type. Loaded
     * values are cached using a stable path-derived identifier and can be reused
     * through typed AssetHandle instances.
     */
    class AssetManager final {
    public:
        /** @brief Callback used to report loading and asset-management errors. */
        using ErrorHandler = std::function<void(const std::string &)>;

        /**
         * @brief Constructs an asset manager.
         * @param asset_root Root directory used to resolve relative asset paths.
         */
        explicit AssetManager(std::filesystem::path asset_root = {});

        /// Destroys the manager and releases its cached records.
        ~AssetManager();

        /// Copy construction is disabled because the manager owns synchronized state.
        AssetManager(const AssetManager &) = delete;

        /// Copy assignment is disabled because the manager owns synchronized state.
        AssetManager &operator=(const AssetManager &) = delete;

        /** @brief Sets the root directory used for relative asset paths. */
        void set_asset_root(std::filesystem::path root);

        /** @brief Returns the currently configured asset root directory. */
        [[nodiscard]] const std::filesystem::path &asset_root() const noexcept { return asset_root_; }

        /** @brief Sets the callback used to receive error messages. */
        void set_error_handler(ErrorHandler handler);

        /** Convenient typed loaders for application code. */
        [[nodiscard]] AssetHandle<::Engine::Mesh> loadMesh(const std::filesystem::path& path) {
            return load<::Engine::Mesh>(path, AssetType::Mesh);
        }

        [[nodiscard]] AssetHandle<TextureAsset> loadTexture(const std::filesystem::path& path) {
            return load<TextureAsset>(path, AssetType::Texture2D);
        }

        [[nodiscard]] AssetHandle<::Engine::Mesh> loadMeshAsync(const std::filesystem::path& path) {
            return load_async<::Engine::Mesh>(path, AssetType::Mesh);
        }

        [[nodiscard]] AssetHandle<TextureAsset> loadTextureAsync(const std::filesystem::path& path) {
            return load_async<TextureAsset>(path, AssetType::Texture2D);
        }

        /**
         * Streams one cooked texture mip. For `rock.png`, level 0 resolves to
         * `rock.mip0.png`; each request reads and decodes that level only.
         */
        [[nodiscard]] AssetHandle<TextureAsset> load_texture_mip_async(
            const std::filesystem::path& source, std::uint32_t mip_level) {
            return load_async<TextureAsset>(stream_level_path(source, "mip", mip_level), AssetType::Texture2D);
        }

        /**
         * Streams one cooked mesh LOD. For `scene.glb`, level 2 resolves to
         * `scene.lod2.glb`; each LOD owns independent geometry data.
         */
        [[nodiscard]] AssetHandle<::Engine::Mesh> load_mesh_lod_async(
            const std::filesystem::path& source, std::uint32_t lod_level) {
            return load_async<::Engine::Mesh>(stream_level_path(source, "lod", lod_level), AssetType::Mesh);
        }


        [[nodiscard]] AssetHandle<PBRMaterial> loadMaterial(const std::filesystem::path& path) {
            return load<PBRMaterial>(path, AssetType::Material);
        }

        [[nodiscard]] AssetHandle<TextAsset> loadText(const std::filesystem::path& path) {
            return load<TextAsset>(path, AssetType::Text);
        }

        /**
         * @brief Function type used to load one asset value.
         * @tparam T Type produced by the loader.
         */
        template<typename T>
        using Loader = std::function<std::shared_ptr<const T>(const std::filesystem::path &, const AssetMetadata &)>;

        /**
         * @brief Registers a loader for an asset and value-type combination.
         * @tparam T Type produced by @p loader.
         * @param type Asset category handled by the loader.
         * @param loader Function receiving the resolved path and asset metadata.
         */
        template<typename T>
        void register_loader(AssetType type, Loader<T> loader) {
            std::unique_lock lock(mutex_);
            loaders_[CacheKey{static_cast<AssetId>(type), std::type_index(typeid(T))}] = [loader = std::move(loader)](
                const std::filesystem::path &path, const AssetMetadata &metadata) {
                        return std::static_pointer_cast<const void>(loader(path, metadata));
                    };
        }

        /**
         * @brief Loads an asset, returning a cached value when available.
         * @tparam T Expected loaded asset type.
         * @param path Absolute or asset-root-relative source path.
         * @param type Asset category used to select the loader.
         * @return Typed handle to the loaded asset, or an empty handle on failure.
         */
        template<typename T>
        AssetHandle<T> load(const std::filesystem::path& path, AssetType type = AssetType::Unknown) {
            const auto key = make_key(path);
            const AssetId id = make_id(key); //NOLINT
            const auto type_index = std::type_index(typeid(T));

            {
                std::scoped_lock lock(mutex_);
                if (const auto it = cache_.find(CacheKey{id, type_index, key}); it != cache_.end()) { //NOLINT
                    return AssetHandle<T>(id, std::static_pointer_cast<const T>(it->second.value));
                }
            }

            const auto absolute_path = resolve(path);
            std::error_code filesystemError;
            if (!std::filesystem::is_regular_file(absolute_path, filesystemError)) {
                report("Asset does not exist: " + absolute_path.string());
                return {};
            }

            AssetMetadata metadata;
            metadata.id = id;
            metadata.type = type;
            metadata.source_path = absolute_path;
            metadata.last_write_time = std::filesystem::last_write_time(absolute_path, filesystemError);
            metadata.source_size = std::filesystem::file_size(absolute_path, filesystemError);
            if (filesystemError) {
                report("Could not read asset metadata: " + absolute_path.string());
                return {};
            }

            LoaderErased loader;
            bool loader_missing = false;
            {
                std::scoped_lock lock(mutex_);
                const auto it = loaders_.find(CacheKey{static_cast<AssetId>(type), type_index}); //NOLINT
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
                const CacheKey cacheKey{id, type_index, key};
                cache_[cacheKey] = Record{std::move(value), std::move(metadata)};
                return AssetHandle<T>(id, std::static_pointer_cast<const T>(cache_[cacheKey].value));
            }
        }

        /**
         * @brief Loads an asset again only when its source file changed.
         * @tparam T Expected loaded asset type.
         * @param path Absolute or asset-root-relative source path.
         * @param type Asset category used to select the loader.
         * @return Typed handle to the current asset, or an empty handle on failure.
         */
        template<typename T>
        AssetHandle<T> load_if_changed(std::filesystem::path path, AssetType type = AssetType::Unknown) {
            const auto key = make_key(path);
            const AssetId id = make_id(key); //NOLINT
            const auto type_index = std::type_index(typeid(T));
            std::unique_lock lock(mutex_);
            const CacheKey cacheKey{id, type_index, key};
            const auto it = cache_.find(cacheKey); //NOLINT
            if (it != cache_.end()) {
                std::error_code error;
                const auto current = std::filesystem::last_write_time(resolve(path), error);
                if (error) {
                    lock.unlock();
                    report("Could not inspect asset: " + resolve(path).string());
                    return {};
                }
                if (current == it->second.metadata.last_write_time) {
                    auto value = std::static_pointer_cast<const T>(it->second.value);
                    lock.unlock();
                    return AssetHandle<T>(id, std::move(value));
                }
                // load() deliberately returns a cached value.  Remove the
                // stale record before leaving the lock so the load below
                // actually invokes the loader for the changed file.
                cache_.erase(it);
            }
            lock.unlock();
            return load<T>(std::move(path), type);
        }

        /** Starts CPU loading on a worker and immediately returns a pending handle. */
        template<typename T>
        AssetHandle<T> load_async(const std::filesystem::path& path, AssetType type = AssetType::Unknown) {
            const auto key = make_key(path);
            const AssetId id = make_id(key); //NOLINT
            const auto typeIndex = std::type_index(typeid(T));
            const CacheKey cacheKey{id, typeIndex, key};
            {
                std::scoped_lock lock(mutex_);
                if (const auto it = async_cache_.find(cacheKey); it != async_cache_.end())
                    return AssetHandle<T>(id, it->second);
            }

            const auto absolutePath = resolve(path);
            std::error_code error;
            if (!std::filesystem::is_regular_file(absolutePath, error)) {
                report("Asset does not exist: " + absolutePath.string()); return {};
            }
            AssetMetadata metadata{id, type, absolutePath,
                std::filesystem::last_write_time(absolutePath, error), std::filesystem::file_size(absolutePath, error)};
            if (error) { report("Could not read asset metadata: " + absolutePath.string()); return {}; }

            LoaderErased loader;
            auto slot = std::make_shared<AssetSlot>();
            bool loaderMissing = false;
            {
                std::scoped_lock lock(mutex_);
                if (const auto it = async_cache_.find(cacheKey); it != async_cache_.end())
                    return AssetHandle<T>(id, it->second);
                const auto loaderIt = loaders_.find(CacheKey{static_cast<AssetId>(type), typeIndex}); //NOLINT
                if (loaderIt == loaders_.end()) {
                    slot->state.store(AssetLoadState::Failed, std::memory_order_release);
                    loaderMissing = true;
                } else {
                    loader = loaderIt->second;
                    async_cache_.emplace(cacheKey, slot);
                    pending_.push_back(TaskScheduler::global().schedule([this, loader = std::move(loader), absolutePath, metadata, slot] {
                        std::shared_ptr<const void> value;
                        try {
                            value = loader(absolutePath, metadata);
                        } catch (const std::exception &exception) {
                            report("Loader threw for asset " + absolutePath.string() + ": " + exception.what());
                        } catch (...) {
                            report("Loader threw for asset: " + absolutePath.string());
                        }
                        {
                            std::scoped_lock slotLock(slot->mutex);
                            slot->value = std::move(value);
                            slot->state.store(slot->value ? AssetLoadState::Ready : AssetLoadState::Failed,
                                              std::memory_order_release);
                        }
                        if (slot->state.load(std::memory_order_acquire) == AssetLoadState::Failed)
                            report("Loader failed for asset: " + absolutePath.string());
                    }, TaskPriority::Low));
                }
            }
            if (loaderMissing)
                report("No loader registered for " + std::string(to_string(type)) + " and type " + typeid(T).name());
            return AssetHandle<T>(id, std::move(slot));
        }

        using GpuUploadJob = std::function<void()>;
        void enqueue_gpu_upload(GpuUploadJob job);
        [[nodiscard]] std::size_t process_gpu_uploads(
            std::size_t max_jobs = std::numeric_limits<std::size_t>::max());

        /** Reloads an asset unconditionally, replacing its cached value. */
        template<typename T>
        AssetHandle<T> reload(const std::filesystem::path& path, AssetType type = AssetType::Unknown) {
            const auto key = make_key(path);
            const AssetId id = make_id(key); //NOLINT
            unload<T>(id);
            return load<T>(path, type);
        }

        /**
         * @brief Removes one typed asset from the cache.
         * @tparam T Cached asset type.
         * @param id Identifier of the asset to unload.
         */
        template<typename T>
        void unload(AssetId id) { //NOLINT
            std::scoped_lock lock(mutex_);
            const auto type = std::type_index(typeid(T));
            std::erase_if(cache_, [id, type](const auto &entry) {
                return entry.first.id == id && entry.first.type == type;
            });
            std::erase_if(async_cache_, [id, type](const auto &entry) {
                return entry.first.id == id && entry.first.type == type;
            });
        }

        /// Removes cached records that are no longer externally referenced.
        void unload_unused();

        /// Removes every cached asset record.
        void clear();

        /** @brief Checks whether a typed asset is present in either cache. */
        [[nodiscard]] bool contains(AssetId id, std::type_index type) const; //NOLINT

        /** @brief Returns the number of cached asset records. */
        [[nodiscard]] std::size_t size() const;

        /**
         * @brief Generates an identifier from a normalized asset path.
         * @param normalized_path Normalized path used as the identifier source.
         * @return Stable hash-based asset identifier.
         */
        [[nodiscard]] static AssetId make_id(std::string_view normalized_path) noexcept;

        /**
         * @brief Normalizes a path for consistent lookup and identifier generation.
         * @param path Path to normalize.
         * @return Normalized path string.
         */
        [[nodiscard]] static std::string normalize_path(const std::filesystem::path &path);

    private:
        struct CacheKey {
            AssetId id;
            std::type_index type;
            std::string path;

            CacheKey(AssetId asset_id, std::type_index asset_type, std::string asset_path = {})
                : id(asset_id), type(asset_type), path(std::move(asset_path)) {
            }

            bool operator==(const CacheKey &other) const noexcept {
                return id == other.id && type == other.type && path == other.path;
            }
        };

        struct CacheKeyHash {
            std::size_t operator()(const CacheKey &key) const noexcept;
        };

        struct Record {
            std::shared_ptr<const void> value;
            AssetMetadata metadata;
        };

        using LoaderErased = std::function<std::shared_ptr<const void>(const std::filesystem::path &,
                                                                       const AssetMetadata &)>;

        [[nodiscard]] std::filesystem::path resolve(const std::filesystem::path &path) const;

        [[nodiscard]] std::string make_key(const std::filesystem::path &path) const;

        [[nodiscard]] static std::filesystem::path stream_level_path(
            const std::filesystem::path &source, std::string_view kind, std::uint32_t level);

        void report(const std::string &message) const;

        mutable std::mutex mutex_;
        std::filesystem::path asset_root_;
        ErrorHandler error_handler_;
        std::unordered_map<CacheKey, Record, CacheKeyHash> cache_;
        std::unordered_map<CacheKey, LoaderErased, CacheKeyHash> loaders_;
        std::unordered_map<CacheKey, std::shared_ptr<AssetSlot>, CacheKeyHash> async_cache_;
        std::deque<TaskHandle> pending_;
        std::deque<GpuUploadJob> gpu_uploads_;
    };

    /**
     * @brief Registers the engine's standard asset loaders.
     * @param manager Manager that receives the default loader registrations.
     */
    void register_default_asset_loaders(AssetManager &manager);
}
