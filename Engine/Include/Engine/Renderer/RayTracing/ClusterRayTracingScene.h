#pragma once

#include "Engine/Renderer/RayTracing/AccelerationStructureManager.h"
#include "Engine/Renderer/Vulkan/buffer.h"

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets {
class AssetManager;
}

/**
 * Ray tracing scene built with RTX Mega Geometry, the Vulkan counterpart of
 * DXR's clustered geometry and partitioned TLAS (Agility SDK 1.722):
 *  - every meshlet becomes a triangle cluster (CLAS), converted on the GPU
 *    and built with one indirect, implicit-destination command;
 *  - displaced geometry uses cluster templates: a template stores a
 *    meshlet's topology once, a compute pass writes the displaced positions
 *    and instantiation turns the template into that variant's CLAS;
 *  - each mesh (or displaced variant) is a cluster BLAS that only links
 *    prebuilt clusters, all of its sections in one structure;
 *  - instances live in a partitioned TLAS (PTLAS) with partition
 *    translation: static instances are stored relative to their spatial
 *    partition's origin, an instance that moves is rewritten into the global
 *    partition, and only partitions holding written instances are rebuilt;
 *  - once a build has finished, CLASes are compacted with MOVE_OBJECTS into
 *    a tightly sized buffer, cluster BLASes are relinked, the PTLAS is
 *    repointed with UPDATE_INSTANCE and every build input is released.
 * Cluster acceleration structures can be traversed only by ray tracing
 * pipelines that allow them, so that pipeline must be bound while building.
 */
class ClusterRayTracingScene final {
  public:
    using BlasKey = AccelerationStructureManager::BlasKey;
    /** A meshlet range of the cluster BLAS named by `key`; inputs sharing a key build one BLAS. */
    struct BlasInput final {
        BlasKey key{};
        /** Range in the global meshlet heap. */
        std::uint32_t firstMeshlet{};
        std::uint32_t meshletCount{};
    };
    /** A mesh section displaced by the materials starting at `materialBase`. */
    struct VariantInput final {
        BlasKey key{};
        std::uint32_t materialBase{};
        bool operator==(const VariantInput&) const noexcept = default;
    };
    struct GeometrySources final {
        VkBuffer meshlets{VK_NULL_HANDLE};
        VkBuffer meshletVertices{VK_NULL_HANDLE};
        VkBuffer meshletTriangles{VK_NULL_HANDLE};
        VkBuffer vertices{VK_NULL_HANDLE};
    };
    struct InstanceInput final {
        BlasKey key{};
        /** Row-major 3x4 object-to-world transform. */
        std::array<float, 12> transform{};
        std::uint32_t materialBase{};
        /** Uses the (key, materialBase) displaced variant instead of the source section. */
        bool displaced{false};
    };
    /** Per-instance shading record, indexed by the PTLAS instance ID. */
    struct InstanceRecord final {
        std::array<float, 12> objectToWorld{};
        std::uint32_t reserved{};
        std::uint32_t materialBase{};
        std::uint32_t padding[2]{};
    };
    static_assert(sizeof(InstanceRecord) == 64, "InstanceRecord is a shader ABI");
    struct MemoryStats final {
        /** Acceleration structure and build-input memory right after the build. */
        VkDeviceSize builtBytes{};
        /** The same after compaction released the build inputs. */
        VkDeviceSize compactedBytes{};
        std::uint32_t clusters{};
        std::uint32_t templates{};
        std::uint32_t displacedClusters{};
        std::uint32_t blases{};
        bool compacted{false};
    };

    ClusterRayTracingScene() = default;
    ~ClusterRayTracingScene();
    ClusterRayTracingScene(const ClusterRayTracingScene&) = delete;
    ClusterRayTracingScene& operator=(const ClusterRayTracingScene&) = delete;

    /** `forwardLayout` is set 0 of the displacement pass, which samples material height maps. */
    void create(VkPhysicalDevice physicalDevice, VkDevice device, VmaAllocator allocator, Assets::AssetManager& assets,
                VkDescriptorSetLayout forwardLayout);
    void destroy() noexcept;
    /** Frees every acceleration structure and build input; the next buildGeometry() starts over. */
    void releaseScene();
    /**
     * Rebuilds every CLAS, template, displaced variant and cluster BLAS.
     * Waits for the device when it replaces existing structures; call only
     * after geometry or the set of displaced variants changed.
     */
    void buildGeometry(VkCommandBuffer commandBuffer, VkPipeline clusterTracingPipeline, VkDescriptorSet forwardSet,
                       std::span<const BlasInput> blases, std::span<const VariantInput> variants,
                       const GeometrySources& sources);
    /**
     * Updates the partitioned TLAS. `topologyChanged` (or a different
     * instance count) writes every instance from scratch; otherwise only
     * instances whose transform changed are written, in place. The first call
     * after a build has completed on the GPU compacts the clusters instead.
     */
    void updateInstances(VkCommandBuffer commandBuffer, VkPipeline clusterTracingPipeline, std::uint32_t frameSlot,
                         std::span<const InstanceInput> instances, bool topologyChanged);
    [[nodiscard]] bool ready() const noexcept {
        return tlasBuilt_ && instanceCount_ > 0;
    }
    [[nodiscard]] bool hasGeometry() const noexcept {
        return !blasFirstCluster_.empty();
    }
    [[nodiscard]] bool hasVariants(std::span<const VariantInput> variants) const;
    [[nodiscard]] VkDeviceAddress tlasAddress() const noexcept {
        return tlasAddress_;
    }
    [[nodiscard]] VkBuffer instanceRecords(std::uint32_t frameSlot) const noexcept {
        return frames_[frameSlot % FramesInFlight].records.handle();
    }
    [[nodiscard]] VkBuffer clusterMeshlets() const noexcept {
        return clusterMeshlets_.handle();
    }
    [[nodiscard]] const GeometrySources& geometrySources() const noexcept {
        return sources_;
    }
    [[nodiscard]] const MemoryStats& memoryStats() const noexcept {
        return stats_;
    }

  private:
    static constexpr std::uint32_t FramesInFlight = 2;
    struct VariantKeyHash final {
        std::size_t operator()(const VariantInput& variant) const noexcept {
            return AccelerationStructureManager::BlasKeyHash{}(variant.key) ^ (variant.materialBase * 0x9e3779b9U);
        }
    };
    struct FrameResources final {
        Buffer records;
        std::uint64_t recordsRevision{};
        Buffer operations;
        Buffer commands;
    };
    struct RetiredBuffer final {
        Buffer buffer;
        std::uint64_t serial{};
    };
    struct Kernel final {
        VkDescriptorSetLayout setLayout{VK_NULL_HANDLE};
        VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
        VkPipeline pipeline{VK_NULL_HANDLE};
    };
    void releaseGeometry() noexcept;
    void retire(Buffer& buffer);
    void ensurePtlas(std::uint32_t instanceCount, std::uint32_t partitionCount, std::uint32_t maxPerPartition);
    void compact(VkCommandBuffer commandBuffer, VkPipeline clusterTracingPipeline, std::uint32_t frameSlot,
                 std::span<const InstanceInput> instances);
    void buildBlases(VkCommandBuffer commandBuffer);
    void patchAddresses(VkCommandBuffer commandBuffer, std::uint32_t frameSlot, const Buffer& records,
                        std::uint32_t count, std::uint32_t recordWords, std::uint32_t addressWord);
    void buildPtlas(VkCommandBuffer commandBuffer, const FrameResources& frame, std::uint32_t commandCount,
                    bool fromScratch);
    [[nodiscard]] std::uint32_t blasFor(const InstanceInput& instance) const noexcept;
    [[nodiscard]] VkDeviceSize allocatedBytes() const noexcept;

    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    PFN_vkGetClusterAccelerationStructureBuildSizesNV getClusterSizes_{};
    PFN_vkCmdBuildClusterAccelerationStructureIndirectNV cmdBuildClusters_{};
    PFN_vkGetPartitionedAccelerationStructuresBuildSizesNV getPartitionedSizes_{};
    PFN_vkCmdBuildPartitionedAccelerationStructuresNV cmdBuildPartitioned_{};
    VkPhysicalDeviceClusterAccelerationStructurePropertiesNV clusterProperties_{};

    // Cluster conversion, displacement and BLAS-address patch compute kernels.
    Kernel prepare_;
    Kernel displace_;
    Kernel patch_;
    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    VkDescriptorSet prepareSet_{VK_NULL_HANDLE};
    VkDescriptorSet displaceSet_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, FramesInFlight> patchSets_{};

    GeometrySources sources_{};
    std::unordered_map<BlasKey, std::uint32_t, AccelerationStructureManager::BlasKeyHash> blasIndices_;
    std::unordered_map<VariantInput, std::uint32_t, VariantKeyHash> variantIndices_;
    std::vector<std::uint32_t> blasFirstCluster_;
    std::vector<std::uint32_t> blasClusterCount_;
    std::uint32_t clusterCount_{};
    std::uint32_t variantClusterCount_{};
    std::uint32_t templateCount_{};
    std::uint32_t maxClustersPerBlas_{};
    // Build inputs, released by compaction.
    Buffer clusterPositions_;
    Buffer clusterIndices_;
    Buffer clusterBuildInfos_;
    Buffer templateInfos_;
    Buffer templateAddresses_;
    Buffer templateSizes_;
    Buffer templateData_;
    Buffer variantJobs_;
    Buffer variantPositions_;
    Buffer instantiateInfos_;
    Buffer variantData_;
    Buffer scratch_;
    // Persistent scene.
    Buffer clusterMeshlets_;
    Buffer clusterAddresses_;
    Buffer clusterSizes_;
    Buffer clusterData_;
    Buffer blasBuildInfos_;
    Buffer blasAddresses_;
    Buffer blasSizes_;
    Buffer blasData_;
    std::vector<RetiredBuffer> retired_;
    std::uint64_t updateSerial_{};
    std::uint64_t builtSerial_{};
    bool compactionPending_{false};
    MemoryStats stats_{};

    // Partitioned TLAS.
    VkPartitionedAccelerationStructureFlagsNV ptlasFlags_{};
    VkPartitionedAccelerationStructureInstancesInputNV ptlasInput_{};
    Buffer ptlasData_;
    Buffer ptlasScratch_;
    VkDeviceAddress tlasAddress_{};
    bool tlasBuilt_{false};
    std::uint32_t instanceCount_{};
    std::vector<std::array<float, 12>> writtenTransforms_;
    std::vector<std::uint32_t> partitions_;
    std::vector<std::array<float, 3>> partitionOrigins_;
    std::vector<InstanceRecord> records_;
    std::uint64_t recordsRevision_{};
    std::array<FrameResources, FramesInFlight> frames_{};
};
} // namespace Engine
