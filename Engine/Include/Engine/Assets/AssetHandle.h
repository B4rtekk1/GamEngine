#pragma once

#include "Engine/Assets/AssetTypes.h"

#include <memory>
#include <type_traits>

namespace Engine::Assets {
    template <typename T>
    class AssetHandle {
    public:
        AssetHandle() = default;
        AssetHandle(AssetId id, std::shared_ptr<const T> value) : id_(id), value_(std::move(value)) {}

        [[nodiscard]] explicit operator bool() const noexcept {return static_cast<bool>(value_); }
        [[nodiscard]] AssetId id() const noexcept { return id_; }
        [[nodiscard]] const T* get() const noexcept {return value_.get(); }
        [[nodiscard]] const T& operator*() const noexcept {return *value_; }
        [[nodiscard]] const T* operator->() const noexcept {return value_.get(); }
        [[nodiscard]] std::shared_ptr<const T> shared() const noexcept {return value_; }
        void reset() noexcept { id_ = {}; value_.reset(); }

    private:
        AssetId id_{};
        std::shared_ptr<const T> value_;
    };

    template <typename T>
    struct is_asset_handle : std::false_type {};

    template <typename T>
    struct is_asset_handle<AssetHandle<T>> : std::true_type {};
}