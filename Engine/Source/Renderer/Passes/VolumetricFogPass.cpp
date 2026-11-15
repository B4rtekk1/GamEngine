#include "Engine/Renderer/Passes/VolumetricFogPass.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Renderer/Vulkan/PipelineCache.h"
#include "Engine/Renderer/shader_loader.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace Engine {
namespace {
constexpr VkFormat VolumeFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr std::uint32_t ScatteringA = 0, ScatteringB = 1, Integrated = 2;
// Fog closer than this to the camera is faded in by fog_apply.
constexpr float NearDepth = 0.3F;

// Must match VolumetricFogParams in Common/VolumetricFog.slang.
struct PushConstants {
    float albedoDensity[4];
    float height[4];
    float intensity[4];
    float grid[4];
    std::uint32_t size[4];
};
static_assert(sizeof(PushConstants) == 80, "Volumetric fog push constants must fit 128 bytes");

constexpr VkPipelineStageFlags2 ComputeStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

void memoryBarrier(const VkCommandBuffer commandBuffer, const VkPipelineStageFlags2 srcStage,
                   const VkAccessFlags2 srcAccess, const VkPipelineStageFlags2 dstStage,
                   const VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

[[nodiscard]] VkImageMemoryBarrier2 imageBarrier(const VkImage image, const VkPipelineStageFlags2 srcStage,
                                                 const VkAccessFlags2 srcAccess, const VkPipelineStageFlags2 dstStage,
                                                 const VkAccessFlags2 dstAccess, const VkImageLayout oldLayout,
                                                 const VkImageLayout newLayout) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return barrier;
}

void submit(const VkCommandBuffer commandBuffer, const VkImageMemoryBarrier2* barriers, const std::uint32_t count) {
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = count;
    dependency.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

[[nodiscard]] std::uint32_t groups(const std::uint32_t size, const std::uint32_t groupSize) {
    return (size + groupSize - 1U) / groupSize;
}
} // namespace

VolumetricFogPass::~VolumetricFogPass() {
    destroy();
}

bool VolumetricFogPass::matches(const VkExtent2D extent, const VolumetricFogQuality quality,
                                const VkImageView depthView, const VkImageView hdrView) const noexcept {
    return created() && info_.extent.width == extent.width && info_.extent.height == extent.height &&
           info_.quality == quality && info_.depthView == depthView && info_.hdrView == hdrView;
}

void VolumetricFogPass::create(const CreateInfo& info) {
    destroy();
    if (info.device == VK_NULL_HANDLE || info.assets == nullptr || info.sceneLayout == VK_NULL_HANDLE ||
        info.extent.width == 0 || info.extent.height == 0 || info.depthView == VK_NULL_HANDLE ||
        info.hdrView == VK_NULL_HANDLE || info.hdrImage == VK_NULL_HANDLE)
        throw std::invalid_argument("VolumetricFogPass requires a device, the scene layout, depth and HDR targets");
    info_ = info;
    device_ = info.device;
    const VolumetricFogQualitySettings quality = volumetricFogQualitySettings(info.quality);
    grid_ = {groups(info.extent.width, quality.tilePixels), groups(info.extent.height, quality.tilePixels),
             quality.slices};
    try {
        createImages();
        createPipelines();
        createDescriptors();
    } catch (...) {
        destroy();
        throw;
    }
}

void VolumetricFogPass::createImages() {
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_3D;
    imageInfo.format = VolumeFormat;
    imageInfo.extent = grid_;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    for (std::size_t i = 0; i < images_.size(); ++i) {
        if (vmaCreateImage(info_.allocator, &imageInfo, &allocation, &images_[i], &allocations_[i], nullptr) !=
            VK_SUCCESS)
            throw std::runtime_error("Could not allocate a volumetric fog volume");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = images_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
        viewInfo.format = VolumeFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &viewInfo, nullptr, &views_[i]) != VK_SUCCESS)
            throw std::runtime_error("Could not create a volumetric fog view");
    }
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS)
        throw std::runtime_error("Could not create the volumetric fog sampler");
}

void VolumetricFogPass::createPipelines() {
    constexpr VkDescriptorType Storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    constexpr VkDescriptorType Sampled = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    const std::array bindings{
        VkDescriptorSetLayoutBinding{0, Storage, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, // scattering (write)
        VkDescriptorSetLayoutBinding{1, Sampled, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, // history
        VkDescriptorSetLayoutBinding{2, Storage, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, // integrated (write)
        VkDescriptorSetLayoutBinding{3, Sampled, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, // integrated (read)
        VkDescriptorSetLayoutBinding{4, Sampled, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, // scene depth
        VkDescriptorSetLayoutBinding{5, Storage, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, // HDR target
    };
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &setLayout_) != VK_SUCCESS)
        throw std::runtime_error("Could not create the volumetric fog set layout");
    const std::array setLayouts{info_.sceneLayout, setLayout_};
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS)
        throw std::runtime_error("Could not create the volumetric fog pipeline layout");
    const std::array shaders{"shaders/fog_inject.spv", "shaders/fog_integrate.spv", "shaders/fog_apply.spv"};
    for (std::size_t i = 0; i < shaders.size(); ++i) {
        const auto module = Vkutil::loadShaderModule(device_, *info_.assets, shaders[i]);
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module.get();
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = pipelineLayout_;
        if (vkCreateComputePipelines(device_, PipelineCache::get(device_), 1, &pipelineInfo, nullptr,
                                     &pipelines_[i]) != VK_SUCCESS)
            throw std::runtime_error(std::string("Could not create volumetric fog pipeline: ") + shaders[i]);
    }
}

void VolumetricFogPass::createDescriptors() {
    const std::array sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6},
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = static_cast<std::uint32_t>(sets_.size());
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS)
        throw std::runtime_error("Could not create the volumetric fog descriptor pool");
    const std::array layouts{setLayout_, setLayout_};
    VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = descriptorPool_;
    allocateInfo.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
    allocateInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device_, &allocateInfo, sets_.data()) != VK_SUCCESS)
        throw std::runtime_error("Could not allocate the volumetric fog descriptor sets");
    for (std::uint32_t variant = 0; variant < 2; ++variant) {
        const std::uint32_t write = variant == 0 ? ScatteringA : ScatteringB;
        const std::uint32_t read = variant == 0 ? ScatteringB : ScatteringA;
        const std::array images{
            VkDescriptorImageInfo{VK_NULL_HANDLE, views_[write], VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{sampler_, views_[read], VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{VK_NULL_HANDLE, views_[Integrated], VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{sampler_, views_[Integrated], VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{info_.depthSampler, info_.depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            VkDescriptorImageInfo{VK_NULL_HANDLE, info_.hdrView, VK_IMAGE_LAYOUT_GENERAL},
        };
        std::array<VkWriteDescriptorSet, 6> writes{};
        for (std::uint32_t b = 0; b < writes.size(); ++b)
            writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                         nullptr,
                         sets_[variant],
                         b,
                         0,
                         1,
                         images[b].sampler == VK_NULL_HANDLE ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                             : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         &images[b],
                         nullptr,
                         nullptr};
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void VolumetricFogPass::record(const VkCommandBuffer commandBuffer, const VkDescriptorSet sceneSet,
                               const FogSettings& settings) {
    if (!created() || sceneSet == VK_NULL_HANDLE)
        return;
    if (!initialized_) {
        std::array<VkImageMemoryBarrier2, 3> barriers{};
        for (std::size_t i = 0; i < images_.size(); ++i)
            barriers[i] = imageBarrier(images_[i], VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, ComputeStage,
                                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_GENERAL);
        submit(commandBuffer, barriers.data(), static_cast<std::uint32_t>(barriers.size()));
        initialized_ = true;
        historyValid_ = false;
    }
    const float tau = std::max(settings.temporalBlend, 0.0F);
    // Golden-ratio sequence: consecutive frames sample well-spread depths.
    const float jitter = historyValid_ ? std::fmod(static_cast<float>(frameCounter_) * 0.618034F, 1.0F) : 0.5F;
    const PushConstants push{
        {std::max(settings.albedoR, 0.0F), std::max(settings.albedoG, 0.0F), std::max(settings.albedoB, 0.0F),
         std::max(settings.density, 0.0F)},
        {settings.baseHeight, std::max(settings.heightFalloff, 0.0F), std::clamp(settings.anisotropy, -0.95F, 0.95F),
         std::clamp(settings.noise, 0.0F, 1.0F)},
        {std::max(settings.directionalScattering, 0.0F), std::max(settings.localLightScattering, 0.0F),
         std::max(settings.ambientScattering, 0.0F), std::min(tau, 0.98F)},
        {NearDepth, std::max(settings.distance, NearDepth * 2.0F), historyValid_ ? 1.0F : 0.0F, jitter},
        {grid_.width, grid_.height, grid_.depth, frameCounter_}};

    // The previous frame's kernels read and wrote the volumes; the lit colour
    // was written as an attachment or from compute and is sampled in place.
    memoryBarrier(commandBuffer, ComputeStage, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  ComputeStage, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    const VkDescriptorSet sets[] = {sceneSet, sets_[current_]};
    const auto bind = [&](const Kernel kernel) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[static_cast<std::size_t>(kernel)]);
    };
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    bind(Kernel::Inject);
    vkCmdDispatch(commandBuffer, groups(grid_.width, 4), groups(grid_.height, 4), groups(grid_.depth, 4));
    memoryBarrier(commandBuffer, ComputeStage, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ComputeStage,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    bind(Kernel::Integrate);
    vkCmdDispatch(commandBuffer, groups(grid_.width, 8), groups(grid_.height, 8), 1);

    // The integral becomes readable; the HDR target becomes a storage image.
    memoryBarrier(commandBuffer, ComputeStage, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ComputeStage,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    const auto toStorage = imageBarrier(
        info_.hdrImage,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | ComputeStage,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ComputeStage,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    submit(commandBuffer, &toStorage, 1);
    bind(Kernel::Apply);
    vkCmdDispatch(commandBuffer, groups(info_.extent.width, 8), groups(info_.extent.height, 8), 1);
    const auto toSampled =
        imageBarrier(info_.hdrImage, ComputeStage, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | ComputeStage | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    submit(commandBuffer, &toSampled, 1);

    current_ ^= 1U;
    historyValid_ = true;
    ++frameCounter_;
}

void VolumetricFogPass::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        if (descriptorPool_ != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        for (const VkPipeline pipeline : pipelines_)
            if (pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device_, pipeline, nullptr);
        if (pipelineLayout_ != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (setLayout_ != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
        if (sampler_ != VK_NULL_HANDLE)
            vkDestroySampler(device_, sampler_, nullptr);
        for (std::size_t i = 0; i < images_.size(); ++i) {
            if (views_[i] != VK_NULL_HANDLE)
                vkDestroyImageView(device_, views_[i], nullptr);
            if (images_[i] != VK_NULL_HANDLE)
                vmaDestroyImage(info_.allocator, images_[i], allocations_[i]);
        }
    }
    images_ = {};
    allocations_ = {};
    views_ = {};
    sampler_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    pipelines_ = {};
    descriptorPool_ = VK_NULL_HANDLE;
    sets_ = {};
    device_ = VK_NULL_HANDLE;
    grid_ = {};
    current_ = 0;
    initialized_ = false;
    historyValid_ = false;
}
} // namespace Engine
