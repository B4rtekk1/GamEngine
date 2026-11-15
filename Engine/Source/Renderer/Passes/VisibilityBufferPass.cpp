#include "Engine/Renderer/Passes/VisibilityBufferPass.h"

#include "Engine/Renderer/Vulkan/PipelineCache.h"
#include "Engine/Renderer/shader_loader.h"

#include <array>
#include <span>
#include <stdexcept>
#include <string>

namespace Engine {
namespace {
constexpr std::uint32_t FramesInFlight = 2;
// VkDispatchIndirectCommand padded to four words per class, then the
// reflection pass's, whose fourth word counts its queued pixels
// (Common/ScreenSpaceReflection.slang SsrDispatchBase).
constexpr VkDeviceSize DispatchStride = 4 * sizeof(std::uint32_t);
constexpr std::uint32_t DispatchSlots = VisibilityBufferPass::ClassCount + 1;
// packSsrRequest: one uint4 per queued pixel.
constexpr VkDeviceSize SsrRequestSize = 4 * sizeof(std::uint32_t);

VkImageMemoryBarrier2 imageBarrier(const VkImage image, const VkPipelineStageFlags2 srcStage,
                                   const VkAccessFlags2 srcAccess, const VkImageLayout oldLayout,
                                   const VkPipelineStageFlags2 dstStage, const VkAccessFlags2 dstAccess,
                                   const VkImageLayout newLayout) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return barrier;
}

void submitBarriers(const VkCommandBuffer cmd, const std::span<const VkImageMemoryBarrier2> images,
                    const VkMemoryBarrier2* memory = nullptr) {
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = memory != nullptr ? 1U : 0U;
    dependency.pMemoryBarriers = memory;
    dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(images.size());
    dependency.pImageMemoryBarriers = images.data();
    vkCmdPipelineBarrier2(cmd, &dependency);
}

VkMemoryBarrier2 memoryBarrier(const VkPipelineStageFlags2 srcStage, const VkAccessFlags2 srcAccess,
                               const VkPipelineStageFlags2 dstStage, const VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    return barrier;
}

VkDescriptorSetLayout makeLayout(const VkDevice device, std::span<const VkDescriptorType> types) {
    std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
    for (std::uint32_t binding = 0; binding < types.size(); ++binding)
        bindings[binding] = {binding, types[binding], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<std::uint32_t>(types.size());
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout{};
    if (vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) != VK_SUCCESS)
        throw std::runtime_error("Could not create visibility buffer descriptor layout");
    return layout;
}

VkPipelineLayout makePipelineLayout(const VkDevice device, std::span<const VkDescriptorSetLayout> sets) {
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(std::uint32_t)};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = static_cast<std::uint32_t>(sets.size());
    info.pSetLayouts = sets.data();
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    VkPipelineLayout layout{};
    if (vkCreatePipelineLayout(device, &info, nullptr, &layout) != VK_SUCCESS)
        throw std::runtime_error("Could not create visibility buffer pipeline layout");
    return layout;
}

VkPipeline makePipeline(const VkDevice device, Assets::AssetManager& assets, const std::string& path,
                        const VkPipelineLayout layout) {
    const auto shader = Vkutil::loadShaderModule(device, assets, path);
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  nullptr,
                  0,
                  VK_SHADER_STAGE_COMPUTE_BIT,
                  shader.get(),
                  "main",
                  nullptr};
    info.layout = layout;
    VkPipeline pipeline{};
    if (vkCreateComputePipelines(device, PipelineCache::get(device), 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        throw std::runtime_error("Could not create visibility buffer pipeline " + path);
    return pipeline;
}
} // namespace

VisibilityBufferPass::~VisibilityBufferPass() {
    destroy();
}

void VisibilityBufferPass::create(const VkPhysicalDevice physical, const VkDevice device, const VkExtent2D extent,
                                  const VmaAllocator allocator, Assets::AssetManager& assets,
                                  const VkDescriptorSetLayout sceneLayout, const VkDescriptorSetLayout meshletDrawLayout) {
    if (!device || !extent.width || !extent.height || !sceneLayout || !meshletDrawLayout)
        throw std::invalid_argument("The visibility buffer requires the forward layouts and an extent");
    destroy();
    device_ = device;
    extent_ = extent;
    try {
        ids_.create(physical, device_, extent_, allocator, VK_FILTER_NEAREST, IdFormat);
        classTiles_.createDeviceLocalEmpty(device_, sizeof(std::uint32_t) * tileCapacity() * ClassCount,
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, allocator);
        classDispatch_.createDeviceLocalEmpty(device_, DispatchStride * DispatchSlots,
                                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                  VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                              allocator);
        // Every resolved pixel may queue its reflection.
        ssrRequests_.createDeviceLocalEmpty(device_, SsrRequestSize * extent_.width * extent_.height,
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, allocator);

        constexpr VkDescriptorType Sampled = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        constexpr VkDescriptorType Storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        constexpr VkDescriptorType Buffer = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        // visibility_classify.slang: ids, tile lists, dispatch arguments.
        classifyLayout_ = makeLayout(device_, std::array{Sampled, Buffer, Buffer});
        // forward_pbr.slang (FORWARD_VISIBILITY_RESOLVE) and visibility_ssr.slang
        // set 2: ids, HDR, deferred surface, tile lists, queued reflections and
        // the dispatch arguments.
        resolveLayout_ = makeLayout(device_, std::array{Sampled, Storage, Storage, Buffer, Buffer, Buffer});
        const std::array<VkDescriptorPoolSize, 3> sizes{{
            {Sampled, 1 + FramesInFlight},
            {Storage, FramesInFlight * 2},
            {Buffer, 2 + FramesInFlight * 3},
        }};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1 + FramesInFlight;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool_) != VK_SUCCESS)
            throw std::runtime_error("Could not create visibility buffer descriptor pool");
        std::array<VkDescriptorSetLayout, 1 + FramesInFlight> layouts{classifyLayout_, resolveLayout_, resolveLayout_};
        std::array<VkDescriptorSet, 1 + FramesInFlight> sets{};
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = pool_;
        allocation.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
        allocation.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(device_, &allocation, sets.data()) != VK_SUCCESS)
            throw std::runtime_error("Could not allocate visibility buffer descriptor sets");
        classifySet_ = sets[0];
        resolveSets_ = {sets[1], sets[2]};

        const VkDescriptorImageInfo idsInfo{VK_NULL_HANDLE, ids_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo tilesInfo{classTiles_.handle(), 0, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo dispatchInfo{classDispatch_.handle(), 0, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo requestsInfo{ssrRequests_.handle(), 0, VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet, 11> writes{{
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, classifySet_, 0, 0, 1, Sampled, &idsInfo},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, classifySet_, 1, 0, 1, Buffer, nullptr, &tilesInfo},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, classifySet_, 2, 0, 1, Buffer, nullptr, &dispatchInfo},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, resolveSets_[0], 0, 0, 1, Sampled, &idsInfo},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, resolveSets_[0], 3, 0, 1, Buffer, nullptr, &tilesInfo},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, resolveSets_[1], 0, 0, 1, Sampled, &idsInfo},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, resolveSets_[1], 3, 0, 1, Buffer, nullptr, &tilesInfo},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, resolveSets_[0], 4, 0, 1, Buffer, nullptr, &requestsInfo},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, resolveSets_[1], 4, 0, 1, Buffer, nullptr, &requestsInfo},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, resolveSets_[0], 5, 0, 1, Buffer, nullptr, &dispatchInfo},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, resolveSets_[1], 5, 0, 1, Buffer, nullptr, &dispatchInfo},
        }};
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

        classifyPipelineLayout_ = makePipelineLayout(device_, std::array{classifyLayout_});
        resolvePipelineLayout_ =
            makePipelineLayout(device_, std::array{sceneLayout, meshletDrawLayout, resolveLayout_});
        classifyPipeline_ = makePipeline(device_, assets, "shaders/visibility_classify.spv", classifyPipelineLayout_);
        for (std::uint32_t materialClass = 0; materialClass < ClassCount; ++materialClass)
            resolvePipelines_[materialClass] =
                makePipeline(device_, assets,
                             "shaders/visibility_resolve_class" + std::to_string(materialClass) + ".spv",
                             resolvePipelineLayout_);
        reflectionPipeline_ = makePipeline(device_, assets, "shaders/visibility_ssr.spv", resolvePipelineLayout_);
    } catch (...) {
        destroy();
        throw;
    }
}

void VisibilityBufferPass::destroy() noexcept {
    if (device_) {
        for (VkPipeline pipeline : resolvePipelines_)
            if (pipeline)
                vkDestroyPipeline(device_, pipeline, nullptr);
        for (VkPipeline pipeline : {classifyPipeline_, reflectionPipeline_})
            if (pipeline)
                vkDestroyPipeline(device_, pipeline, nullptr);
        for (VkPipelineLayout layout : {classifyPipelineLayout_, resolvePipelineLayout_})
            if (layout)
                vkDestroyPipelineLayout(device_, layout, nullptr);
        if (pool_)
            vkDestroyDescriptorPool(device_, pool_, nullptr);
        for (VkDescriptorSetLayout layout : {classifyLayout_, resolveLayout_})
            if (layout)
                vkDestroyDescriptorSetLayout(device_, layout, nullptr);
    }
    ids_.destroy();
    classTiles_.destroy();
    classDispatch_.destroy();
    ssrRequests_.destroy();
    resolvePipelines_ = {};
    classifyPipeline_ = reflectionPipeline_ = VK_NULL_HANDLE;
    classifyPipelineLayout_ = resolvePipelineLayout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    classifyLayout_ = resolveLayout_ = VK_NULL_HANDLE;
    classifySet_ = VK_NULL_HANDLE;
    resolveSets_ = {};
    extent_ = {};
    device_ = VK_NULL_HANDLE;
}

void VisibilityBufferPass::prepare(const VkCommandBuffer cmd) const {
    if (ids_.image() == VK_NULL_HANDLE)
        return;
    // The previous frame's classify and resolve read the ids; the prepass clears them.
    const std::array barriers{imageBarrier(ids_.image(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                                           VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)};
    submitBarriers(cmd, barriers);
}

bool VisibilityBufferPass::record(const VkCommandBuffer cmd, const std::uint32_t frame, const VkDescriptorSet sceneSet,
                                  const VkDescriptorSet meshletDrawSet, const Targets& targets) {
    if (!ready() || frame >= FramesInFlight || !sceneSet || !meshletDrawSet || !targets.hdr || !targets.surface)
        return false;
    const VkDescriptorImageInfo hdrInfo{VK_NULL_HANDLE, targets.hdrView, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo surfaceInfo{VK_NULL_HANDLE, targets.surfaceView, VK_IMAGE_LAYOUT_GENERAL};
    const std::array<VkWriteDescriptorSet, 2> writes{{
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, resolveSets_[frame], 1, 0, 1,
         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &hdrInfo},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, resolveSets_[frame], 2, 0, 1,
         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &surfaceInfo},
    }};
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    constexpr VkPipelineStageFlags2 Attachment = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    constexpr VkPipelineStageFlags2 Compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr VkPipelineStageFlags2 Indirect = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    constexpr VkAccessFlags2 AttachmentWrite = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    constexpr VkAccessFlags2 StorageRead = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    constexpr VkAccessFlags2 StorageWrite = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

    // 1. Reset the per-class tile counts (the previous frame's dispatches
    //    have read them) and expose the prepass ids.
    const VkMemoryBarrier2 beforeReset =
        memoryBarrier(Compute | Indirect, StorageRead | StorageWrite | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    submitBarriers(cmd, {}, &beforeReset);
    std::array<std::uint32_t, 4 * DispatchSlots> dispatch{};
    for (std::uint32_t slot = 0; slot < DispatchSlots; ++slot)
        dispatch[slot * 4 + 1] = dispatch[slot * 4 + 2] = 1;
    vkCmdUpdateBuffer(cmd, classDispatch_.handle(), 0, sizeof(dispatch), dispatch.data());
    const VkMemoryBarrier2 afterReset = memoryBarrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                      Compute, StorageRead | StorageWrite);
    const std::array idsReadable{imageBarrier(ids_.image(), Attachment, AttachmentWrite,
                                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, Compute,
                                              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)};
    submitBarriers(cmd, idsReadable, &afterReset);

    // 2. Classify the tiles.
    const std::uint32_t capacity = tileCapacity();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, classifyPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, classifyPipelineLayout_, 0, 1, &classifySet_, 0,
                            nullptr);
    vkCmdPushConstants(cmd, classifyPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(capacity), &capacity);
    vkCmdDispatch(cmd, tilesX(), tilesY(), 1);

    // 3. Tile lists -> indirect arguments and resolve; the lighting pass
    //    cleared both targets and shaded the other slots.
    const VkMemoryBarrier2 classified =
        memoryBarrier(Compute, StorageWrite, Compute | Indirect, StorageRead | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    const std::array targetsWritable{
        imageBarrier(targets.hdr, Attachment, AttachmentWrite, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, Compute,
                     StorageWrite, VK_IMAGE_LAYOUT_GENERAL),
        imageBarrier(targets.surface, Attachment, AttachmentWrite, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, Compute,
                     StorageWrite, VK_IMAGE_LAYOUT_GENERAL),
    };
    submitBarriers(cmd, targetsWritable, &classified);

    // 4. One indirect dispatch per material class over its tiles. The
    //    classes write disjoint pixels, so they need no barrier between them.
    const std::array sets{sceneSet, meshletDrawSet, resolveSets_[frame]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolvePipelineLayout_, 0,
                            static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdPushConstants(cmd, resolvePipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(capacity), &capacity);
    for (std::uint32_t materialClass = 0; materialClass < ClassCount; ++materialClass) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolvePipelines_[materialClass]);
        vkCmdDispatchIndirect(cmd, classDispatch_.handle(), DispatchStride * materialClass);
    }
    return true;
}

void VisibilityBufferPass::recordReflections(const VkCommandBuffer cmd, const Targets& targets) const {
    constexpr VkPipelineStageFlags2 Attachment = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    constexpr VkPipelineStageFlags2 Compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr VkPipelineStageFlags2 Indirect = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    constexpr VkAccessFlags2 AttachmentWrite = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    constexpr VkAccessFlags2 StorageRead = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    constexpr VkAccessFlags2 StorageWrite = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

    // 5. Trace the queued reflections. The resolve wrote the requests, their
    //    count and dispatch size, and the HDR and surface texels they read.
    //    The sets bound by record() stay bound.
    const VkMemoryBarrier2 queued = memoryBarrier(Compute, StorageWrite, Compute | Indirect,
                                                  StorageRead | StorageWrite | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    submitBarriers(cmd, {}, &queued);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, reflectionPipeline_);
    vkCmdDispatchIndirect(cmd, classDispatch_.handle(), DispatchStride * ClassCount);

    // 6. Back to attachments: the deferred local light pass expects them
    //    there and reads both after its own attachment-to-storage barrier.
    constexpr VkAccessFlags2 Consumers =
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | AttachmentWrite | StorageRead | StorageWrite;
    const std::array targetsBack{
        imageBarrier(targets.hdr, Compute, StorageWrite, VK_IMAGE_LAYOUT_GENERAL, Attachment | Compute, Consumers,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
        imageBarrier(targets.surface, Compute, StorageWrite, VK_IMAGE_LAYOUT_GENERAL, Attachment | Compute, Consumers,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
    };
    submitBarriers(cmd, targetsBack);
}
} // namespace Engine
