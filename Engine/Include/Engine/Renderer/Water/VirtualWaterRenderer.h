#pragma once

#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Renderer/Culling/HiZBuffer.h"
#include "Engine/Renderer/Culling/HiZPass.h"
#include "Engine/Renderer/Culling/IndexedIndirectDrawCount.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Renderer/Vulkan/buffer.h"
#include "Engine/Renderer/Vulkan/graphics_pipeline.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"
#include "Engine/Renderer/Water/VirtualWaterTypes.h"
#include "Engine/Renderer/Water/WaterRenderWorld.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>
#include <deque>
#include <memory>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace Engine {
namespace Assets { class AssetManager; }

namespace Water {
/** GPU-owned virtual ocean renderer. Ocean pages never become generic scene draw records. */
class VirtualWaterRenderer final {
public:
    VirtualWaterRenderer() = default;
    ~VirtualWaterRenderer() { destroy(); }
    VirtualWaterRenderer(const VirtualWaterRenderer&) = delete;
    VirtualWaterRenderer& operator=(const VirtualWaterRenderer&) = delete;
    static constexpr std::uint32_t FramesInFlight = 3;

    void create(VkPhysicalDevice physicalDevice, VkDevice device, VmaAllocator allocator,
                Assets::AssetManager& assets, VkExtent2D extent, VkFormat depthFormat,
                VkImageView depthView, VkDescriptorSetLayout sceneLayout,
                VkImageView hdrTargetView, VkDescriptorImageInfo opaqueColor,
                VkDescriptorImageInfo opaqueDepth, VkDescriptorImageInfo previousHiZ,
                std::span<const VkBuffer> instanceBuffers,
                std::span<const VkBuffer> cullingUniformBuffers,
                bool enableHiZ = true);
    void destroy() noexcept;

    /** Rebuilds GPU page/templates from an immutable scene-generation snapshot. */
    void rebuild(const WaterRenderWorld& world);
    /** Refreshes frame-buffer bindings after renderer buffer growth/recreation. */
    void updateFrameBindings(std::span<const VkBuffer> instanceBuffers,
                             std::span<const VkBuffer> cullingUniformBuffers,
                             VkDescriptorImageInfo previousHiZ);

    void recordCull(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                    VkDescriptorSet sceneSet);
    void recordState(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                     VkDescriptorSet sceneSet, float deltaTime);
    void recordPrepass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                       VkDescriptorSet sceneSet, VkBuffer vertexBuffer, VkBuffer indexBuffer,
                       const Culling::IndexedIndirectDrawCount& authoredWaterDraw,
                       VkDeviceSize authoredCommandOffset, VkDeviceSize authoredCountOffset) const;
    void recordAdaptiveShading(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                               VkDescriptorSet sceneSet);
    void recordComposite(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                         VkDescriptorSet sceneSet) const;

    /** Queues a local disturbance for the sparse persistent water-state cache. */
    void queueInteraction(const Vec3& worldPosition, float radius, float strength);
    /** Updates the full-screen underwater post-process state for one frame slot. */
    void updateUnderwaterState(std::uint32_t frameIndex, Registry& registry,
                               const Vec3& cameraPosition, float time);
    /** Invalidates temporal water/reflection history after a discontinuous camera change. */
    void invalidateTemporalHistory() noexcept { temporalHistoryValid_ = false; }
    /** Feeds the completed frame cost back into the bounded quality scheduler. */
    void onFrameCompleted(std::uint32_t frameIndex, float measuredWaterGpuMs);
    /** Feeds the previous completed water GPU time into the feedback controller. */
    void updateFrameBudget(float measuredGpuMs) noexcept;
    /** Releases snapshot resources after a fence-completed frame advances the safety window. */
    void onFrameCompleted() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] std::uint32_t pageCount() const noexcept { return pageCount_; }
    [[nodiscard]] std::uint32_t drawBinCount() const noexcept { return drawBinCount_; }
    [[nodiscard]] VkImageView surfaceView() const noexcept { return surface_.imageView(); }
    [[nodiscard]] VkSampler surfaceSampler() const noexcept { return surface_.sampler(); }
    [[nodiscard]] VkImageView metaView() const noexcept { return meta_.imageView(); }
    [[nodiscard]] VkSampler metaSampler() const noexcept { return meta_.sampler(); }
    [[nodiscard]] VkImageView velocityView() const noexcept { return velocity_.imageView(); }
    [[nodiscard]] VkSampler velocitySampler() const noexcept { return velocity_.sampler(); }

private:
    struct ShadePush { std::uint32_t tier{}, pad0{}, pad1{}, pad2{}; };
    struct StatePush { std::uint32_t eventCount{}, pageCount{}, frameIndex{}, pad{}; float deltaTime{}, pad1{}, pad2{}, pad3{}; };

    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    Assets::AssetManager* assets_{};
    VkExtent2D extent_{};
    VkImageView depthView_{VK_NULL_HANDLE};

    std::uint32_t pageCount_{};
    std::uint32_t drawBinCount_{};
    std::uint32_t bodyCount_{};
    bool active_{};
    bool authoredWaterActive_{};
    std::array<bool, FramesInFlight> lightingInitialized_{};
    bool temporalHistoryValid_{false};
    // True when stateCellsScratch_ contains the most recently committed simulation state.
    bool stateCurrentScratch_{};

    Buffer pages_;
    Buffer drawTemplates_;
    // Reusable clipmap page topology, owned by Virtual Water rather than ECS.
    Buffer oceanVertexBuffer_;
    Buffer oceanIndexBuffer_;
    std::vector<Mesh::DrawRange> oceanDrawRanges_;
    std::array<Buffer, FramesInFlight> cullConfig_;
    Buffer farOceanConfig_;
    Buffer authoredWaterConfig_;
    std::array<Buffer, FramesInFlight> visiblePages_;
    std::array<Buffer, FramesInFlight> drawBinCounts_;
    std::array<Buffer, FramesInFlight> indirectCommands_;
    std::array<Buffer, FramesInFlight> history_;
    std::array<Buffer, FramesInFlight> stats_;

    Buffer statePageTable_;
    Buffer statePhysical_;
    Buffer stateCells_;
    Buffer stateCellsScratch_;
    Buffer stateAllocator_;
    std::array<Buffer, FramesInFlight> interactionEvents_;
    std::array<Buffer, FramesInFlight> underwaterConfig_;
    std::vector<GPUWaterInteractionEvent> pendingInteractions_;

    HdrBuffer surface_;
    HdrBuffer meta_;
    HdrBuffer velocity_;
    std::array<HdrBuffer, FramesInFlight> lighting_;
    Culling::HiZBuffer sssrDepth_;
    Culling::HiZPass sssrDepthPass_;
    bool sssrDepthInitialized_{};

    GPUWaterTileSettings tileSettings_{};
    std::array<Buffer, FramesInFlight> tileSettingsBuffer_;
    std::array<Buffer, FramesInFlight> frameBudgetBuffer_;
    WaterFrameBudget frameBudget_{};
    WaterDynamicThresholds dynamicThresholds_{};
    std::array<Buffer, FramesInFlight> importanceHistogram_;
    std::array<Buffer, FramesInFlight> candidateTiles_;
    std::array<Buffer, FramesInFlight> tileLists_;
    std::array<Buffer, FramesInFlight> tileCounts_;
    std::array<Buffer, FramesInFlight> tileDispatch_;

    struct RetiredSnapshot final {
        std::uint64_t retireAfterCompletedFrame{};
        std::vector<std::shared_ptr<const MeshGpuResource>> resources;
    };
    std::vector<std::shared_ptr<const MeshGpuResource>> snapshotResources_;
    std::deque<RetiredSnapshot> retiredSnapshots_;
    std::uint64_t completedFrameSerial_{};
    std::uint64_t waterWorldGeneration_{};
    bool stateInitialized_{};
    float measuredGpuMsEma_{};
    bool denseTileClassification_{true};

    VkDescriptorSetLayout cullLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout buildLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout drawLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout classifyLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout buildDispatchLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout shadeLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout compositeLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout stateLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout farLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout authoredLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout sssrInitLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout sssrReduceLayout_{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, FramesInFlight> cullSets_{};
    std::array<VkDescriptorSet, FramesInFlight> buildSets_{};
    // State-dependent passes have two prewritten variants per frame slot:
    // [0] reads stateCells_, [1] reads stateCellsScratch_.  Rendering consumes
    // the inverse (the simulation output) variant after recordState() flips.
    std::array<std::array<VkDescriptorSet, FramesInFlight>, 2> drawSets_{};
    std::array<VkDescriptorSet, FramesInFlight> classifySets_{};
    std::array<VkDescriptorSet, FramesInFlight> buildDispatchSets_{};
    std::array<std::array<VkDescriptorSet, FramesInFlight>, 2> shadeSets_{};
    std::array<VkDescriptorSet, FramesInFlight> compositeSets_{};
    std::array<std::array<VkDescriptorSet, FramesInFlight>, 2> stateSets_{};
    std::array<VkDescriptorSet, FramesInFlight> farSets_{};
    std::array<std::array<VkDescriptorSet, FramesInFlight>, 2> authoredSets_{};

    VkPipelineLayout cullPipelineLayout_{VK_NULL_HANDLE};
    VkPipelineLayout buildPipelineLayout_{VK_NULL_HANDLE};
    VkPipelineLayout classifyPipelineLayout_{VK_NULL_HANDLE};
    VkPipelineLayout buildDispatchPipelineLayout_{VK_NULL_HANDLE};
    VkPipelineLayout shadePipelineLayout_{VK_NULL_HANDLE};
    VkPipelineLayout statePipelineLayout_{VK_NULL_HANDLE};
    VkPipelineLayout sssrInitPipelineLayout_{VK_NULL_HANDLE};
    VkPipelineLayout sssrReducePipelineLayout_{VK_NULL_HANDLE};
    VkPipeline cullPipeline_{VK_NULL_HANDLE};
    VkPipeline buildPipeline_{VK_NULL_HANDLE};
    VkPipeline classifyPipeline_{VK_NULL_HANDLE};
    VkPipeline buildDispatchPipeline_{VK_NULL_HANDLE};
    VkPipeline shadePipeline_{VK_NULL_HANDLE};
    VkPipeline stateAllocatePipeline_{VK_NULL_HANDLE};
    VkPipeline statePipeline_{VK_NULL_HANDLE};
    VkPipeline sssrInitPipeline_{VK_NULL_HANDLE};
    VkPipeline sssrReducePipeline_{VK_NULL_HANDLE};

    GraphicsPipeline prepassPipeline_;
    GraphicsPipeline farPrepassPipeline_;
    GraphicsPipeline authoredPrepassPipeline_;
    GraphicsPipeline compositePipeline_;
    VkFramebuffer prepassFramebuffer_{VK_NULL_HANDLE};
    VkFramebuffer compositeFramebuffer_{VK_NULL_HANDLE};

    std::array<VkBuffer, FramesInFlight> instanceBuffers_{};
    std::array<VkBuffer, FramesInFlight> cullingUniformBuffers_{};
    VkDescriptorImageInfo previousHiZ_{};
    VkDescriptorImageInfo opaqueColor_{};
    VkDescriptorImageInfo opaqueDepth_{};
    // Editor Scene View starts without a separate Hi-Z hierarchy.  Its
    // descriptor remains valid (depth is bound), but page culling is strictly
    // frustum based until a per-view hierarchy is introduced.
    bool hiZEnabled_{true};
    std::uint32_t waterFrameCounter_{};
    float measuredWaterGpuMs_{};

    void createBuffers();
    void createOceanGeometry();
    void createDescriptors(VkDescriptorSetLayout sceneLayout);
    void createPipelines(VkDescriptorSetLayout sceneLayout, VkFormat depthFormat);
    void createFramebuffers(VkImageView hdrTargetView);
    void writeDescriptors();
    [[nodiscard]] VkPipeline makeCompute(const char* shader, VkPipelineLayout layout) const;
};
} // namespace Water
} // namespace Engine
