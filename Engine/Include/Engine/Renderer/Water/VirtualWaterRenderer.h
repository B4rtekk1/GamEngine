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

#include <array>
#include <cstdint>
#include <span>
#include <vector>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace Engine {
class Registry;
class SceneGpuResources;
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
                std::span<const VkBuffer> cullingUniformBuffers);
    void destroy() noexcept;

    /** Rebuilds logical pages/templates after scene topology or water authoring changes. */
    void rebuild(Registry& registry, const SceneGpuResources& sceneGpu);
    /** Refreshes frame-buffer bindings after renderer buffer growth/recreation. */
    void updateFrameBindings(std::span<const VkBuffer> instanceBuffers,
                             std::span<const VkBuffer> cullingUniformBuffers,
                             VkDescriptorImageInfo previousHiZ);

    void recordCull(VkCommandBuffer commandBuffer, std::uint32_t frameIndex) const;
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
    struct TileSettings { std::uint32_t width{}, height{}, tilesX{}, maxTiles{}; };
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
    bool lightingInitialized_{};
    // True when stateCellsScratch_ contains the most recently committed simulation state.
    bool stateCurrentScratch_{};

    Buffer pages_;
    Buffer drawTemplates_;
    Buffer cullConfig_;
    Buffer farOceanConfig_;
    Buffer authoredWaterConfig_;
    std::array<Buffer, FramesInFlight> visiblePages_;
    std::array<Buffer, FramesInFlight> drawBinCounts_;
    std::array<Buffer, FramesInFlight> indirectCommands_;
    std::array<Buffer, FramesInFlight> history_;
    std::array<Buffer, FramesInFlight> stats_;

    Buffer statePageTable_;
    Buffer stateOwners_;
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
    HdrBuffer lighting_;
    Culling::HiZBuffer sssrDepth_;
    Culling::HiZPass sssrDepthPass_;
    bool sssrDepthInitialized_{};

    TileSettings tileSettings_{};
    Buffer tileSettingsBuffer_;
    std::array<Buffer, FramesInFlight> tileLists_;
    std::array<Buffer, FramesInFlight> tileCounts_;
    std::array<Buffer, FramesInFlight> tileDispatch_;

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
    std::array<VkDescriptorSet, FramesInFlight> drawSets_{};
    std::array<VkDescriptorSet, FramesInFlight> classifySets_{};
    std::array<VkDescriptorSet, FramesInFlight> buildDispatchSets_{};
    std::array<VkDescriptorSet, FramesInFlight> shadeSets_{};
    std::array<VkDescriptorSet, FramesInFlight> compositeSets_{};
    std::array<VkDescriptorSet, FramesInFlight> stateSets_{};
    std::array<VkDescriptorSet, FramesInFlight> farSets_{};
    std::array<VkDescriptorSet, FramesInFlight> authoredSets_{};

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

    void createBuffers();
    void createDescriptors(VkDescriptorSetLayout sceneLayout);
    void createPipelines(VkDescriptorSetLayout sceneLayout, VkFormat depthFormat);
    void createFramebuffers(VkImageView hdrTargetView);
    void writeDescriptors();
    [[nodiscard]] VkPipeline makeCompute(const char* shader, VkPipelineLayout layout) const;
};
} // namespace Water
} // namespace Engine
