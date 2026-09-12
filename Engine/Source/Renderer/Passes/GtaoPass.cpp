#include "Engine/Renderer/Passes/GtaoPass.h"

#include <algorithm>
#include <glm/glm.hpp>
#include <stdexcept>

namespace Engine {
namespace {
struct RawSettings { glm::mat4 inverseProjection; float fullWidth, fullHeight, radius, falloff; };
struct FilterSettings { float inverseWidth, inverseHeight, depthSigma, normalSigma; };
struct TemporalSettings { float inverseWidth, inverseHeight, historyWeight, padding; };
struct UpsampleSettings { float inverseFullWidth, inverseFullHeight, depthSigma, padding; };
constexpr VkFormat AoFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
}

GtaoPass::~GtaoPass() { destroy(); }

void GtaoPass::create(const VkPhysicalDevice physical, const VkDevice device, const VkExtent2D fullExtent,
                      const VmaAllocator allocator, Assets::AssetManager& assets) {
    if (!device || !fullExtent.width || !fullExtent.height) throw std::invalid_argument("GTAO requires a valid extent");
    destroy(); device_ = device; fullExtent_ = fullExtent;
    halfExtent_ = {std::max(1U, fullExtent.width / 2U), std::max(1U, fullExtent.height / 2U)};
    try {
        raw_.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, AoFormat);
        filtered_.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, AoFormat);
        for (auto& image : history_) image.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, AoFormat);
        full_.create(physical, device_, fullExtent_, allocator, VK_FILTER_LINEAR, AoFormat);
        const std::array<std::uint32_t, 4> counts{1, 3, 3, 2};
        const std::array<const char*, 4> shaders{"shaders/gtao_raw.spv", "shaders/gtao_spatial.spv", "shaders/gtao_temporal.spv", "shaders/gtao_upsample.spv"};
        const std::array<std::uint32_t, 4> constantSizes{sizeof(RawSettings), sizeof(FilterSettings), sizeof(TemporalSettings), sizeof(UpsampleSettings)};
        for (std::size_t p = 0; p < pipelines_.size(); ++p) {
            std::vector<VkDescriptorSetLayoutBinding> bindings(counts[p]);
            for (std::uint32_t i = 0; i < counts[p]; ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = counts[p]; li.pBindings = bindings.data();
            if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &layouts_[p]) != VK_SUCCESS) throw std::runtime_error("Could not create GTAO descriptor layout");
            GraphicsPipelineOptions options{}; options.colorFormat = AoFormat; options.colorInitialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; options.colorFinalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; options.shader = shaders[p]; options.assetManager = &assets; options.pushConstantSize = constantSizes[p]; options.pushConstantStages = VK_SHADER_STAGE_FRAGMENT_BIT; options.cullMode = VK_CULL_MODE_NONE; options.depthTestEnable = VK_FALSE; options.depthWriteEnable = VK_FALSE; options.descriptorSetLayouts = {layouts_[p]}; pipelines_[p].create(device_, options);
            VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, counts[p]}; VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = &size;
            if (vkCreateDescriptorPool(device_, &pi, nullptr, &pools_[p]) != VK_SUCCESS) throw std::runtime_error("Could not create GTAO descriptor pool");
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = pools_[p]; ai.descriptorSetCount = 1; ai.pSetLayouts = &layouts_[p];
            if (vkAllocateDescriptorSets(device_, &ai, &sets_[p]) != VK_SUCCESS) throw std::runtime_error("Could not allocate GTAO descriptor set");
        }
        const std::array<VkImageView, 5> views{raw_.imageView(), filtered_.imageView(), history_[0].imageView(), history_[1].imageView(), full_.imageView()};
        for (std::size_t i = 0; i < views.size(); ++i) { VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fb.renderPass = pipelines_[i == 0 ? 0 : i == 1 ? 1 : i < 4 ? 2 : 3].renderPass(); fb.attachmentCount = 1; fb.pAttachments = &views[i]; fb.width = i == 4 ? fullExtent_.width : halfExtent_.width; fb.height = i == 4 ? fullExtent_.height : halfExtent_.height; fb.layers = 1; if (vkCreateFramebuffer(device_, &fb, nullptr, &framebuffers_[i]) != VK_SUCCESS) throw std::runtime_error("Could not create GTAO framebuffer"); }
    } catch (...) { destroy(); throw; }
}

void GtaoPass::clearImages(const VkCommandBuffer cmd) {
    std::array<VkImageMemoryBarrier2, 5> b{}; const std::array<VkImage, 5> images{raw_.image(), filtered_.image(), history_[0].image(), history_[1].image(), full_.image()};
    for (std::size_t i=0;i<b.size();++i) { b[i]={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}; b[i].dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT; b[i].dstAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT; b[i].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b[i].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b[i].image=images[i]; b[i].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; }
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount=uint32_t(b.size()); d.pImageMemoryBarriers=b.data(); vkCmdPipelineBarrier2(cmd,&d); VkClearColorValue white{{1.F,1.F,1.F,1.F}}; for(auto& barrier:b) vkCmdClearColorImage(cmd,barrier.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&white,1,&barrier.subresourceRange);
    for(auto& barrier:b) { barrier.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT; barrier.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT; barrier.dstStageMask=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; barrier.dstAccessMask=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT; barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; } vkCmdPipelineBarrier2(cmd,&d); initialized_=true;
}

void GtaoPass::draw(const VkCommandBuffer cmd, GraphicsPipeline& pipeline, const VkFramebuffer fb, const VkDescriptorSet set, const VkExtent2D extent, const void* constants, const std::uint32_t size) { VkClearValue clear{}; VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass=pipeline.renderPass(); begin.framebuffer=fb; begin.renderArea.extent=extent; begin.clearValueCount=1; begin.pClearValues=&clear; vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE); vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline.handle()); vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline.layout(),0,1,&set,0,nullptr); vkCmdPushConstants(cmd,pipeline.layout(),VK_SHADER_STAGE_FRAGMENT_BIT,0,size,constants); const VkViewport vp{0,0,float(extent.width),float(extent.height),0,1}; const VkRect2D sc{{0,0},extent}; vkCmdSetViewport(cmd,0,1,&vp); vkCmdSetScissor(cmd,0,1,&sc); vkCmdDraw(cmd,3,1,0,0); vkCmdEndRenderPass(cmd); }

void GtaoPass::initialize(const VkCommandBuffer cmd) { if (!initialized_) clearImages(cmd); }

void GtaoPass::record(const VkCommandBuffer cmd, const VkImageView depth, const VkSampler depthSampler, const VkImageView velocity, const VkSampler velocitySampler, const Mat4& inverseProjection, const bool useTemporalVelocity) {
    if (!initialized_) clearImages(cmd); const auto write = [&](uint32_t p, std::initializer_list<VkDescriptorImageInfo> images) { std::vector<VkWriteDescriptorSet> writes; uint32_t binding=0; for (const auto& image:images) { VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet=sets_[p]; w.dstBinding=binding++; w.descriptorCount=1; w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo=&image; writes.push_back(w); } vkUpdateDescriptorSets(device_,uint32_t(writes.size()),writes.data(),0,nullptr); };
    write(0, {{depthSampler,depth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}}); RawSettings raw{inverseProjection.native(),float(fullExtent_.width),float(fullExtent_.height),1.2F,1.8F}; draw(cmd,pipelines_[0],framebuffers_[0],sets_[0],halfExtent_,&raw,sizeof(raw));
    write(1, {{raw_.sampler(),raw_.imageView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{depthSampler,depth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},{raw_.sampler(),raw_.imageView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}); FilterSettings filter{1.F/float(halfExtent_.width),1.F/float(halfExtent_.height),80.F,8.F}; draw(cmd,pipelines_[1],framebuffers_[1],sets_[1],halfExtent_,&filter,sizeof(filter));
    const uint32_t out=1U-historyIndex_; write(2, {{filtered_.sampler(),filtered_.imageView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{history_[historyIndex_].sampler(),history_[historyIndex_].imageView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{velocitySampler,velocity,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}); TemporalSettings temporal{1.F/float(halfExtent_.width),1.F/float(halfExtent_.height),useTemporalVelocity ? 0.88F : 0.F,0}; draw(cmd,pipelines_[2],framebuffers_[2+out],sets_[2],halfExtent_,&temporal,sizeof(temporal)); historyIndex_=out;
    write(3, {{history_[historyIndex_].sampler(),history_[historyIndex_].imageView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{depthSampler,depth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}}); UpsampleSettings up{1.F/float(fullExtent_.width),1.F/float(fullExtent_.height),100.F,0}; draw(cmd,pipelines_[3],framebuffers_[4],sets_[3],fullExtent_,&up,sizeof(up));
}

void GtaoPass::reset() noexcept { historyIndex_=0; initialized_=false; }
void GtaoPass::destroy() noexcept { if(device_) { for(auto fb:framebuffers_) if(fb) vkDestroyFramebuffer(device_,fb,nullptr); for(auto pool:pools_) if(pool) vkDestroyDescriptorPool(device_,pool,nullptr); for(auto layout:layouts_) if(layout) vkDestroyDescriptorSetLayout(device_,layout,nullptr); } framebuffers_.fill(VK_NULL_HANDLE); pools_.fill(VK_NULL_HANDLE); layouts_.fill(VK_NULL_HANDLE); sets_.fill(VK_NULL_HANDLE); for(auto& p:pipelines_) p.destroy(); raw_.destroy(); filtered_.destroy(); for(auto& h:history_) h.destroy(); full_.destroy(); device_=VK_NULL_HANDLE; reset(); }
} // namespace Engine
