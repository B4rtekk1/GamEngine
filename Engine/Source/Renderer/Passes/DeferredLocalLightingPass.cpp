#include "Engine/Renderer/Passes/DeferredLocalLightingPass.h"

#include "Engine/Renderer/Vulkan/PipelineCache.h"
#include "Engine/Renderer/shader_loader.h"

#include <array>
#include <vector>
#include <stdexcept>

namespace Engine {
namespace {
constexpr std::uint32_t FramesInFlight = 2;

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
} // namespace

DeferredLocalLightingPass::~DeferredLocalLightingPass() {
    destroy();
}

void DeferredLocalLightingPass::create(const VkPhysicalDevice physical, const VkDevice device,
                                       const VkExtent2D extent, const VmaAllocator allocator,
                                       Assets::AssetManager& assets, const std::span<const VkBuffer> ubos,
                                       const bool stochastic) {
    if (!device || !extent.width || !extent.height || ubos.size() != FramesInFlight)
        throw std::invalid_argument("Deferred local lighting requires two frame UBOs and an extent");
    destroy();
    device_ = device;
    extent_ = extent;
    try {
        surface_.create(physical, device_, extent_, allocator, VK_FILTER_NEAREST, SurfaceFormat, true);
        constexpr VkShaderStageFlags Compute = VK_SHADER_STAGE_COMPUTE_BIT;
        const std::array<VkDescriptorSetLayoutBinding, 7> bindings{{
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, Compute, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, Compute, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, Compute, nullptr},
            {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, Compute, nullptr},
            {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, Compute, nullptr},
            {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, Compute, nullptr},
            {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, Compute, nullptr},
        }};
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &layout_) != VK_SUCCESS)
            throw std::runtime_error("Could not create deferred local lighting descriptor layout");
        const std::array<VkDescriptorPoolSize, 4> sizes{{
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FramesInFlight * 2},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, FramesInFlight * 2},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FramesInFlight},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, FramesInFlight * 2},
        }};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = FramesInFlight;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool_) != VK_SUCCESS)
            throw std::runtime_error("Could not create deferred local lighting descriptor pool");
        std::array<VkDescriptorSetLayout, FramesInFlight> layouts{};
        layouts.fill(layout_);
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = pool_;
        allocation.descriptorSetCount = FramesInFlight;
        allocation.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(device_, &allocation, sets_.data()) != VK_SUCCESS)
            throw std::runtime_error("Could not allocate deferred local lighting descriptor sets");
        for (std::uint32_t frame = 0; frame < FramesInFlight; ++frame) {
            const VkDescriptorImageInfo surface{VK_NULL_HANDLE, surface_.imageView(), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorBufferInfo ubo{ubos[frame], 0, VK_WHOLE_SIZE};
            const std::array<VkWriteDescriptorSet, 2> writes{{
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 1, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &surface},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 3, 0, 1,
                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubo},
            }};
            vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &layout_;
        if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS)
            throw std::runtime_error("Could not create deferred local lighting pipeline layout");
        const auto shader = Vkutil::loadShaderModule(device_, assets, "shaders/deferred_local_lights.spv");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                              nullptr,
                              0,
                              VK_SHADER_STAGE_COMPUTE_BIT,
                              shader.get(),
                              "main",
                              nullptr};
        pipelineInfo.layout = pipelineLayout_;
        if (vkCreateComputePipelines(device_, PipelineCache::get(device_), 1, &pipelineInfo, nullptr, &pipeline_) !=
            VK_SUCCESS)
            throw std::runtime_error("Could not create deferred local lighting pipeline");
        // The stochastic path owns five full-resolution RGBA16F targets.
        if (stochastic)
            createStochastic(physical, allocator, assets, ubos);
    } catch (...) {
        destroy();
        throw;
    }
}

void DeferredLocalLightingPass::prepare(const VkCommandBuffer cmd) const {
    if (surface_.image() == VK_NULL_HANDLE)
        return;
    // The previous frame's compute pass read it; the lighting pass clears it.
    const VkImageMemoryBarrier2 barrier =
        imageBarrier(surface_.image(),
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void DeferredLocalLightingPass::record(const VkCommandBuffer cmd, const std::uint32_t frame, const Inputs& inputs) {
    if (!pipeline_ || frame >= FramesInFlight || !inputs.depth || !inputs.hdr || !inputs.localLights ||
        !inputs.tileLights || !inputs.localShadows)
        return;
    const VkDescriptorImageInfo depthInfo{inputs.depthSampler, inputs.depth,
                                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo hdrInfo{VK_NULL_HANDLE, inputs.hdrView, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo lightsInfo{inputs.localLights, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo tilesInfo{inputs.tileLights, 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo shadowInfo{inputs.localShadowSampler, inputs.localShadows,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const std::array<VkWriteDescriptorSet, 5> writes{{
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 0, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         &hdrInfo},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 4, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         nullptr, &lightsInfo},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 5, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         nullptr, &tilesInfo},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 6, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shadowInfo},
    }};
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    constexpr VkPipelineStageFlags2 Attachment = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    constexpr VkPipelineStageFlags2 Compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const std::array before{
        imageBarrier(inputs.hdr, Attachment, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, Compute,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL),
        imageBarrier(surface_.image(), Attachment, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, Compute, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                     VK_IMAGE_LAYOUT_GENERAL),
    };
    // Tile lists and the denoised shadows were written by earlier compute
    // dispatches whose barriers targeted only the fragment stage.
    VkMemoryBarrier2 computeResults{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    // (The stochastic samples may come from the ray tracing stage.)
    computeResults.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    computeResults.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    computeResults.dstStageMask = Compute;
    computeResults.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    VkDependencyInfo beforeDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    beforeDependency.memoryBarrierCount = 1;
    beforeDependency.pMemoryBarriers = &computeResults;
    beforeDependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(before.size());
    beforeDependency.pImageMemoryBarriers = before.data();
    vkCmdPipelineBarrier2(cmd, &beforeDependency);

    if (inputs.lightSamples && inputs.lightSampleWeights && stochasticReady()) {
        recordStochastic(cmd, frame, inputs);
    } else {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &sets_[frame], 0,
                                nullptr);
        vkCmdDispatch(cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
    }

    // Back to the attachment layout the lighting pass left, for the
    // barriers and passes that follow it.
    const std::array after{
        imageBarrier(inputs.hdr, Compute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                     Attachment | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | Compute |
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
    };
    VkDependencyInfo afterDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    afterDependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(after.size());
    afterDependency.pImageMemoryBarriers = after.data();
    vkCmdPipelineBarrier2(cmd, &afterDependency);
}

void DeferredLocalLightingPass::destroy() noexcept {
    if (device_) {
        if (pipeline_)
            vkDestroyPipeline(device_, pipeline_, nullptr);
        if (pipelineLayout_)
            vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (pool_)
            vkDestroyDescriptorPool(device_, pool_, nullptr);
        if (layout_)
            vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
    }
    if (device_) {
        for (VkPipeline pipeline : {shadePipeline_, temporalPipeline_})
            if (pipeline)
                vkDestroyPipeline(device_, pipeline, nullptr);
        for (VkPipelineLayout layout : {shadePipelineLayout_, temporalPipelineLayout_})
            if (layout)
                vkDestroyPipelineLayout(device_, layout, nullptr);
        if (stochasticPool_)
            vkDestroyDescriptorPool(device_, stochasticPool_, nullptr);
        for (VkDescriptorSetLayout layout : {shadeLayout_, temporalLayout_})
            if (layout)
                vkDestroyDescriptorSetLayout(device_, layout, nullptr);
    }
    shadePipeline_ = temporalPipeline_ = VK_NULL_HANDLE;
    shadePipelineLayout_ = temporalPipelineLayout_ = VK_NULL_HANDLE;
    stochasticPool_ = VK_NULL_HANDLE;
    shadeLayout_ = temporalLayout_ = VK_NULL_HANDLE;
    shadeSets_ = {};
    temporalSets_ = {};
    noisyDiffuse_.destroy();
    noisySpecular_.destroy();
    for (auto& history : historyDiffuse_)
        history.destroy();
    for (auto& history : historySpecular_)
        history.destroy();
    historyDefined_ = {};
    surface_.destroy();
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    sets_ = {};
    extent_ = {};
    device_ = VK_NULL_HANDLE;
}

void DeferredLocalLightingPass::createStochastic(const VkPhysicalDevice physical, const VmaAllocator allocator,
                                                 Assets::AssetManager& assets,
                                                 const std::span<const VkBuffer> ubos) {
    constexpr VkFormat LightingFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    noisyDiffuse_.create(physical, device_, extent_, allocator, VK_FILTER_NEAREST, LightingFormat, true);
    noisySpecular_.create(physical, device_, extent_, allocator, VK_FILTER_NEAREST, LightingFormat, true);
    for (auto& history : historyDiffuse_)
        history.create(physical, device_, extent_, allocator, VK_FILTER_NEAREST, LightingFormat, true);
    for (auto& history : historySpecular_)
        history.create(physical, device_, extent_, allocator, VK_FILTER_NEAREST, LightingFormat, true);
    historyDefined_ = {};

    constexpr VkShaderStageFlags Compute = VK_SHADER_STAGE_COMPUTE_BIT;
    const auto makeLayout = [&](std::span<const VkDescriptorType> types, VkDescriptorSetLayout& layout) {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (std::uint32_t binding = 0; binding < types.size(); ++binding)
            bindings.push_back({binding, types[binding], 1, Compute, nullptr});
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device_, &info, nullptr, &layout) != VK_SUCCESS)
            throw std::runtime_error("Could not create stochastic local lighting descriptor layout");
    };
    constexpr VkDescriptorType Sampled = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    constexpr VkDescriptorType Storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    constexpr VkDescriptorType Uniform = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    constexpr VkDescriptorType Buffer = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    // See Lighting/megalights_shade.slang and megalights_temporal.slang.
    constexpr std::array<VkDescriptorType, 8> shadeTypes{Sampled, Storage, Storage, Uniform,
                                                         Buffer,  Storage, Storage, Storage};
    constexpr std::array<VkDescriptorType, 10> temporalTypes{Sampled, Storage, Storage, Uniform, Storage,
                                                             Sampled, Sampled, Storage, Storage, Storage};
    makeLayout(shadeTypes, shadeLayout_);
    makeLayout(temporalTypes, temporalLayout_);
    const std::array<VkDescriptorPoolSize, 4> sizes{{
        {Sampled, FramesInFlight * 4},
        {Storage, FramesInFlight * 13},
        {Uniform, FramesInFlight * 2},
        {Buffer, FramesInFlight},
    }};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = FramesInFlight * 2;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &stochasticPool_) != VK_SUCCESS)
        throw std::runtime_error("Could not create stochastic local lighting descriptor pool");
    const auto allocate = [&](VkDescriptorSetLayout layout, std::array<VkDescriptorSet, 2>& sets) {
        std::array<VkDescriptorSetLayout, FramesInFlight> layouts{};
        layouts.fill(layout);
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = stochasticPool_;
        allocation.descriptorSetCount = FramesInFlight;
        allocation.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(device_, &allocation, sets.data()) != VK_SUCCESS)
            throw std::runtime_error("Could not allocate stochastic local lighting descriptor sets");
    };
    allocate(shadeLayout_, shadeSets_);
    allocate(temporalLayout_, temporalSets_);
    for (std::uint32_t frame = 0; frame < FramesInFlight; ++frame) {
        const std::uint32_t previous = (frame + 1) % FramesInFlight;
        const VkDescriptorImageInfo surface{VK_NULL_HANDLE, surface_.imageView(), VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo diffuse{VK_NULL_HANDLE, noisyDiffuse_.imageView(), VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo specular{VK_NULL_HANDLE, noisySpecular_.imageView(), VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorBufferInfo ubo{ubos[frame], 0, VK_WHOLE_SIZE};
        const VkDescriptorImageInfo historyDiffuse{historyDiffuse_[previous].sampler(),
                                                   historyDiffuse_[previous].imageView(),
                                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo historySpecular{historySpecular_[previous].sampler(),
                                                    historySpecular_[previous].imageView(),
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo outDiffuse{VK_NULL_HANDLE, historyDiffuse_[frame].imageView(),
                                               VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo outSpecular{VK_NULL_HANDLE, historySpecular_[frame].imageView(),
                                                VK_IMAGE_LAYOUT_GENERAL};
        const auto write = [](VkDescriptorSet set, std::uint32_t binding, VkDescriptorType type,
                              const VkDescriptorImageInfo* image, const VkDescriptorBufferInfo* buffer) {
            return VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, 1, type,
                                        image, buffer, nullptr};
        };
        const std::array writes{
            write(shadeSets_[frame], 1, Storage, &surface, nullptr),
            write(shadeSets_[frame], 2, Storage, &diffuse, nullptr),
            write(shadeSets_[frame], 3, Uniform, nullptr, &ubo),
            write(shadeSets_[frame], 7, Storage, &specular, nullptr),
            write(temporalSets_[frame], 1, Storage, &surface, nullptr),
            write(temporalSets_[frame], 2, Storage, &diffuse, nullptr),
            write(temporalSets_[frame], 3, Uniform, nullptr, &ubo),
            write(temporalSets_[frame], 4, Storage, &specular, nullptr),
            write(temporalSets_[frame], 5, Sampled, &historyDiffuse, nullptr),
            write(temporalSets_[frame], 6, Sampled, &historySpecular, nullptr),
            write(temporalSets_[frame], 7, Storage, &outDiffuse, nullptr),
            write(temporalSets_[frame], 8, Storage, &outSpecular, nullptr),
        };
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    const auto makePipeline = [&](VkDescriptorSetLayout layout, const char* path, VkPipelineLayout& pipelineLayout,
                                  VkPipeline& pipeline) {
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &layout;
        if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
            throw std::runtime_error("Could not create stochastic local lighting pipeline layout");
        const auto shader = Vkutil::loadShaderModule(device_, assets, path);
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                      shader.get(), "main", nullptr};
        info.layout = pipelineLayout;
        if (vkCreateComputePipelines(device_, PipelineCache::get(device_), 1, &info, nullptr, &pipeline) !=
            VK_SUCCESS)
            throw std::runtime_error("Could not create stochastic local lighting pipeline");
    };
    makePipeline(shadeLayout_, "shaders/megalights_shade.spv", shadePipelineLayout_, shadePipeline_);
    makePipeline(temporalLayout_, "shaders/megalights_temporal.spv", temporalPipelineLayout_, temporalPipeline_);
}

void DeferredLocalLightingPass::recordStochastic(const VkCommandBuffer cmd, const std::uint32_t frame,
                                                 const Inputs& inputs) {
    const std::uint32_t previous = (frame + 1) % FramesInFlight;
    constexpr VkPipelineStageFlags2 Compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    // A history slot that was never written enters as "no history" (zero).
    if (!historyDefined_[previous]) {
        const std::array toTransfer{
            imageBarrier(historyDiffuse_[previous].image(), Compute, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
            imageBarrier(historySpecular_[previous].image(), Compute, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        };
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(toTransfer.size());
        dependency.pImageMemoryBarriers = toTransfer.data();
        vkCmdPipelineBarrier2(cmd, &dependency);
        const VkClearColorValue zero{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, historyDiffuse_[previous].image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                             &range);
        vkCmdClearColorImage(cmd, historySpecular_[previous].image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                             &range);
        const std::array toRead{
            imageBarrier(historyDiffuse_[previous].image(), VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, Compute,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            imageBarrier(historySpecular_[previous].image(), VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, Compute,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        };
        dependency.pImageMemoryBarriers = toRead.data();
        vkCmdPipelineBarrier2(cmd, &dependency);
        historyDefined_[previous] = true;
    }
    const VkDescriptorImageInfo depthInfo{inputs.depthSampler, inputs.depth,
                                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo lightsInfo{inputs.localLights, 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo samplesInfo{VK_NULL_HANDLE, inputs.lightSamples, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo weightsInfo{VK_NULL_HANDLE, inputs.lightSampleWeights, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo hdrInfo{VK_NULL_HANDLE, inputs.hdrView, VK_IMAGE_LAYOUT_GENERAL};
    const std::array writes{
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, shadeSets_[frame], 0, 0, 1,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo, nullptr, nullptr},
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, shadeSets_[frame], 4, 0, 1,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &lightsInfo, nullptr},
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, shadeSets_[frame], 5, 0, 1,
                             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &samplesInfo, nullptr, nullptr},
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, shadeSets_[frame], 6, 0, 1,
                             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &weightsInfo, nullptr, nullptr},
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, temporalSets_[frame], 0, 0, 1,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo, nullptr, nullptr},
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, temporalSets_[frame], 9, 0, 1,
                             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &hdrInfo, nullptr, nullptr},
    };
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    // The previous frame's temporal step read the noisy images and this
    // slot's history (as its "previous"); both are rewritten here.
    const std::array before{
        imageBarrier(noisyDiffuse_.image(), Compute, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED, Compute,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL),
        imageBarrier(noisySpecular_.image(), Compute, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED, Compute,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL),
        imageBarrier(historyDiffuse_[frame].image(), Compute, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED, Compute,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL),
        imageBarrier(historySpecular_[frame].image(), Compute, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED, Compute,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL),
    };
    VkDependencyInfo beforeDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    beforeDependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(before.size());
    beforeDependency.pImageMemoryBarriers = before.data();
    vkCmdPipelineBarrier2(cmd, &beforeDependency);

    const std::uint32_t groupsX = (extent_.width + 7) / 8;
    const std::uint32_t groupsY = (extent_.height + 7) / 8;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shadePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shadePipelineLayout_, 0, 1, &shadeSets_[frame], 0,
                            nullptr);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    VkMemoryBarrier2 shaded{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    shaded.srcStageMask = Compute;
    shaded.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    shaded.dstStageMask = Compute;
    shaded.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo shadedDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    shadedDependency.memoryBarrierCount = 1;
    shadedDependency.pMemoryBarriers = &shaded;
    vkCmdPipelineBarrier2(cmd, &shadedDependency);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, temporalPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, temporalPipelineLayout_, 0, 1,
                            &temporalSets_[frame], 0, nullptr);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // Next frame samples this slot's accumulation as its history.
    const std::array after{
        imageBarrier(historyDiffuse_[frame].image(), Compute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, Compute, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        imageBarrier(historySpecular_[frame].image(), Compute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, Compute, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
    };
    VkDependencyInfo afterDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    afterDependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(after.size());
    afterDependency.pImageMemoryBarriers = after.data();
    vkCmdPipelineBarrier2(cmd, &afterDependency);
    historyDefined_[frame] = true;
}
} // namespace Engine
