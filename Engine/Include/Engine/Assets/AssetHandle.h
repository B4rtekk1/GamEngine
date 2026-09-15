#pragma once

/**
 * @file AssetHandle.h
 * @brief Defines a typed, shared handle to a loaded asset.
 */

#include "Engine/Assets/AssetTypes.h"

#include <memory>
#include <atomic>
#include <mutex>
#include <type_traits>

namespace Engine::Assets {
    enum class AssetLoadState : std::uint8_t { Pending, Ready, Failed };

    struct AssetSlot {
        mutable std::mutex mutex;
        std::shared_ptr<const void> value;
        std::atomic<AssetLoadState> state{AssetLoadState::Pending};
    };
    /**
     * @brief Non-owning-style typed access wrapper for a loaded asset.
     *
     * The handle shares ownership of an immutable asset value through a
     * @c std::shared_ptr. An empty handle represents a failed or unavailable
     * asset load.
     *
     * @tparam T Asset value type.
     */
    template<typename T>
    class AssetHandle {
    public:
        /// Creates an empty handle.
        AssetHandle() = default;

        /**
         * @brief Creates a handle from an asset identifier and shared value.
         * @param id Stable asset identifier.
         * @param value Immutable loaded asset value.
         */
        AssetHandle(AssetId id, std::shared_ptr<const T> value) : id_(id), slot_(std::make_shared<AssetSlot>()) { //NOLINT
            slot_->value = std::move(value);
            slot_->state.store(slot_->value ? AssetLoadState::Ready : AssetLoadState::Failed, std::memory_order_release);
        }
        AssetHandle(AssetId id, std::shared_ptr<AssetSlot> slot) : id_(id), slot_(std::move(slot)) {} //NOLINT

        /** @brief Checks whether the handle refers to a loaded asset. */
        [[nodiscard]] explicit operator bool() const noexcept { return is_ready(); }
        [[nodiscard]] bool is_ready() const noexcept { return slot_ && slot_->state.load(std::memory_order_acquire) == AssetLoadState::Ready; }
        [[nodiscard]] bool failed() const noexcept { return slot_ && slot_->state.load(std::memory_order_acquire) == AssetLoadState::Failed; }
        [[nodiscard]] AssetLoadState state() const noexcept { return slot_ ? slot_->state.load(std::memory_order_acquire) : AssetLoadState::Failed; }
        /** @brief Returns the stable asset identifier. */
        [[nodiscard]] AssetId id() const noexcept { return id_; }
        /** @brief Returns a pointer to the asset, or nullptr when empty. */
        [[nodiscard]] const T *get() const noexcept { return shared().get(); }
        /** @brief Dereferences the asset value. The handle must be valid. */
        [[nodiscard]] const T &operator*() const noexcept { return *get(); }
        /** @brief Accesses a member of the asset value. The handle must be valid. */
        [[nodiscard]] const T *operator->() const noexcept { return get(); }
        /** @brief Returns the shared immutable pointer held by the handle. */
        [[nodiscard]] std::shared_ptr<const T> shared() const noexcept {
            if (!slot_ || !is_ready()) return {};
            std::scoped_lock lock(slot_->mutex);
            return std::static_pointer_cast<const T>(slot_->value);
        }
        [[nodiscard]] bool IsReady() const noexcept { return is_ready(); }
        [[nodiscard]] const T *Get() const noexcept { return get(); }
        /** @brief Clears the identifier and releases the shared asset value. */
        void reset() noexcept {
            id_ = {};
            slot_.reset();
        }

    private:
        AssetId id_{};
        std::shared_ptr<AssetSlot> slot_;
    };

    /** @brief Type trait that is false for general types. */
    template<typename T>
    struct is_asset_handle : std::false_type {
    };

    /** @brief Type trait specialization identifying an AssetHandle type. */
    template<typename T>
    struct is_asset_handle<AssetHandle<T> > : std::true_type {
    };
}
