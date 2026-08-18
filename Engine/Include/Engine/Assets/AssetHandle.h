#pragma once

/**
 * @file AssetHandle.h
 * @brief Defines a typed, shared handle to a loaded asset.
 */

#include "Engine/Assets/AssetTypes.h"

#include <memory>
#include <type_traits>

namespace Engine::Assets {
    /**
     * @brief Non-owning-style typed access wrapper for a loaded asset.
     *
     * The handle shares ownership of an immutable asset value through a
     * @c std::shared_ptr. An empty handle represents a failed or unavailable
     * asset load.
     *
     * @tparam T Asset value type.
     */
    template <typename T>
    class AssetHandle {
    public:
        /// Creates an empty handle.
        AssetHandle() = default;

        /**
         * @brief Creates a handle from an asset identifier and shared value.
         * @param id Stable asset identifier.
         * @param value Immutable loaded asset value.
         */
        AssetHandle(AssetId id, std::shared_ptr<const T> value) : id_(id), value_(std::move(value)) {}

        /** @brief Checks whether the handle refers to a loaded asset. */
        [[nodiscard]] explicit operator bool() const noexcept {return static_cast<bool>(value_); }
        /** @brief Returns the stable asset identifier. */
        [[nodiscard]] AssetId id() const noexcept { return id_; }
        /** @brief Returns a pointer to the asset, or nullptr when empty. */
        [[nodiscard]] const T* get() const noexcept {return value_.get(); }
        /** @brief Dereferences the asset value. The handle must be valid. */
        [[nodiscard]] const T& operator*() const noexcept {return *value_; }
        /** @brief Accesses a member of the asset value. The handle must be valid. */
        [[nodiscard]] const T* operator->() const noexcept {return value_.get(); }
        /** @brief Returns the shared immutable pointer held by the handle. */
        [[nodiscard]] std::shared_ptr<const T> shared() const noexcept {return value_; }
        /** @brief Clears the identifier and releases the shared asset value. */
        void reset() noexcept { id_ = {}; value_.reset(); }

    private:
        AssetId id_{};
        std::shared_ptr<const T> value_;
    };

    /** @brief Type trait that is false for general types. */
    template <typename T>
    struct is_asset_handle : std::false_type {};

    /** @brief Type trait specialization identifying an AssetHandle type. */
    template <typename T>
    struct is_asset_handle<AssetHandle<T>> : std::true_type {};
}