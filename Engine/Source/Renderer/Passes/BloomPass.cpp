#include "Engine/Renderer/Passes/BloomPass.h"

#include <algorithm>
#include <stdexcept>

namespace Engine {
namespace {
struct DownSettings {
    float threshold, knee, inverseWidth, inverseHeight;
};
struct UpSettings {
    float inverseWidth, inverseHeight, padding0, padding1;
};

void transition(const VkCommandBuffer cmd, const VkImage image, const VkImageLayout oldLayout,
                const VkPipelineStageFlags2 srcStage, const VkAccessFlags2 srcAccess, const VkImageLayout newLayout,
                const VkPipelineStageFlags2 dstStage, const VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

// A level was rendered; make it readable by the next step.
void toSampled(const VkCommandBuffer cmd, const VkImage image) {
    transition(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

// A level is fully overwritten every frame; its previous contents never matter.
void toTarget(const VkCommandBuffer cmd, const VkImage image) {
    transition(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

VkExtent2D halved(const VkExtent2D extent) {
    return {std::max(1U, extent.width / 2U), std::max(1U, extent.height / 2U)};
}
} // namespace

BloomPass::~BloomPass() {
    destroy();
}

void BloomPass::create(VkPhysicalDevice physical, VkDevice device, VkExtent2D sourceExtent, VmaAllocator allocator,
                       Assets::AssetManager& assets) {
    if (!device || !sourceExtent.width || !sourceExtent.height)
        throw std::invalid_argument("Bloom requires a valid HDR extent");
    destroy();
    device_ = device;
    extent_ = halved(sourceExtent);
    try {
        result_.create(physical, device_, extent_, allocator);
        VkExtent2D levelExtent = extent_;
        for (std::uint32_t level = 0; level < LevelCount; ++level) {
            levelExtents_[level] = levelExtent;
            down_[level].create(physical, device_, levelExtent, allocator);
            if (level > 0 && level < LevelCount - 1)
                up_[level].create(physical, device_, levelExtent, allocator);
            levelExtent = halved(levelExtent);
        }
        const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        }};
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device_, &info, nullptr, &layout_) != VK_SUCCESS)
            throw std::runtime_error("Could not create bloom layout");
        GraphicsPipelineOptions options{};
        options.colorFormat = HdrBuffer::Format;
        options.dynamicRendering = true;
        options.colorInitialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        options.colorFinalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        options.shader = "shaders/bloom_downsample.spv";
        options.assetManager = &assets;
        options.pushConstantSize = sizeof(DownSettings);
        options.pushConstantStages = VK_SHADER_STAGE_FRAGMENT_BIT;
        options.cullMode = VK_CULL_MODE_NONE;
        options.depthTestEnable = VK_FALSE;
        options.depthWriteEnable = VK_FALSE;
        options.descriptorSetLayouts = {layout_};
        pipeline_.create(device_, options);
        options.shader = "shaders/bloom_upsample.spv";
        options.pushConstantSize = sizeof(UpSettings);
        upPipeline_.create(device_, options);

        constexpr std::uint32_t setCount = FramesInFlight + 2 * (LevelCount - 1);
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setCount * 2};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = setCount;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &size;
        if (vkCreateDescriptorPool(device_, &pi, nullptr, &pool_) != VK_SUCCESS)
            throw std::runtime_error("Could not create bloom pool");
        std::array<VkDescriptorSetLayout, setCount> layouts{};
        layouts.fill(layout_);
        std::array<VkDescriptorSet, setCount> sets{};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = setCount;
        ai.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(device_, &ai, sets.data()) != VK_SUCCESS)
            throw std::runtime_error("Could not allocate bloom sets");
        std::copy_n(sets.begin(), FramesInFlight, sets_.begin());
        std::copy_n(sets.begin() + FramesInFlight, LevelCount - 1, downSets_.begin());
        std::copy_n(sets.begin() + FramesInFlight + (LevelCount - 1), LevelCount - 1, upSets_.begin());

        // The chain's own links never change: down step i reads level i - 1;
        // up step k reads level k and the next smaller up level (the bottom
        // of the down chain for the last one).
        const auto sampled = [](const HdrBuffer& buffer) {
            return VkDescriptorImageInfo{buffer.sampler(), buffer.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        };
        for (std::uint32_t level = 1; level < LevelCount; ++level) {
            const VkDescriptorImageInfo source = sampled(down_[level - 1]);
            const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, downSets_[level - 1], 0, 0,
                                             1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &source};
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        }
        for (std::uint32_t level = 0; level < LevelCount - 1; ++level) {
            const VkDescriptorImageInfo current = sampled(down_[level]);
            const VkDescriptorImageInfo smaller =
                level + 1 == LevelCount - 1 ? sampled(down_[level + 1]) : sampled(up_[level + 1]);
            const std::array<VkWriteDescriptorSet, 2> writes{{
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, upSets_[level], 0, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &current},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, upSets_[level], 1, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &smaller},
            }};
            vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    } catch (...) {
        destroy();
        throw;
    }
}

// The presentation graph imports the result as already sampled and inserts
// its own transition to a colour attachment. Initializing inside record()
// ran after that transition, so the first frame after every (re)creation
// rendered to an image the graph believed was in another layout.
void BloomPass::prepare(VkCommandBuffer commandBuffer) {
    if (!initialized_) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.image = result_.image();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = 1;
        d.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(commandBuffer, &d);
        VkClearColorValue c{};
        vkCmdClearColorImage(commandBuffer, result_.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1,
                             &b.subresourceRange);
        b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(commandBuffer, &d);
        initialized_ = true;
    }
}

void BloomPass::drawLevel(const VkCommandBuffer cmd, const GraphicsPipeline& pipeline, const VkDescriptorSet set,
                          const VkImageView target, const VkExtent2D extent, const void* constants,
                          const std::uint32_t constantsSize) const {
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, pipeline.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, constantsSize, constants);
    const VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

// The render graph hands over result_ as a colour attachment and expects it
// back as one; every other level is private to this pass.
void BloomPass::record(VkCommandBuffer commandBuffer, VkImageView source, VkSampler sampler,
                       const std::uint32_t frameIndex) {
    const auto slot = frameIndex % FramesInFlight;
    const VkDescriptorSet set = sets_.at(slot);
    VkDescriptorImageInfo image{sampler, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const auto& cached = cachedSources_[slot];
    if (!sourceDescriptorsValid_[slot] || cached.sampler != image.sampler || cached.imageView != image.imageView ||
        cached.imageLayout != image.imageLayout) {
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        cachedSources_[slot] = image;
        sourceDescriptorsValid_[slot] = true;
    }
    // Down: threshold into level 0, then filter each level into the next.
    toTarget(commandBuffer, down_[0].image());
    const DownSettings first{1.0F, 0.5F, 1.0F / static_cast<float>(extent_.width * 2U),
                             1.0F / static_cast<float>(extent_.height * 2U)};
    drawLevel(commandBuffer, pipeline_, set, down_[0].imageView(), levelExtents_[0], &first, sizeof(first));
    toSampled(commandBuffer, down_[0].image());
    for (std::uint32_t level = 1; level < LevelCount; ++level) {
        const VkExtent2D sourceExtent = levelExtents_[level - 1];
        const DownSettings step{-1.0F, 0.0F, 1.0F / static_cast<float>(sourceExtent.width),
                                1.0F / static_cast<float>(sourceExtent.height)};
        toTarget(commandBuffer, down_[level].image());
        drawLevel(commandBuffer, pipeline_, downSets_[level - 1], down_[level].imageView(), levelExtents_[level], &step,
                  sizeof(step));
        toSampled(commandBuffer, down_[level].image());
    }
    // Up: each level adds the wider glow below it; level 0 lands in result_.
    for (std::uint32_t level = LevelCount - 1; level-- > 0;) {
        const VkExtent2D smaller = levelExtents_[level + 1];
        const UpSettings step{1.0F / static_cast<float>(smaller.width), 1.0F / static_cast<float>(smaller.height), 0.0F,
                              0.0F};
        const bool top = level == 0;
        if (!top)
            toTarget(commandBuffer, up_[level].image());
        drawLevel(commandBuffer, upPipeline_, upSets_[level], top ? result_.imageView() : up_[level].imageView(),
                  levelExtents_[level], &step, sizeof(step));
        if (!top)
            toSampled(commandBuffer, up_[level].image());
    }
}

void BloomPass::destroy() noexcept {
    if (device_) {
        if (pool_) {
            vkDestroyDescriptorPool(device_, pool_, nullptr);
        }
        if (layout_) {
            vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
        }
    }
    pool_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    sets_.fill(VK_NULL_HANDLE);
    downSets_.fill(VK_NULL_HANDLE);
    upSets_.fill(VK_NULL_HANDLE);
    cachedSources_.fill({});
    sourceDescriptorsValid_.fill(false);
    pipeline_.destroy();
    upPipeline_.destroy();
    for (auto& level : down_)
        level.destroy();
    for (auto& level : up_)
        level.destroy();
    result_.destroy();
    device_ = VK_NULL_HANDLE;
    initialized_ = false;
}
} // namespace Engine
