#include "Engine/Renderer/Passes/TonemapPass.h"

#include "Engine/Renderer/Vulkan/PipelineCache.h"
#include "Engine/Renderer/shader_loader.h"

#include <algorithm>
#include <stdexcept>

namespace Engine {
namespace {
// Mirrors TonemapSettings in aces_tonemap.slang.
struct TonemapSettings {
    float exposure;
    float applyGamma;
    float sharpness;
    float autoExposure;
    float tonemapper;
    float contrast;
    float saturation;
    float temperature;
    float tint;
    float vignette;
    float chromaticAberration;
    float motionBlur;
    float depthOfField;
    float focusDistance;
    float aperture;
    float maxBlurPixels;
    // Weight of the summed bloom mip chain.
    float bloom;
};
static_assert(sizeof(TonemapSettings) == 68);

// Mirrors Settings in auto_exposure.slang.
struct ExposureSettings {
    float minLuminanceEv;
    float maxLuminanceEv;
    float adaptationSpeedUp;
    float adaptationSpeedDown;
    float deltaTime;
    float reset;
};

// TAA output is resampled history and grows soft in motion. Sharpen only
// what is displayed, never the history itself, so the filter cannot feed
// back into later frames. The shader applies it only to moving pixels: at
// rest TAA converges sharp, and sharpening there amplified the residual
// jitter at edges into visible flicker.
constexpr float TemporalSharpness = 0.5F;
constexpr std::uint32_t SourceCount = 3;
constexpr std::uint32_t FragmentBindingCount = 5;

bool isSrgbFormat(const VkFormat format) {
    return format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB ||
           format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

void imageBarrier(const VkCommandBuffer commandBuffer, const VkImage image, const VkPipelineStageFlags2 srcStage,
                  const VkAccessFlags2 srcAccess, const VkPipelineStageFlags2 dstStage, const VkAccessFlags2 dstAccess,
                  const VkImageLayout oldLayout, const VkImageLayout newLayout) {
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
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}
} // namespace

TonemapPass::~TonemapPass() {
    destroy();
}

void TonemapPass::create(const VkPhysicalDevice physicalDevice, const VkDevice device, const VmaAllocator allocator,
                         const VkFormat swapchainFormat, const VkExtent2D extent,
                         const std::vector<VkImageView>& swapchainViews, const VkImageView hdrView,
                         const VkSampler hdrSampler, const VkImageView bloomView, Assets::AssetManager& assets,
                         const std::array<VkImageView, 2> temporalViews,
                         const std::array<VkImageView, 2> temporalDepthViews, const VkImageView velocityView) {
    if (device == VK_NULL_HANDLE || swapchainFormat == VK_FORMAT_UNDEFINED || swapchainViews.empty() ||
        hdrView == VK_NULL_HANDLE || hdrSampler == VK_NULL_HANDLE) {
        throw std::invalid_argument("Tonemap pass received incomplete resources");
    }

    destroy();
    device_ = device;
    manualGamma_ = !isSrgbFormat(swapchainFormat);
    hasVelocity_ = velocityView != VK_NULL_HANDLE;
    hasTemporalDepth_ = temporalDepthViews[0] != VK_NULL_HANDLE && temporalDepthViews[1] != VK_NULL_HANDLE;
    try {
        exposure_.create(physicalDevice, device_, {1, 1}, allocator, VK_FILTER_NEAREST, VK_FORMAT_R32_SFLOAT, true);
        // Same format as the swapchain, so the one pipeline draws both, and
        // the editor's UI pass encodes it exactly like a presented frame.
        display_.create(physicalDevice, device_, extent, allocator, VK_FILTER_LINEAR, swapchainFormat);

        std::array<VkDescriptorSetLayoutBinding, FragmentBindingCount> bindings{};
        for (std::uint32_t index = 0; index < FragmentBindingCount; ++index)
            bindings[index] = {index, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                               nullptr};
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorSetLayout_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create tonemap descriptor layout");
        }

        const std::array<VkDescriptorSetLayoutBinding, 2> exposureBindings{{
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        }};
        layoutInfo.bindingCount = static_cast<std::uint32_t>(exposureBindings.size());
        layoutInfo.pBindings = exposureBindings.data();
        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &exposureSetLayout_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create auto-exposure descriptor layout");
        }
        const VkPushConstantRange exposureRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ExposureSettings)};
        VkPipelineLayoutCreateInfo exposureLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        exposureLayoutInfo.setLayoutCount = 1;
        exposureLayoutInfo.pSetLayouts = &exposureSetLayout_;
        exposureLayoutInfo.pushConstantRangeCount = 1;
        exposureLayoutInfo.pPushConstantRanges = &exposureRange;
        if (vkCreatePipelineLayout(device_, &exposureLayoutInfo, nullptr, &exposurePipelineLayout_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create auto-exposure pipeline layout");
        }
        {
            auto shader = Vkutil::loadShaderModule(device_, assets, "shaders/auto_exposure.spv");
            VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = shader.get();
            stage.pName = "main";
            VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipelineInfo.stage = stage;
            pipelineInfo.layout = exposurePipelineLayout_;
            if (vkCreateComputePipelines(device_, PipelineCache::get(device_), 1, &pipelineInfo, nullptr,
                                         &exposurePipeline_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create auto-exposure pipeline");
            }
        }

        GraphicsPipelineOptions options{};
        options.colorFormat = swapchainFormat;
        options.dynamicRendering = true;
        // The UI pass loads this result before transitioning the image for presentation.
        options.colorFinalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        options.shader = "shaders/aces_tonemap.spv";
        options.assetManager = &assets;
        options.pushConstantSize = sizeof(TonemapSettings);
        options.pushConstantStages = VK_SHADER_STAGE_FRAGMENT_BIT;
        options.cullMode = VK_CULL_MODE_NONE;
        options.depthTestEnable = VK_FALSE;
        options.depthWriteEnable = VK_FALSE;
        options.descriptorSetLayouts = {descriptorSetLayout_};
        pipeline_.create(device_, options);

        targetViews_ = swapchainViews;

        const std::array<VkDescriptorPoolSize, 2> poolSizes{{
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SourceCount * (FragmentBindingCount + 1)},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SourceCount},
        }};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = SourceCount * 2;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create tonemap descriptor pool");
        }

        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = descriptorPool_;
        allocation.descriptorSetCount = SourceCount;
        std::array<VkDescriptorSetLayout, SourceCount> layouts{descriptorSetLayout_, descriptorSetLayout_,
                                                               descriptorSetLayout_};
        allocation.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(device_, &allocation, descriptorSets_.data()) != VK_SUCCESS) {
            throw std::runtime_error("Could not allocate tonemap descriptor set");
        }
        std::array<VkDescriptorSetLayout, SourceCount> exposureLayouts{exposureSetLayout_, exposureSetLayout_,
                                                                       exposureSetLayout_};
        allocation.pSetLayouts = exposureLayouts.data();
        if (vkAllocateDescriptorSets(device_, &allocation, exposureSets_.data()) != VK_SUCCESS) {
            throw std::runtime_error("Could not allocate auto-exposure descriptor set");
        }

        const std::array<VkImageView, SourceCount> sources{
            hdrView, temporalViews[0] == VK_NULL_HANDLE ? hdrView : temporalViews[0],
            temporalViews[1] == VK_NULL_HANDLE ? hdrView : temporalViews[1]};
        // Without TAA there is no linear depth; depth of field then stays off
        // and any view keeps the binding valid.
        const std::array<VkImageView, SourceCount> depths{
            hdrView, hasTemporalDepth_ ? temporalDepthViews[0] : hdrView,
            hasTemporalDepth_ ? temporalDepthViews[1] : hdrView};
        for (std::size_t index = 0; index < SourceCount; ++index) {
            // Velocity is current only while TAA consumes it (without TAA the
            // prepass that writes it may be culled), so sharpening and motion
            // blur read it from the temporal sources only; any view keeps the
            // binding valid otherwise.
            const bool temporalSource = index != 0;
            const std::array<VkDescriptorImageInfo, FragmentBindingCount> images{{
                {hdrSampler, sources[index], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {hdrSampler, bloomView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {hdrSampler, hasVelocity_ && temporalSource ? velocityView : hdrView,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {hdrSampler, depths[index], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {exposure_.sampler(), exposure_.imageView(), VK_IMAGE_LAYOUT_GENERAL},
            }};
            std::array<VkWriteDescriptorSet, FragmentBindingCount + 2> writes{};
            for (std::uint32_t bindingIndex = 0; bindingIndex < FragmentBindingCount; ++bindingIndex) {
                writes[bindingIndex] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[bindingIndex].dstSet = descriptorSets_[index];
                writes[bindingIndex].dstBinding = bindingIndex;
                writes[bindingIndex].descriptorCount = 1;
                writes[bindingIndex].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[bindingIndex].pImageInfo = &images[bindingIndex];
            }
            const VkDescriptorImageInfo exposureOutput{VK_NULL_HANDLE, exposure_.imageView(),
                                                       VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet& meteredSource = writes[FragmentBindingCount];
            meteredSource = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            meteredSource.dstSet = exposureSets_[index];
            meteredSource.dstBinding = 0;
            meteredSource.descriptorCount = 1;
            meteredSource.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            meteredSource.pImageInfo = &images[0];
            VkWriteDescriptorSet& adapted = writes[FragmentBindingCount + 1];
            adapted = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            adapted.dstSet = exposureSets_[index];
            adapted.dstBinding = 1;
            adapted.descriptorCount = 1;
            adapted.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            adapted.pImageInfo = &exposureOutput;
            vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    } catch (...) {
        destroy();
        throw;
    }
}

void TonemapPass::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        if (descriptorPool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        }
        if (exposurePipeline_ != VK_NULL_HANDLE)
            vkDestroyPipeline(device_, exposurePipeline_, nullptr);
        if (exposurePipelineLayout_ != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device_, exposurePipelineLayout_, nullptr);
        if (exposureSetLayout_ != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device_, exposureSetLayout_, nullptr);
    }
    exposurePipeline_ = VK_NULL_HANDLE;
    exposurePipelineLayout_ = VK_NULL_HANDLE;
    exposureSetLayout_ = VK_NULL_HANDLE;
    exposureSets_.fill(VK_NULL_HANDLE);
    exposure_.destroy();
    exposureInitialized_ = false;
    exposureMetered_ = false;
    display_.destroy();
    displayInitialized_ = false;
    targetViews_.clear();
    descriptorSets_.fill(VK_NULL_HANDLE);
    descriptorPool_ = VK_NULL_HANDLE;
    pipeline_.destroy();
    if (device_ != VK_NULL_HANDLE && descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
    }
    descriptorSetLayout_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

void TonemapPass::recordExposure(const VkCommandBuffer commandBuffer, const PostProcessSettings& settings,
                                 const float deltaTime, const std::uint32_t sourceIndex) {
    if (!settings.autoExposure)
        return;
    const bool reset = !exposureMetered_;
    if (!exposureInitialized_) {
        initializeExposure(commandBuffer);
    } else {
        // The previous frame's display transform read the value.
        imageBarrier(commandBuffer, exposure_.image(), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    }
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, exposurePipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, exposurePipelineLayout_, 0, 1,
                            &exposureSets_.at(sourceIndex), 0, nullptr);
    const ExposureSettings pushConstants{settings.minLuminanceEv,
                                         std::max(settings.maxLuminanceEv, settings.minLuminanceEv),
                                         settings.adaptationSpeedUp,
                                         settings.adaptationSpeedDown,
                                         std::clamp(deltaTime, 0.0F, 0.25F),
                                         reset ? 1.0F : 0.0F};
    vkCmdPushConstants(commandBuffer, exposurePipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants),
                       &pushConstants);
    vkCmdDispatch(commandBuffer, 1, 1, 1);
    imageBarrier(commandBuffer, exposure_.image(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    exposureMetered_ = true;
}

void TonemapPass::initializeExposure(const VkCommandBuffer commandBuffer) {
    // The display transform binds the image whether or not it is metered,
    // so it must reach GENERAL before the first draw too.
    imageBarrier(commandBuffer, exposure_.image(), VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    exposureInitialized_ = true;
}

void TonemapPass::draw(const VkCommandBuffer commandBuffer, const VkImageView target, const VkExtent2D extent,
                       const PostProcessSettings& settings, const std::uint32_t sourceIndex) {
    if (!exposureInitialized_)
        initializeExposure(commandBuffer);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0F, 0.0F, 0.0F, 1.0F}};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(commandBuffer, &rendering);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(), 0, 1,
                            &descriptorSets_.at(sourceIndex), 0, nullptr);
    const bool temporalSource = sourceIndex != 0;
    const TonemapSettings pushConstants{
        .exposure = settings.exposureCompensation,
        .applyGamma = manualGamma_ ? 1.0F : 0.0F,
        .sharpness = temporalSource && hasVelocity_ ? TemporalSharpness : 0.0F,
        .autoExposure = settings.autoExposure && exposureMetered_ ? 1.0F : 0.0F,
        .tonemapper = settings.tonemapper == TonemapOperator::AgX ? 0.0F : 1.0F,
        .contrast = std::max(settings.contrast, 0.0F),
        .saturation = std::max(settings.saturation, 0.0F),
        .temperature = settings.temperature,
        .tint = settings.tint,
        .vignette = std::max(settings.vignette, 0.0F),
        .chromaticAberration = std::max(settings.chromaticAberration, 0.0F),
        .motionBlur = hasVelocity_ && temporalSource ? std::max(settings.motionBlur, 0.0F) : 0.0F,
        .depthOfField = settings.depthOfField && temporalSource && hasTemporalDepth_ ? 1.0F : 0.0F,
        .focusDistance = settings.focusDistance,
        .aperture = std::max(settings.aperture, 0.0F),
        .maxBlurPixels = std::max(settings.maxBlurPixels, 0.0F),
        .bloom = std::max(settings.bloom, 0.0F)};
    vkCmdPushConstants(commandBuffer, pipeline_.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pushConstants),
                       &pushConstants);
    const VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent.width), static_cast<float>(extent.height),
                              0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRendering(commandBuffer);
}

void TonemapPass::record(const VkCommandBuffer commandBuffer, const std::uint32_t imageIndex, const VkExtent2D extent,
                         const PostProcessSettings& settings, const std::uint32_t sourceIndex) {
    draw(commandBuffer, targetViews_.at(imageIndex), extent, settings, sourceIndex);
}

void TonemapPass::recordDisplay(const VkCommandBuffer commandBuffer, const VkExtent2D extent,
                                const PostProcessSettings& settings, const std::uint32_t sourceIndex) {
    // The previous frame's UI pass sampled the image. Its contents are
    // cleared and redrawn, so the transition starts from UNDEFINED: that is
    // valid whatever layout an earlier, possibly unsubmitted, frame left.
    imageBarrier(commandBuffer, display_.image(),
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    draw(commandBuffer, display_.imageView(), extent, settings, sourceIndex);
    imageBarrier(commandBuffer, display_.image(), VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    displayInitialized_ = true;
}
} // namespace Engine
