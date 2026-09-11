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
                    ShadowMap& physicalPagePool,
                    const std::vector<VkBuffer> &uniformBuffers,
                    const std::vector<VkBuffer> &materialBuffers,
                    const std::vector<VkBuffer> &instanceBuffers,
                    const std::vector<VkBuffer> &previousTransformBuffers,
                    const std::vector<VkBuffer> &instanceIndexBuffers,
                    const std::vector<VkBuffer> &grassInstanceBuffers,
                    const std::vector<VkBuffer> &grassClusterBuffers,
                    const std::vector<VkBuffer> &grassDeformationBuffers,
                    const std::vector<VkBuffer> &clusterRangeBuffers,
                    const std::vector<VkBuffer> &clusterIndexBuffers,
                    const std::vector<VkDescriptorImageInfo> &materialTextures,
                    VkDeviceSize uniformBufferRange, VmaAllocator allocator,
                    Assets::AssetManager &assets);

        // Rebind scene-owned buffers/textures without replacing the descriptor
        // set layout, shadow atlas or graphics pipelines. This is the normal
        // path for an ECS topology change.
        void updateDescriptors(const std::vector<VkBuffer> &uniformBuffers,
                               const std::vector<VkBuffer> &materialBuffers,
                               const std::vector<VkBuffer> &instanceBuffers,
                               const std::vector<VkBuffer> &previousTransformBuffers,
                               const std::vector<VkBuffer> &instanceIndexBuffers,
                               const std::vector<VkBuffer> &grassInstanceBuffers,
                               const std::vector<VkBuffer> &grassClusterBuffers,
                               const std::vector<VkBuffer> &grassDeformationBuffers,
                               const std::vector<VkBuffer> &clusterRangeBuffers,
                               const std::vector<VkBuffer> &clusterIndexBuffers,
                               const std::vector<VkDescriptorImageInfo> &materialTextures,
                               VkDeviceSize uniformBufferRange) const;

        void destroy() noexcept;

        void record(VkCommandBuffer commandBuffer,
                    const std::array<Mat4, ShadowMap::ClipLevelCount> &clipMatrices,
                    std::uint32_t updateMask,
                    VkBuffer vertexBuffer, VkBuffer instanceBuffer, VkBuffer indexBuffer,
                    VkDescriptorSet sceneDescriptorSet,
                    const Culling::GPUCullingPass &cullingPass,
                    const Culling::IndexedIndirectDrawCount &indirectDraw,
                    const Culling::GPUCullingPass &twoSidedCullingPass,
                    const Culling::IndexedIndirectDrawCount &twoSidedIndirectDraw,
                    std::uint32_t objectCount,
                    VkDescriptorSet grassDescriptorSet = VK_NULL_HANDLE,
                    const Culling::IndexedIndirectDrawCount *grassIndirectDraw = nullptr);

        void preparePages(
            const std::array<Mat4, ShadowMap::ClipLevelCount>& clipMatrices,
            const Mat4& cameraViewProjection,
            std::span<const Culling::GPUObjectData> objects,
            std::span<const Culling::GPUObjectData> dirtyObjects,
            // GPU-compacted page keys from the completed frame's depth pass.
            // An empty span is the one-frame bootstrap fallback only.
            std::span<const std::uint32_t> receiverPageRequests,
            std::uint32_t frameIndex,
            std::uint32_t pageUpdateBudget = ShadowMap::PhysicalPageCount);

        void invalidateCache() noexcept;

        [[nodiscard]] const std::array<std::uint32_t, ShadowMap::VirtualPageCount>&
        pageTable() const noexcept { return pageTable_; }

        [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const noexcept {
            return descriptorSetLayout_;
        }

        [[nodiscard]] VkDescriptorSet descriptorSet(std::uint32_t frameIndex) const;
        [[nodiscard]] VkDescriptorSet grassDescriptorSet(std::uint32_t frameIndex) const;
        [[nodiscard]] VkDescriptorSet grassVelocityDescriptorSet(std::uint32_t frameIndex) const;
        [[nodiscard]] VkDescriptorSet grassShadowDescriptorSet(std::uint32_t frameIndex) const;
        void setGrassVisibleInstances(std::uint32_t frameIndex, VkBuffer visibleInstances) const;
        void setGrassVelocityVisibleInstances(std::uint32_t frameIndex, VkBuffer visibleInstances) const;
        void setGrassShadowVisibleInstances(std::uint32_t frameIndex, VkBuffer visibleInstances) const;

        // Packed grass reserves bindings 3/7 for cluster/deformation data.
        // this explicit prevents a future grass-only descriptor set from
        // silently using the generic seven-binding contract.
        static constexpr std::uint32_t GrassClusterBinding = 3;
        static constexpr std::uint32_t GrassDeformationBinding = 7;

    private:
        struct PhysicalPage {
            std::uint16_t virtualX{};
            std::uint16_t virtualY{};
            std::uint8_t level{};
            bool allocated{};
            bool dirty{};
            std::uint64_t lastUsed{};
        };

        // A mapping is intentionally pending while its physical tile is
        // being cleared and rendered. Publishing it earlier would make the
        // sampler resolve a new virtual page to depth left by the previous
        // occupant of that tile.
        struct PendingPageCommit {
            std::uint32_t virtualPage{ShadowMap::InvalidPage};
            std::uint32_t physicalPage{ShadowMap::InvalidPage};
            std::uint32_t evictedVirtualPage{ShadowMap::InvalidPage};
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
        std::vector<PendingPageCommit> pendingPageCommits_;
        std::uint32_t preparedFrameIndex_{};
        std::uint64_t cacheClock_{};
        VkDevice device_{VK_NULL_HANDLE};
        // The two view contexts keep independent virtual page tables, but
        // render into the renderer-owned physical atlas.
        ShadowMap* shadowMap_{nullptr};
        VkDescriptorSetLayout descriptorSetLayout_{VK_NULL_HANDLE};
        VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
        std::vector<VkDescriptorSet> descriptorSets_;
        std::vector<VkDescriptorSet> grassDescriptorSets_;
        std::vector<VkDescriptorSet> grassVelocityDescriptorSets_;
        std::vector<VkDescriptorSet> grassShadowDescriptorSets_;
        std::vector<std::unique_ptr<Buffer>> pageTableBuffers_;
        VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
        VkPipeline pipeline_{VK_NULL_HANDLE};
        VkPipeline twoSidedPipeline_{VK_NULL_HANDLE};
        VkPipeline grassPipeline_{VK_NULL_HANDLE};
    };
} // namespace Engine
