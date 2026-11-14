#pragma once

#include "Engine/Math/Mat4.h"
#include "Engine/Renderer/Culling/CullingTypes.h"
#include "Engine/Renderer/Vulkan/buffer.h"
#include "Engine/Renderer/Vulkan/shadow_map.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets {
class AssetManager;
}

class Mat4;

namespace Culling {
class GPUCullingPass;
class IndexedIndirectDrawCount;
} // namespace Culling

class GpuTimestampProfiler;

class ShadowPass final {
  public:
    /**
     * Value stored in a dirty object's localAabbMin.w when its bounds move
     * every frame without an ECS edit (wind-driven grass). Such bounds always
     * invalidate clip level 0, but coarser levels only every 2/4/8 frames:
     * their texels are large enough that per-frame sway is barely visible,
     * and they cover most of the cached pages.
     */
    static constexpr float AnimatedDirtyBoundsFlag = 1.0F;

    ShadowPass() {
        invalidateCache();
    }

    ~ShadowPass();

    ShadowPass(const ShadowPass&) = delete;

    ShadowPass& operator=(const ShadowPass&) = delete;

    void create(VkPhysicalDevice physicalDevice, VkDevice device, ShadowMap& physicalPagePool,
                const std::vector<VkBuffer>& uniformBuffers, const std::vector<VkBuffer>& materialBuffers,
                const std::vector<VkBuffer>& instanceBuffers, const std::vector<VkBuffer>& previousTransformBuffers,
                const std::vector<VkBuffer>& instanceIndexBuffers, const std::vector<VkBuffer>& grassInstanceBuffers,
                const std::vector<VkBuffer>& grassClusterBuffers, const std::vector<VkBuffer>& grassDeformationBuffers,
                const std::vector<VkBuffer>& clusterRangeBuffers, const std::vector<VkBuffer>& clusterIndexBuffers,
                const std::vector<VkBuffer>& reflectionProbeBuffers,
                const std::vector<VkDescriptorImageInfo>& materialTextures,
                const std::array<VkDescriptorImageInfo, 3>& imageBasedLighting, VkDeviceSize uniformBufferRange,
                VmaAllocator allocator, Assets::AssetManager& assets);

    // Rebind scene-owned buffers/textures without replacing the descriptor
    // set layout, shadow atlas or graphics pipelines. This is the normal
    // path for an ECS topology change.
    void updateDescriptors(
        const std::vector<VkBuffer>& uniformBuffers, const std::vector<VkBuffer>& materialBuffers,
        const std::vector<VkBuffer>& instanceBuffers, const std::vector<VkBuffer>& previousTransformBuffers,
        const std::vector<VkBuffer>& instanceIndexBuffers, const std::vector<VkBuffer>& grassInstanceBuffers,
        const std::vector<VkBuffer>& grassClusterBuffers, const std::vector<VkBuffer>& grassDeformationBuffers,
        const std::vector<VkBuffer>& clusterRangeBuffers, const std::vector<VkBuffer>& clusterIndexBuffers,
        const std::vector<VkBuffer>& reflectionProbeBuffers, const std::vector<VkDescriptorImageInfo>& materialTextures,
        const std::array<VkDescriptorImageInfo, 3>& imageBasedLighting, VkDeviceSize uniformBufferRange) const;

    void updateImageBasedLightingDescriptors(const std::array<VkDescriptorImageInfo, 3>& imageBasedLighting) const;

    /** Replaces one local-probe fallback descriptor in every forward set
     * for a frame. The image must remain alive until that frame fence. */
    void setReflectionProbeTexture(std::uint32_t frameIndex, std::uint32_t textureIndex,
                                   const VkDescriptorImageInfo& texture) const;

    /** Binds the full-resolution GTAO visibility texture for one safe frame slot. */
    void setGtaoTexture(std::uint32_t frameIndex, const VkDescriptorImageInfo& texture) const;
    void setContactShadowTexture(std::uint32_t frameIndex, const VkDescriptorImageInfo& texture) const;
    void setSsgiTexture(std::uint32_t frameIndex, const VkDescriptorImageInfo& texture) const;
    /** Binds the frame slot's buffer of every point and spot light (LocalLightGPU records). */
    void setLocalLights(std::uint32_t frameIndex, VkBuffer lights) const;
    /** Game View's depth-bounded tile light lists (TileLightCullingPass). */
    void setTileLights(std::uint32_t frameIndex, VkBuffer lists) const;
    /** Binds the frame slot's LocalLightShadowPass mask (sampled as uint2). */
    void setLocalShadowMask(std::uint32_t frameIndex, const VkDescriptorImageInfo& mask) const;
    void setDdgiResources(std::uint32_t frameIndex, const VkDescriptorImageInfo& irradiance,
                          const VkDescriptorImageInfo& distance, VkBuffer volume, VkDeviceSize volumeSize) const;
    /** Adds the ray generation stage to the bindings DDGI shades with. Call before create(). */
    void setRayTracingStages(bool enabled) noexcept {
        rayTracingStages_ = enabled;
    }
    /** Adds the mesh stage to every binding the vertex stage reads, for the
     *  mesh-shader forward pipelines. Call before create(). */
    void setMeshShaderStages(bool enabled) noexcept {
        meshShaderStages_ = enabled;
    }

    /** Set 1 of the mesh-shader caster pipeline (Common/MeshletDraw.slang's
     *  layout); null keeps meshlet casters on indexed draws. Call before create(). */
    void setMeshletDrawLayout(VkDescriptorSetLayout layout) noexcept {
        meshletDrawLayout_ = layout;
    }

    void destroy() noexcept;

    /// Meshlet draws one VSM page may emit; objects beyond it draw whole.
    static constexpr std::uint32_t MeshletDrawsPerPage = 4096;
    /** Workgroups sharing one page's object list in the meshlet refine (shader ObjectSlices). */
    static constexpr std::uint32_t MeshletRefineObjectSlices = 16;

    /**
     * Per-meshlet caster refinement of one VSM stream (one-sided, or the
     * two-sided foliage stream). Sections with meshlets are culled per
     * meshlet against each page and drawn from the expanded meshlet index
     * buffer, or with mesh tasks, instead of whole.
     */
    struct MeshletStream final {
        VkPipeline refinePipeline = VK_NULL_HANDLE;
        VkPipelineLayout refineLayout = VK_NULL_HANDLE;
        VkDescriptorSet refineSet = VK_NULL_HANDLE;
        VkBuffer objectCounts = VK_NULL_HANDLE;
        VkBuffer commands = VK_NULL_HANDLE;
        VkBuffer commandCounts = VK_NULL_HANDLE;
        /// Index buffer the meshlet commands index (the shared index heap).
        VkBuffer meshletIndexBuffer = VK_NULL_HANDLE;
        std::uint32_t passCapacity = 0;
        std::uint32_t meshletCount = 0;
        // Mesh-shader casters (Shadow/shadow_meshlet_mesh.slang); all four
        // set, the stream lists meshlets and draws them with mesh tasks.
        /// shadow_meshlet_refine with SHADOW_MESH_TASKS.
        VkPipeline meshRefinePipeline = VK_NULL_HANDLE;
        /// shadow_meshlet_build_mesh_commands, on refineLayout and refineSet.
        VkPipeline meshCommandPipeline = VK_NULL_HANDLE;
        /// One VkDrawMeshTasksIndirectCommandEXT per page.
        VkBuffer meshTaskCommands = VK_NULL_HANDLE;
        VkDescriptorSet meshDrawSet = VK_NULL_HANDLE;
    };

    void record(VkCommandBuffer commandBuffer, const std::array<Mat4, ShadowMap::ClipLevelCount>& clipMatrices,
                std::uint32_t updateMask, VkBuffer vertexBuffer, VkBuffer instanceBuffer, VkBuffer indexBuffer,
                VkDescriptorSet sceneDescriptorSet, VkDescriptorSet twoSidedSceneDescriptorSet,
                const Culling::GPUCullingPass& cullingPass, const Culling::IndexedIndirectDrawCount& indirectDraw,
                const Culling::GPUCullingPass& twoSidedCullingPass,
                const Culling::IndexedIndirectDrawCount& twoSidedIndirectDraw, std::uint32_t objectCount,
                VkDescriptorSet grassDescriptorSet = VK_NULL_HANDLE,
                const Culling::IndexedIndirectDrawCount* grassIndirectDraw = nullptr,
                std::uint32_t grassCommandsPerPage = 0, const MeshletStream* meshletStream = nullptr,
                const MeshletStream* twoSidedMeshletStream = nullptr);

    /** Exact page transforms used by record(), in its current page order.
     * The grass VSM culler consumes these before the render pass. */
    [[nodiscard]] std::vector<Mat4>
    grassPageMatrices(const std::array<Mat4, ShadowMap::ClipLevelCount>& clipMatrices) const;

    void preparePages(const std::array<Mat4, ShadowMap::ClipLevelCount>& clipMatrices, const Mat4& cameraViewProjection,
                      std::span<const Culling::GPUObjectData> objects,
                      std::span<const Culling::GPUObjectData> dirtyObjects,
                      // GPU-compacted page keys from the completed frame's depth pass.
                      // feedbackValid distinguishes zero requests from unavailable feedback.
                      std::span<const std::uint32_t> receiverPageRequests, bool feedbackValid, std::uint32_t frameIndex,
                      std::uint32_t pageUpdateBudget = ShadowMap::PhysicalPageCount);

    void invalidateCache() noexcept;

    /** What the last preparePages() did to the page cache. */
    struct CacheStatistics final {
        std::uint32_t newPages{};
        std::uint32_t refreshedPages{};
        std::uint32_t dirtyMarkedPages{};
        std::uint32_t levelsScrolled{};
        std::uint32_t levelsInvalidated{};
        std::uint32_t deferredRequests{};
        std::uint32_t residentPages{};
    };
    [[nodiscard]] const CacheStatistics& cacheStatistics() const noexcept {
        return cacheStatistics_;
    }
    /** Optional GPU zones around the cull, refine and raster phases of record(). */
    void setProfiler(const GpuTimestampProfiler* profiler, std::uint32_t frameIndex) noexcept {
        profiler_ = profiler;
        profilerFrame_ = frameIndex;
    }
    [[nodiscard]] bool hasPendingPageUpdates() const noexcept {
        return pageUpdatesPending_;
    }

    [[nodiscard]] const std::array<std::uint32_t, ShadowMap::VirtualPageCount>& pageTable() const noexcept {
        return pageTable_;
    }

    [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const noexcept {
        return descriptorSetLayout_;
    }

    [[nodiscard]] VkDescriptorSet descriptorSet(std::uint32_t frameIndex) const;
    [[nodiscard]] VkBuffer pageTableBuffer(std::uint32_t frameIndex) const {
        return pageTableBuffers_.at(frameIndex)->handle();
    }
    [[nodiscard]] Buffer::MemoryInfo pageTableMemoryInfo(std::uint32_t frameIndex,
                                                         VkPhysicalDevice physicalDevice) const {
        if (frameIndex >= pageTableBuffers_.size())
            return {};
        return pageTableBuffers_.at(frameIndex)->memoryInfo(physicalDevice);
    }
    void setDirectionalVisibility(std::uint32_t frameIndex, const VkDescriptorImageInfo& texture) const;
    [[nodiscard]] VkDescriptorSet grassDescriptorSet(std::uint32_t frameIndex) const;
    [[nodiscard]] VkDescriptorSet grassVelocityDescriptorSet(std::uint32_t frameIndex) const;
    [[nodiscard]] VkDescriptorSet grassShadowDescriptorSet(std::uint32_t frameIndex) const;
    [[nodiscard]] VkDescriptorSet shadowDescriptorSet(std::uint32_t frameIndex) const;
    [[nodiscard]] VkDescriptorSet shadowTwoSidedDescriptorSet(std::uint32_t frameIndex) const;
    void setGrassVisibleInstances(std::uint32_t frameIndex, VkBuffer visibleInstances) const;
    void setGrassVelocityVisibleInstances(std::uint32_t frameIndex, VkBuffer visibleInstances) const;
    void setGrassShadowVisibleInstances(std::uint32_t frameIndex, VkBuffer visibleInstances) const;
    /** Bind the GPU-compacted shadow-only transform/material streams. */
    void setShadowInstanceTransforms(std::uint32_t frameIndex, VkBuffer transforms, VkBuffer materialOffsets,
                                     bool twoSided = false) const;

    // Packed grass reserves bindings 3/7 for cluster/deformation data.
    // this explicit prevents a future grass-only descriptor set from
    // silently using the generic seven-binding contract.
    static constexpr std::uint32_t GrassClusterBinding = 3;
    static constexpr std::uint32_t GrassDeformationBinding = 7;
    static constexpr std::uint32_t LocalLightBinding = 24;
    static constexpr std::uint32_t LocalShadowMaskBinding = 25;
    static constexpr std::uint32_t TileLightBinding = 26;

  private:
    struct PhysicalPage {
        std::uint16_t virtualX{};
        std::uint16_t virtualY{};
        std::uint8_t level{};
        bool allocated{};
        bool dirty{};
        std::uint64_t lastUsed{};
        std::uint64_t lastRendered{};
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

    [[nodiscard]] static std::uint32_t virtualPageIndex(std::uint32_t level, std::uint32_t x, std::uint32_t y) noexcept;
    void recordPageTableUpdate(VkCommandBuffer commandBuffer, bool afterShaderReads);

    mutable bool atlasInitialized_{false};
    mutable bool atlasContentValid_{false};
    std::array<std::uint32_t, ShadowMap::VirtualPageCount> pageTable_{};
    std::array<PhysicalPage, ShadowMap::PhysicalPageCount> physicalPages_{};
    std::array<Mat4, ShadowMap::ClipLevelCount> cachedClipMatrices_{};
    std::array<bool, ShadowMap::ClipLevelCount> cachedClipMatricesValid_{};
    std::vector<std::uint32_t> pagesToRender_;
    CacheStatistics cacheStatistics_{};
    const GpuTimestampProfiler* profiler_{};
    std::uint32_t profilerFrame_{};
    // Requests that could not be assigned a physical tile within the
    // per-frame update budget. Keep them until they are rendered instead
    // of depending on a later GPU feedback pass to rediscover them.
    std::vector<std::uint32_t> deferredRequests_;
    std::vector<PendingPageCommit> pendingPageCommits_;
    std::uint32_t preparedFrameIndex_{};
    std::uint64_t cacheClock_{};
    bool pageUpdatesPending_{};
    VkDevice device_{VK_NULL_HANDLE};
    // The two view contexts keep independent virtual page tables, but
    // render into the renderer-owned physical atlas.
    ShadowMap* shadowMap_{nullptr};
    VkDescriptorSetLayout descriptorSetLayout_{VK_NULL_HANDLE};
    bool rayTracingStages_{false};
    bool meshShaderStages_{false};
    VkDescriptorSetLayout meshletDrawLayout_{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> descriptorSets_;
    std::vector<VkDescriptorSet> grassDescriptorSets_;
    std::vector<VkDescriptorSet> grassVelocityDescriptorSets_;
    std::vector<VkDescriptorSet> grassShadowDescriptorSets_;
    // Isolated from forward descriptors: bindings 5/6 are shadow-only.
    std::vector<VkDescriptorSet> shadowDescriptorSets_;
    std::vector<VkDescriptorSet> shadowTwoSidedDescriptorSets_;
    // Per-frame-slot descriptor fingerprints. These setters are called
    // while recording, so avoid rewriting an unchanged binding every frame.
    mutable std::vector<VkDescriptorImageInfo> gtaoDescriptorCache_;
    mutable std::vector<bool> gtaoDescriptorCacheValid_;
    mutable std::vector<VkBuffer> grassVisibleDescriptorCache_;
    mutable std::vector<VkBuffer> grassVelocityVisibleDescriptorCache_;
    mutable std::vector<VkBuffer> grassShadowVisibleDescriptorCache_;
    mutable std::vector<bool> grassVisibleDescriptorCacheValid_;
    mutable std::vector<bool> grassVelocityVisibleDescriptorCacheValid_;
    mutable std::vector<bool> grassShadowVisibleDescriptorCacheValid_;
    std::vector<std::unique_ptr<Buffer>> pageTableBuffers_;
    std::vector<std::array<std::uint32_t, ShadowMap::VirtualPageCount>> uploadedPageTables_;
    std::vector<bool> uploadedPageTablesValid_;
    VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkPipeline opaquePipeline_{VK_NULL_HANDLE};
    // Mesh-shader twin of opaquePipeline_ for the meshlet stream.
    VkPipelineLayout meshletMeshPipelineLayout_{VK_NULL_HANDLE};
    VkPipeline meshletMeshPipeline_{VK_NULL_HANDLE};
    /// Two-sided stream: shadow_meshlet_mesh_alpha.spv, no culling.
    VkPipeline meshletAlphaMeshPipeline_{VK_NULL_HANDLE};
    PFN_vkCmdDrawMeshTasksIndirectEXT drawMeshTasksIndirect_{nullptr};
    VkPipeline twoSidedPipeline_{VK_NULL_HANDLE};
    VkPipeline grassPipeline_{VK_NULL_HANDLE};
};
} // namespace Engine
