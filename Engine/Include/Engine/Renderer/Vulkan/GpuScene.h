#pragma once

#include "Engine/Renderer/Vulkan/buffer.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace Engine {
    /** A GPU virtual address.  Zero is the only null value. */
    struct GpuAddress final {
        VkDeviceAddress value{};

        [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
        [[nodiscard]] constexpr GpuAddress offset(const VkDeviceSize bytes) const noexcept {
            return {value + bytes};
        }
        [[nodiscard]] constexpr explicit operator VkDeviceAddress() const noexcept { return value; }
    };
    static_assert(sizeof(GpuAddress) == sizeof(VkDeviceAddress));

    /**
     * Typed, owning BDA buffer. T must describe the GPU layout exactly
     * (including alignas padding needed by the shader ABI).
     */
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    class GpuBuffer final {
    public:
        void createDeviceLocal(const VkDevice device, const std::size_t elementCapacity,
                               const VkBufferUsageFlags usage, const VmaAllocator allocator) {
            buffer_.createDeviceLocalEmpty(device, byteSize(elementCapacity), usage, allocator, true);
        }

        void createHostVisible(const VkPhysicalDevice physicalDevice, const VkDevice device,
                               const std::size_t elementCapacity, const VkBufferUsageFlags usage,
                               const VmaAllocator allocator) {
            buffer_.createHostVisible(physicalDevice, device, byteSize(elementCapacity), usage, allocator, true);
        }

        void destroy() noexcept { buffer_.destroy(); }
        void update(const T* data, const std::size_t elementCount, const std::size_t firstElement = 0) const {
            buffer_.update(data, byteSize(elementCount), byteSize(firstElement));
        }

        [[nodiscard]] GpuAddress address(const std::size_t firstElement = 0) const {
            const auto byteOffset = byteSize(firstElement);
            const GpuAddress result{buffer_.deviceAddress() + byteOffset};
            if (!result.valid() || result.value % alignof(T) != 0) {
                throw std::runtime_error("GPU buffer address does not satisfy the element alignment");
            }
            return result;
        }
        [[nodiscard]] VkBuffer handle() const noexcept { return buffer_.handle(); }
        [[nodiscard]] std::size_t capacity() const noexcept { return buffer_.size() / sizeof(T); }
        [[nodiscard]] Buffer& raw() noexcept { return buffer_; }
        [[nodiscard]] const Buffer& raw() const noexcept { return buffer_; }

    private:
        [[nodiscard]] static VkDeviceSize byteSize(const std::size_t elements) {
            if (elements == 0 || elements > std::numeric_limits<VkDeviceSize>::max() / sizeof(T)) {
                throw std::invalid_argument("GPU buffer requires a non-zero representable element count");
            }
            return static_cast<VkDeviceSize>(elements * sizeof(T));
        }
        Buffer buffer_;
    };

    /** A typed BDA buffer paired with its logical element count. */
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    class GpuArray final {
    public:
        void createDeviceLocal(const VkDevice device, const std::size_t count,
                               const VkBufferUsageFlags usage, const VmaAllocator allocator) {
            storage_.createDeviceLocal(device, count, usage, allocator);
            count_ = count;
        }
        void createHostVisible(const VkPhysicalDevice physicalDevice, const VkDevice device,
                               const std::size_t count, const VkBufferUsageFlags usage,
                               const VmaAllocator allocator) {
            storage_.createHostVisible(physicalDevice, device, count, usage, allocator);
            count_ = count;
        }
        void destroy() noexcept { storage_.destroy(); count_ = 0; }
        void update(const T* data, const std::size_t count, const std::size_t firstElement = 0) const {
            if (firstElement > count_ || count > count_ - firstElement) {
                throw std::out_of_range("GPU array update exceeds its logical size");
            }
            storage_.update(data, count, firstElement);
        }
        [[nodiscard]] GpuAddress address() const { return storage_.address(); }
        [[nodiscard]] std::size_t size() const noexcept { return count_; }
        [[nodiscard]] GpuBuffer<T>& storage() noexcept { return storage_; }
        [[nodiscard]] const GpuBuffer<T>& storage() const noexcept { return storage_; }
    private:
        GpuBuffer<T> storage_;
        std::size_t count_{};
    };

    /** Root record intended for a push constant or a tiny root buffer. */
    struct alignas(16) GpuScene final {
        GpuAddress objects;
        GpuAddress materials;
        GpuAddress meshes;
        GpuAddress meshlets;
        GpuAddress transforms;
        std::uint32_t objectCount{};
        std::uint32_t materialCount{};
        std::uint32_t meshCount{};
        std::uint32_t meshletCount{};
        std::uint32_t transformCount{};
        std::uint32_t reserved{};
    };
    static_assert(sizeof(GpuScene) == 64);

    /** Mesh table record with direct GPU links to its geometry. */
    struct alignas(16) GpuMesh final {
        GpuAddress vertices;
        GpuAddress indices;
        GpuAddress meshlets;
        std::uint32_t indexCount{};
        std::uint32_t meshletCount{};
        // Explicit padding keeps the C++ and shader-side record at 48 bytes
        // without relying on compiler-inserted tail padding.
        std::uint32_t reserved[4]{};
    };
    static_assert(sizeof(GpuMesh) == 48);
} // namespace Engine
