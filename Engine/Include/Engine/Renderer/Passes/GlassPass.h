#pragma once

#include "Engine/Renderer/Culling/HiZBuffer.h"
#include "Engine/Renderer/Vulkan/buffer.h"
#include "Engine/Renderer/Vulkan/graphics_pipeline.h"

#include <array>
#include <cstdint>
#include <vector>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets {
class AssetManager;
}
namespace Culling {
class IndexedIndirectDrawCount;
}
class GpuTimestampProfiler;

/**
 * Composites transmissive (glass) materials over the finished opaque scene.
 *
 * One record() call, on a single-sample colour target that already holds
 * the lit opaque scene (plus water and particles):
 *  1. copies the colour into a mip-mapped snapshot. Rough (frosted) glass
 *     refracts a wider footprint of it by sampling coarser mips;
 *  2. reduces the scene depth into a MIN (closest-surface) hardware-depth
 *     pyramid. Level 0 is the scene depth itself (sampled in place, never
 *     copied); the coarse levels let reflection and refraction rays skip
 *     empty screen space (Hi-Z traversal, see Glass/glass_surface.slang).
 *     Steps 1 and 2 are compute work recorded interleaved: the snapshot takes
 *     two shared-memory downsample dispatches (glass_snapshot_downsample),
 *     the pyramid one fused dispatch for its first even-sized levels
 *     (glass_depth_downsample) plus one per remaining level, up to the
 *     deepest level the trace reads;
 *  3. draws the glass program slot. Every glass pixel composes its final
 *     colour itself (refracted snapshot, Hi-Z SSR reflection with probe/sky
 *     fallback, absorption, direct specular), so no blending is needed.
 *
 * The scene depth image is only read: Hi-Z culling may still read it on the
 * async compute queue. Glass draws into a private depth target seeded with
 * the opaque depth, so panes behind opaque geometry are
 * rejected by early depth testing and the nearest pane wins.
 */
class GlassPass final {
  public:
    struct CreateInfo {
        VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
        VkDevice device{VK_NULL_HANDLE};
        VmaAllocator allocator{VK_NULL_HANDLE};
        Assets::AssetManager* assets{nullptr};
        VkDescriptorSetLayout sceneLayout{VK_NULL_HANDLE};
        VkExtent2D extent{};
        /// Format of the single-sample scene depth that record() reads.
        VkFormat depthFormat{VK_FORMAT_UNDEFINED};
        VkImageView sceneDepthView{VK_NULL_HANDLE};
        VkSampler sceneDepthSampler{VK_NULL_HANDLE};
    };

    struct RecordInfo {
        /// Single-sample colour target in SHADER_READ_ONLY_OPTIMAL. It is
        /// left in that layout, readable from fragment and compute shaders.
        VkImage color{VK_NULL_HANDLE};
        VkImageView colorView{VK_NULL_HANDLE};
        /// Scene depth (the view in CreateInfo) in DEPTH_STENCIL_READ_ONLY_OPTIMAL.
        VkImage depth{VK_NULL_HANDLE};
        VkDescriptorSet sceneDescriptorSet{VK_NULL_HANDLE};
        VkBuffer vertexBuffer{VK_NULL_HANDLE};
        VkBuffer indexBuffer{VK_NULL_HANDLE};
        /// Indexed streams holding the glass program slot: the single-sided
        /// stream (back faces culled) and the foliage/two-sided stream.
        const Culling::IndexedIndirectDrawCount* singleSided{nullptr};
        const Culling::IndexedIndirectDrawCount* twoSided{nullptr};
        /// Glass slot commands in each stream (the streams pack slots separately).
        VkDeviceSize commandOffset{0};
        VkDeviceSize twoSidedCommandOffset{0};
        VkDeviceSize countOffset{0};
        /// Optional: times the inputs (snapshot and pyramid), depth seed and
        /// draws as nested Glass.* zones of the caller's open zone.
        const GpuTimestampProfiler* profiler{nullptr};
        std::uint32_t profilerFrame{0};
    };

    GlassPass() = default;
    ~GlassPass();
    GlassPass(const GlassPass&) = delete;
    GlassPass& operator=(const GlassPass&) = delete;

    void create(const CreateInfo& info);
    void destroy() noexcept;
    [[nodiscard]] bool created() const noexcept {
        return device_ != VK_NULL_HANDLE;
    }
    [[nodiscard]] VkExtent2D extent() const noexcept {
        return extent_;
    }

    void record(VkCommandBuffer commandBuffer, const RecordInfo& info);

  private:
    void createImages();
    void createPyramidPipelines();
    void createGraphicsPipelines();
    void createDescriptors();
    void createSnapshotPipeline();
    void createGate();
    void planInputs();
    void recordGate(VkCommandBuffer commandBuffer, const RecordInfo& info) const;
    void dispatchInput(VkCommandBuffer commandBuffer, std::uint32_t& entry, std::uint32_t x, std::uint32_t y) const;
    void recordInputs(VkCommandBuffer commandBuffer, VkImage color, VkImage sceneDepth);

    CreateInfo info_{};
    VkDevice device_{VK_NULL_HANDLE};
    VkExtent2D extent_{};

    // Mip-mapped copy of the scene colour the glass refracts and reflects.
    VkImage snapshot_{VK_NULL_HANDLE};
    VmaAllocation snapshotMemory_{VK_NULL_HANDLE};
    VkImageView snapshotView_{VK_NULL_HANDLE};
    VkSampler snapshotSampler_{VK_NULL_HANDLE};
    std::uint32_t snapshotMips_{0};
    bool snapshotInitialized_{false};
    // One storage view per level for the downsample (unused slots repeat the last).
    std::array<VkImageView, 8> snapshotMipViews_{};
    VkDescriptorSetLayout snapshotSetLayout_{VK_NULL_HANDLE};
    VkPipelineLayout snapshotPipelineLayout_{VK_NULL_HANDLE};
    VkPipeline snapshotPipeline_{VK_NULL_HANDLE};
    VkDescriptorSet snapshotSet_{VK_NULL_HANDLE};
    // Colour view bound as the snapshot source (binding 0 of snapshotSet_).
    VkImageView snapshotSourceView_{VK_NULL_HANDLE};

    // Closest-surface hardware-depth pyramid.
    Culling::HiZBuffer pyramid_;
    bool pyramidInitialized_{false};
    // Levels written by the fused first dispatch (besides level 0), and the
    // deepest level built at all: the Hi-Z trace never reads past it.
    std::uint32_t pyramidFusedLevels_{0};
    std::uint32_t pyramidLastLevel_{0};
    VkDescriptorSetLayout pyramidFusedSetLayout_{VK_NULL_HANDLE};
    VkPipelineLayout pyramidFusedPipelineLayout_{VK_NULL_HANDLE};
    VkPipeline pyramidFusedPipeline_{VK_NULL_HANDLE};
    VkDescriptorSet pyramidFusedSet_{VK_NULL_HANDLE};
    VkDescriptorSetLayout pyramidSetLayout_{VK_NULL_HANDLE};
    VkPipelineLayout pyramidPipelineLayout_{VK_NULL_HANDLE};
    VkPipeline pyramidReducePipeline_{VK_NULL_HANDLE};
    // reduce set i writes level pyramidFusedLevels_ + 1 + i.
    std::vector<VkDescriptorSet> pyramidReduceSets_;

    // Private depth: glass-over-glass visibility only.
    VkImage glassDepth_{VK_NULL_HANDLE};
    VmaAllocation glassDepthMemory_{VK_NULL_HANDLE};
    VkImageView glassDepthView_{VK_NULL_HANDLE};

    GraphicsPipeline singleSidedPipeline_;
    GraphicsPipeline twoSidedPipeline_;
    // Copies the opaque scene depth into glassDepth_ first.
    GraphicsPipeline depthSeedPipeline_;
    VkDescriptorSetLayout glassSetLayout_{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    VkDescriptorSet glassSet_{VK_NULL_HANDLE};

    // glass_inputs_gate: the depth seed draw and every input dispatch run
    // indirectly from gate_, whose arguments are zeroed on the GPU when
    // culling left no glass draw. inputGroups_ lists their sizes in
    // recording order (entry 0 is the seed draw).
    Buffer gate_;
    std::vector<std::array<std::uint32_t, 2>> inputGroups_;
    VkDescriptorSetLayout gateSetLayout_{VK_NULL_HANDLE};
    VkPipelineLayout gatePipelineLayout_{VK_NULL_HANDLE};
    VkPipeline gatePipeline_{VK_NULL_HANDLE};
    VkDescriptorSet gateSet_{VK_NULL_HANDLE};
};
} // namespace Engine
