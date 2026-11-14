#include "Engine/Renderer/Passes/SsgiPass.h"

#include "Engine/Renderer/Vulkan/PipelineCache.h"
#include "Engine/Renderer/shader_loader.h"

#include <algorithm>
#include <array>
#include <glm/glm.hpp>
#include <stdexcept>

namespace Engine {
namespace {
class GpuLabel final {
  public:
    GpuLabel(VkCommandBuffer commandBuffer, PFN_vkCmdBeginDebugUtilsLabelEXT begin, PFN_vkCmdEndDebugUtilsLabelEXT end,
             const char* name)
        : commandBuffer_(commandBuffer), end_(begin && end ? end : nullptr) {
        if (end_) {
            const VkDebugUtilsLabelEXT label{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
                                             .pLabelName = name,
                                             .color = {0.9F, 0.6F, 0.2F, 1.0F}};
            begin(commandBuffer_, &label);
        }
    }
    ~GpuLabel() {
        if (end_)
            end_(commandBuffer_);
    }
    GpuLabel(const GpuLabel&) = delete;
    GpuLabel& operator=(const GpuLabel&) = delete;

  private:
    VkCommandBuffer commandBuffer_;
    PFN_vkCmdEndDebugUtilsLabelEXT end_;
};

struct CaptureSettings {
    float maxLuminance;
};

struct PrepareSettings {
    glm::vec4 depthToView;
};

struct TraceSettings {
    glm::mat4 viewToPreviousClip;
    glm::vec4 depthToView;
    glm::vec2 projectionScale;
    float radiusView, thickness;
    std::uint32_t frameIndex, sliceCount, stepCount;
    float maxRadiusPixels;
    std::uint32_t radianceValid;
    float padding[3];
};
static_assert(sizeof(TraceSettings) == 128, "SSGI trace push constants must fit the guaranteed 128 bytes");

struct TemporalSettings {
    glm::mat4 viewToPreviousClip;
    glm::vec2 projectionScale;
    std::uint32_t historyValid;
    float currentWeight;
};

struct ResolveSettings {
    glm::vec4 depthToView;
    float intensity;
    float padding[3];
};

constexpr VkFormat GiFormat = HdrBuffer::Format;
// Roughly a sixteen-frame exponential history: low enough noise for two
// slices, short enough that moving lights settle within a quarter second.
constexpr float TemporalCurrentWeight = 0.06F;
// Diffuse emitters in HDR rarely exceed this; the sun disc and specular
// highlights would otherwise produce isolated bright taps.
constexpr float CaptureMaxLuminance = 16.0F;
// Fraction of the trace width a slice may reach; bounds cache misses up close.
constexpr float MaxRadiusScreenFraction = 0.12F;

constexpr VkPipelineStageFlags2 ComputeStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkAccessFlags2 ShaderWrite = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
constexpr VkAccessFlags2 ShaderRead = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
constexpr VkDescriptorType CombinedSampler = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           StorageImage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .srcStageMask = srcStage,
                                   .srcAccessMask = srcAccess,
                                   .dstStageMask = dstStage,
                                   .dstAccessMask = dstAccess};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(cmd, &dependency);
}

VkImageMemoryBarrier2 layoutBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

VkPipeline makeCompute(VkDevice device, Assets::AssetManager& assets, const char* path, VkPipelineLayout layout) {
    auto shader = Vkutil::loadShaderModule(device, assets, path);
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader.get();
    stage.pName = "main";
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = stage;
    info.layout = layout;
    VkPipeline pipeline{};
    if (vkCreateComputePipelines(device, PipelineCache::get(device), 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        throw std::runtime_error("Could not create SSGI compute pipeline");
    return pipeline;
}

glm::vec4 depthToView(const glm::mat4& inverseProjection) {
    // Only view.z and view.w of inverseProjection * (0, 0, z, 1) matter.
    return {inverseProjection[2][2], inverseProjection[3][2], inverseProjection[2][3], inverseProjection[3][3]};
}

std::uint32_t groups(std::uint32_t size) {
    return (size + 7) / 8;
}
} // namespace

SsgiPass::~SsgiPass() {
    destroy();
}

void SsgiPass::create(VkPhysicalDevice physical, VkDevice device, VkExtent2D fullExtent, VmaAllocator allocator,
                      Assets::AssetManager& assets, const std::span<const std::uint32_t> sharingFamilies) {
    if (!device || !fullExtent.width || !fullExtent.height)
        throw std::invalid_argument("SSGI requires a valid extent");
    destroy();
    device_ = device;
    beginLabel_ = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(device_, "vkCmdBeginDebugUtilsLabelEXT"));
    endLabel_ =
        reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(vkGetDeviceProcAddr(device_, "vkCmdEndDebugUtilsLabelEXT"));
    fullExtent_ = fullExtent;
    halfExtent_ = {std::max(1u, (fullExtent.width + 1) / 2), std::max(1u, (fullExtent.height + 1) / 2)};
    try {
        // The radiance capture runs on graphics while the trace may run on
        // async compute, and both histories persist across frames, so every
        // image but the per-frame raw trace is shared between the families.
        radiance_.create(physical, device_, halfExtent_, allocator, VK_FILTER_LINEAR, GiFormat, true, sharingFamilies);
        raw_.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, GiFormat, true);
        geometry_.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, VK_FORMAT_R32G32_UINT, true);
        for (HdrBuffer& history : history_)
            history.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, GiFormat, true,
                           sharingFamilies);
        full_.create(physical, device_, fullExtent_, allocator, VK_FILTER_LINEAR, GiFormat, true, sharingFamilies);

        createKernel(Kernel::Capture, std::array{CombinedSampler, StorageImage}, sizeof(CaptureSettings), assets,
                     "shaders/ssgi_capture.spv");
        createKernel(Kernel::Prepare, std::array{CombinedSampler, CombinedSampler, StorageImage},
                     sizeof(PrepareSettings), assets, "shaders/ssgi_prepare.spv");
        createKernel(Kernel::Trace, std::array{StorageImage, CombinedSampler, StorageImage}, sizeof(TraceSettings),
                     assets, "shaders/ssgi_trace.spv");
        createKernel(Kernel::Temporal, std::array{CombinedSampler, CombinedSampler, StorageImage},
                     sizeof(TemporalSettings), assets, "shaders/ssgi_temporal.spv");
        createKernel(Kernel::Resolve, std::array{CombinedSampler, CombinedSampler, StorageImage},
                     sizeof(ResolveSettings), assets, "shaders/ssgi_resolve.spv");

        const auto variants = [](std::size_t k) {
            const auto id = static_cast<Kernel>(k);
            return id == Kernel::Temporal || id == Kernel::Resolve ? MaxVariants : 1u;
        };
        std::array<VkDescriptorPoolSize, 2> poolSizes{{{CombinedSampler, 0}, {StorageImage, 0}}};
        std::uint32_t setCount = 0;
        for (std::size_t k = 0; k < KernelCount; ++k) {
            const std::uint32_t sets = variants(k) * FramesInFlight;
            setCount += sets;
            for (std::uint32_t b = 0; b < kernels_[k].bindingCount; ++b)
                for (auto& size : poolSizes)
                    if (size.type == kernels_[k].bindingTypes[b])
                        size.descriptorCount += sets;
        }
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = setCount;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS)
            throw std::runtime_error("Could not create SSGI descriptor pool");
        for (std::size_t k = 0; k < KernelCount; ++k)
            for (std::uint32_t frame = 0; frame < FramesInFlight; ++frame)
                for (std::uint32_t variant = 0; variant < variants(k); ++variant) {
                    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                    ai.descriptorPool = descriptorPool_;
                    ai.descriptorSetCount = 1;
                    ai.pSetLayouts = &kernels_[k].setLayout;
                    if (vkAllocateDescriptorSets(device_, &ai, &slots_[k][frame][variant].set) != VK_SUCCESS)
                        throw std::runtime_error("Could not allocate SSGI descriptor sets");
                }
    } catch (...) {
        destroy();
        throw;
    }
}

void SsgiPass::createKernel(const Kernel id, const std::span<const VkDescriptorType> bindings,
                            const std::uint32_t pushConstantSize, Assets::AssetManager& assets, const char* shader) {
    ComputeKernel& k = kernel(id);
    std::array<VkDescriptorSetLayoutBinding, MaxBindings> layoutBindings{};
    for (std::uint32_t b = 0; b < bindings.size(); ++b) {
        layoutBindings[b] = {b, bindings[b], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        k.bindingTypes[b] = bindings[b];
    }
    k.bindingCount = static_cast<std::uint32_t>(bindings.size());
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = k.bindingCount;
    layoutInfo.pBindings = layoutBindings.data();
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &k.setLayout) != VK_SUCCESS)
        throw std::runtime_error("Could not create SSGI descriptor set layout");
    const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushConstantSize};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &k.setLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &k.pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("Could not create SSGI pipeline layout");
    k.pipeline = makeCompute(device_, assets, shader, k.pipelineLayout);
}

void SsgiPass::bindKernel(VkCommandBuffer cmd, const Kernel id, const std::uint32_t frame, const std::uint32_t variant,
                          const std::span<const VkDescriptorImageInfo> images, const void* pushConstants,
                          const std::uint32_t pushConstantSize) {
    const ComputeKernel& k = kernel(id);
    if (images.size() != k.bindingCount)
        throw std::logic_error("SSGI descriptor count does not match its kernel layout");
    DescriptorSlot& slot = slots_[static_cast<std::size_t>(id)][frame][variant];
    const auto same = [](const VkDescriptorImageInfo& a, const VkDescriptorImageInfo& b) {
        return a.sampler == b.sampler && a.imageView == b.imageView && a.imageLayout == b.imageLayout;
    };
    if (!slot.valid || !std::equal(images.begin(), images.end(), slot.written.begin(), same)) {
        std::array<VkWriteDescriptorSet, MaxBindings> writes{};
        for (std::uint32_t b = 0; b < k.bindingCount; ++b)
            writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                         nullptr,
                         slot.set,
                         b,
                         0,
                         1,
                         k.bindingTypes[b],
                         &images[b],
                         nullptr,
                         nullptr};
        vkUpdateDescriptorSets(device_, k.bindingCount, writes.data(), 0, nullptr);
        std::copy(images.begin(), images.end(), slot.written.begin());
        slot.valid = true;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipelineLayout, 0, 1, &slot.set, 0, nullptr);
    vkCmdPushConstants(cmd, k.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushConstantSize, pushConstants);
}

void SsgiPass::clearImages(VkCommandBuffer cmd, const bool graphOwnsResultState) {
    if (initialized_)
        return;
    // Internal images rest in GENERAL; the result rests in SHADER_READ_ONLY,
    // the state the render graph imports and Forward samples. When the graph
    // already transitions the result this frame, it is left to the resolve.
    std::array<VkImage, 6> images{radiance_.image(), raw_.image(), geometry_.image(), history_[0].image(),
                                  history_[1].image(), full_.image()};
    const std::size_t imageCount = graphOwnsResultState ? images.size() - 1 : images.size();
    std::array<VkImageMemoryBarrier2, images.size()> barriers{};
    for (std::size_t i = 0; i < imageCount; ++i)
        barriers[i] = layoutBarrier(images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(imageCount);
    dependency.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dependency);
    const VkClearColorValue black{{0, 0, 0, 0}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (std::size_t i = 0; i < imageCount; ++i)
        vkCmdClearColorImage(cmd, images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    for (std::size_t i = 0; i < imageCount; ++i) {
        const bool result = images[i] == full_.image();
        barriers[i] = layoutBarrier(
            images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            result ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            result ? ComputeStage | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : ComputeStage,
            result ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : ShaderRead | ShaderWrite);
    }
    vkCmdPipelineBarrier2(cmd, &dependency);
    initialized_ = true;
    historyValid_ = false;
    radianceValid_ = false;
}

void SsgiPass::record(VkCommandBuffer cmd, const std::uint32_t frame, const SsgiFrameInputs& inputs,
                      const SsgiSettings& settings) {
    if (frame >= FramesInFlight)
        throw std::out_of_range("Invalid SSGI frame slot");
    const GpuLabel ssgiLabel{cmd, beginLabel_, endLabel_, "SSGI"};
    if (!initialized_)
        clearImages(cmd, true);
    const SsgiQualitySettings quality = ssgiQualitySettings(settings.quality);
    const glm::mat4& projection = inputs.projection.native();
    const glm::mat4 inverseProjection = glm::inverse(projection);
    const glm::vec2 projectionScale{projection[0][0], projection[1][1]};
    // Orders this trace after the previous frame's capture and resolve on
    // this queue, and its writes after their reads.
    memoryBarrier(cmd, ComputeStage, ShaderWrite, ComputeStage, ShaderRead | ShaderWrite);
    {
        const GpuLabel label{cmd, beginLabel_, endLabel_, "SSGI.Prepare"};
        const std::array<VkDescriptorImageInfo, 3> images{{
            {inputs.depthSampler, inputs.depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {inputs.viewNormalSampler, inputs.viewNormalView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, geometry_.imageView(), VK_IMAGE_LAYOUT_GENERAL},
        }};
        const PrepareSettings prepare{depthToView(inverseProjection)};
        bindKernel(cmd, Kernel::Prepare, frame, 0, images, &prepare, sizeof(prepare));
        vkCmdDispatch(cmd, groups(halfExtent_.width), groups(halfExtent_.height), 1);
    }
    memoryBarrier(cmd, ComputeStage, ShaderWrite, ComputeStage, ShaderRead | ShaderWrite);
    {
        const GpuLabel label{cmd, beginLabel_, endLabel_, "SSGI.Trace"};
        const std::array<VkDescriptorImageInfo, 3> images{{
            {VK_NULL_HANDLE, geometry_.imageView(), VK_IMAGE_LAYOUT_GENERAL},
            {radiance_.sampler(), radiance_.imageView(), VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, raw_.imageView(), VK_IMAGE_LAYOUT_GENERAL},
        }};
        const TraceSettings trace{inputs.viewToPreviousClip.native(),
                                  depthToView(inverseProjection),
                                  projectionScale,
                                  std::max(settings.radius, 0.01F),
                                  std::max(settings.thickness, 0.0F),
                                  frameCounter_,
                                  std::max(1u, quality.slices),
                                  std::max(1u, quality.stepsPerSide),
                                  MaxRadiusScreenFraction * static_cast<float>(halfExtent_.width),
                                  radianceValid_ ? 1U : 0U,
                                  {}};
        bindKernel(cmd, Kernel::Trace, frame, 0, images, &trace, sizeof(trace));
        vkCmdDispatch(cmd, groups(halfExtent_.width), groups(halfExtent_.height), 1);
    }
    memoryBarrier(cmd, ComputeStage, ShaderWrite, ComputeStage, ShaderRead | ShaderWrite);
    const std::uint32_t readIndex = historyIndex_, writeIndex = historyIndex_ ^ 1U;
    {
        const GpuLabel label{cmd, beginLabel_, endLabel_, "SSGI.Temporal"};
        const std::array<VkDescriptorImageInfo, 3> images{{
            {raw_.sampler(), raw_.imageView(), VK_IMAGE_LAYOUT_GENERAL},
            {history_[readIndex].sampler(), history_[readIndex].imageView(), VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, history_[writeIndex].imageView(), VK_IMAGE_LAYOUT_GENERAL},
        }};
        const TemporalSettings temporal{inputs.viewToPreviousClip.native(), projectionScale, historyValid_ ? 1U : 0U,
                                        TemporalCurrentWeight};
        bindKernel(cmd, Kernel::Temporal, frame, writeIndex, images, &temporal, sizeof(temporal));
        vkCmdDispatch(cmd, groups(halfExtent_.width), groups(halfExtent_.height), 1);
    }
    memoryBarrier(cmd, ComputeStage, ShaderWrite, ComputeStage, ShaderRead | ShaderWrite);
    {
        const GpuLabel label{cmd, beginLabel_, endLabel_, "SSGI.Resolve"};
        const std::array<VkDescriptorImageInfo, 3> images{{
            {history_[writeIndex].sampler(), history_[writeIndex].imageView(), VK_IMAGE_LAYOUT_GENERAL},
            {inputs.depthSampler, inputs.depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, full_.imageView(), VK_IMAGE_LAYOUT_GENERAL},
        }};
        const ResolveSettings resolve{depthToView(inverseProjection), std::max(settings.intensity, 0.0F), {}};
        bindKernel(cmd, Kernel::Resolve, frame, writeIndex, images, &resolve, sizeof(resolve));
        vkCmdDispatch(cmd, groups(fullExtent_.width), groups(fullExtent_.height), 1);
    }
    historyIndex_ = writeIndex;
    historyValid_ = true;
    ++frameCounter_;
}

void SsgiPass::captureRadiance(VkCommandBuffer cmd, const std::uint32_t frame, VkImageView sceneColor,
                               VkSampler sceneColorSampler) {
    if (frame >= FramesInFlight)
        throw std::out_of_range("Invalid SSGI frame slot");
    if (!initialized_)
        return;
    const GpuLabel label{cmd, beginLabel_, endLabel_, "SSGI.Capture"};
    // Lighting and water write the HDR target as an attachment or
    // from compute; the trace on this queue may still read the radiance.
    memoryBarrier(cmd,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                      ComputeStage,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | ShaderWrite, ComputeStage, ShaderRead | ShaderWrite);
    const std::array<VkDescriptorImageInfo, 2> images{{
        {sceneColorSampler, sceneColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, radiance_.imageView(), VK_IMAGE_LAYOUT_GENERAL},
    }};
    const CaptureSettings capture{CaptureMaxLuminance};
    bindKernel(cmd, Kernel::Capture, frame, 0, images, &capture, sizeof(capture));
    vkCmdDispatch(cmd, groups(halfExtent_.width), groups(halfExtent_.height), 1);
    // Chains this compute read of the HDR target into the fragment-stage
    // scope later passes synchronise against before rewriting it, and makes
    // the radiance visible to the next trace.
    memoryBarrier(cmd, ComputeStage, ShaderWrite, ComputeStage | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  ShaderRead);
    radianceValid_ = true;
}

void SsgiPass::reset() noexcept {
    historyValid_ = false;
    radianceValid_ = false;
}

void SsgiPass::destroy() noexcept {
    if (device_) {
        if (descriptorPool_)
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        for (const ComputeKernel& k : kernels_) {
            if (k.pipeline)
                vkDestroyPipeline(device_, k.pipeline, nullptr);
            if (k.pipelineLayout)
                vkDestroyPipelineLayout(device_, k.pipelineLayout, nullptr);
            if (k.setLayout)
                vkDestroyDescriptorSetLayout(device_, k.setLayout, nullptr);
        }
    }
    radiance_.destroy();
    raw_.destroy();
    geometry_.destroy();
    for (HdrBuffer& history : history_)
        history.destroy();
    full_.destroy();
    kernels_ = {};
    slots_ = {};
    descriptorPool_ = {};
    device_ = {};
    beginLabel_ = nullptr;
    endLabel_ = nullptr;
    historyIndex_ = 0;
    initialized_ = false;
    reset();
}
} // namespace Engine
