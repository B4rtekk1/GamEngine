#pragma once

#include "Engine/ECS/Components/GlassDestructionComponent.h"
#include "Engine/Renderer/Vulkan/MemoryBudgetManager.h"
#include "Engine/Renderer/Vulkan/graphics_pipeline.h"

#include <array>
#include <cstdint>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace Engine {
class Scene;
namespace Culling {
class IndexedIndirectDrawCount;
}

/**
 * Dedicated compositing pass for glass surfaces.
 *
 * It loads opaque HDR/depth, samples a copied opaque scene in the shader and
 * writes the composed GlassThin colour.  Keeping this out of ForwardPass
 * prevents glass from becoming an opaque depth-prepass material.
 */
class GlassPass final {
  public:
    void create(VkDevice device, VmaAllocator allocator, VkFormat colorFormat, VkFormat depthFormat,
                VkSampleCountFlagBits samples, VkFormat depthResolveFormat, VkResolveModeFlagBits depthResolveMode,
                VkDescriptorSetLayout sceneLayout, Assets::AssetManager& assets, bool velocity,
                const VkDescriptorImageInfo& opaqueColor, const VkDescriptorImageInfo& opaqueDepth,
                unsigned paneCount = 0);
    void destroy() noexcept;
    [[nodiscard]] std::vector<GpuSceneMemoryAllocation> memoryAllocations() const;
    /** Caller must retire all frames before resizing descriptor-backed storage. */
    void resizeCrackAtlas(unsigned paneCount);
    [[nodiscard]] bool needsCrackAtlasResize(unsigned paneCount) const noexcept;
    /** Incrementally stamps changed pane regions on the GPU, before rendering. */
    void recordCracks(VkCommandBuffer commandBuffer, const Scene& scene, std::uint32_t frameIndex);
    void begin(VkCommandBuffer commandBuffer, VkImageView colorView, VkImageView depthView, VkImageView velocityView,
               VkImageView colorResolveView, VkImageView depthResolveView, VkExtent2D extent,
               VkDescriptorSet sceneDescriptorSet, std::uint32_t frameIndex, VkBuffer vertexBuffer,
               VkBuffer indexBuffer) const;
    void draw(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
              const Culling::IndexedIndirectDrawCount& indirectDraw, VkDeviceSize commandOffset,
              VkDeviceSize countOffset) const;
    static void end(VkCommandBuffer commandBuffer);

  private:
    static constexpr std::uint32_t FramesInFlight = 2;
    void createSceneDescriptors(const VkDescriptorImageInfo& opaqueColor, const VkDescriptorImageInfo& opaqueDepth);
    static constexpr unsigned AtlasSlots = 512, AtlasResolution = 256;
    void createCrackResources(Assets::AssetManager& assets, unsigned paneCount);
    void createCrackStorage(unsigned layers);
    void destroyCrackStorage() noexcept;
    unsigned atlasLayers_{};
    VmaAllocator allocator_{};
    VkImage crackAtlas_{};
    VmaAllocation crackAtlasMemory_{};
    VkImageView crackAtlasView_{};
    VkSampler crackSampler_{};
    VkPipeline crackPipeline_{};
    VkPipelineLayout crackPipelineLayout_{};
    bool crackAtlasInitialized_{};
    std::array<VkBuffer, FramesInFlight> crackMetadata_{};
    std::array<VmaAllocation, FramesInFlight> crackMetadataMemory_{};
    std::array<void*, FramesInFlight> crackMetadataMapped_{};
    struct CrackSlot {
        std::uint64_t uuid{};
        std::size_t submitted{}, segmentOffset{};
        std::uint32_t historyStart{};
        bool reverse{};
        std::vector<GlassCrackSegment> segments;
        const Mesh* geometry{};
        std::vector<std::uint8_t> exposureState;
        std::array<glm::uvec4, 128> exposureMasks{};
    };
    std::array<CrackSlot, AtlasSlots> crackSlots_{};
    GraphicsPipeline pipeline_;
    VkDevice device_{VK_NULL_HANDLE};
    VkDescriptorSetLayout sceneTextureLayout_{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, FramesInFlight> sceneTextureSets_{};
    bool hasVelocity_{};
    VkSampleCountFlagBits samples_{VK_SAMPLE_COUNT_1_BIT};
    VkResolveModeFlagBits depthResolveMode_{VK_RESOLVE_MODE_NONE};
};
} // namespace Engine
