#pragma once

#include "Engine/Renderer/Vulkan/shadow_map.h"
#include "Engine/Renderer/Vulkan/buffer.h"
#include "Engine/Math/Mat4.h"
#include "Engine/Renderer/Culling/CullingTypes.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <array>
#include <memory>
#include <span>
#include <vector>

namespace Engine {
    namespace Assets {
        class AssetManager;
    }

    class Mat4;

    namespace Culling {
        class GPUCullingPass;
        class IndexedIndirectDrawCount;
    }

    class ShadowPass final {
    public:
        ShadowPass() = default;

        ~ShadowPass();

        ShadowPass(const ShadowPass &) = delete;

        ShadowPass &operator=(const ShadowPass &) = delete;

        void create(VkPhysicalDevice physicalDevice, VkDevice device,
                    const std::vector<VkBuffer> &uniformBuffers,
                    const std::vector<VkBuffer> &materialBuffers,
                    const std::vector<VkDescriptorImageInfo> &materialTextures,
                    VkDeviceSize uniformBufferRange, VmaAllocator allocator,
                    Assets::AssetManager &assets);

        void destroy() noexcept;

        void record(VkCommandBuffer commandBuffer,
                    const std::array<Mat4, ShadowMap::ClipLevelCount> &clipMatrices,
                    std::uint32_t updateMask,
                    VkBuffer vertexBuffer, VkBuffer instanceBuffer, VkBuffer indexBuffer,
                    VkDescriptorSet sceneDescriptorSet,
                    const Culling::GPUCullingPass &cullingPass,
                    const Culling::IndexedIndirectDrawCount &indirectDraw,
                    std::uint32_t objectCount);

        void preparePages(
            const std::array<Mat4, ShadowMap::ClipLevelCount>& clipMatrices,
            const Mat4& cameraViewProjection,
            std::span<const Culling::GPUObjectData> objects,
            std::span<const Culling::GPUObjectData> dirtyObjects,
            std::uint32_t frameIndex);

        void invalidateCache() noexcept;

        [[nodiscard]] const std::array<std::uint32_t, ShadowMap::VirtualPageCount>&
        pageTable() const noexcept { return pageTable_; }

        [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const noexcept {
            return descriptorSetLayout_;
        }

        [[nodiscard]] VkDescriptorSet descriptorSet(std::uint32_t frameIndex) const;

    private:
        struct PhysicalPage {
            std::uint16_t virtualX{};
            std::uint16_t virtualY{};
            std::uint8_t level{};
            bool allocated{};
            std::uint64_t lastUsed{};
        };

        [[nodiscard]] static std::uint32_t virtualPageIndex(
            std::uint32_t level, std::uint32_t x, std::uint32_t y) noexcept;

        mutable bool atlasInitialized_{false};
        mutable bool atlasContentValid_{false};
        std::array<std::uint32_t, ShadowMap::VirtualPageCount> pageTable_{};
        std::array<PhysicalPage, ShadowMap::PhysicalPageCount> physicalPages_{};
        std::array<Mat4, ShadowMap::ClipLevelCount> cachedClipMatrices_{};
        std::array<bool, ShadowMap::ClipLevelCount> cachedClipMatricesValid_{};
        std::vector<std::uint32_t> pagesToRender_;
        std::uint64_t cacheClock_{};
        VkDevice device_{VK_NULL_HANDLE};
        ShadowMap shadowMap_;
        VkDescriptorSetLayout descriptorSetLayout_{VK_NULL_HANDLE};
        VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
        std::vector<VkDescriptorSet> descriptorSets_;
        std::vector<std::unique_ptr<Buffer>> pageTableBuffers_;
        VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
        VkPipeline pipeline_{VK_NULL_HANDLE};
    };
} // namespace Engine
