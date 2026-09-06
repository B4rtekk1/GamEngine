#include "Engine/Renderer/Passes/BloomPass.h"
#include <algorithm>
#include <stdexcept>
namespace Engine {
namespace { struct Settings { float threshold, knee, inverseWidth, inverseHeight; }; }
BloomPass::~BloomPass() { destroy(); }
void BloomPass::create(VkPhysicalDevice physical, VkDevice device, VkExtent2D sourceExtent,
                       VmaAllocator allocator, Assets::AssetManager& assets) {
    if (!device || !sourceExtent.width || !sourceExtent.height) throw std::invalid_argument("Bloom requires a valid HDR extent");
    destroy(); device_ = device; extent_ = {std::max(1U, sourceExtent.width / 2U), std::max(1U, sourceExtent.height / 2U)};
    try {
        result_.create(physical, device_, extent_, allocator);
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; info.bindingCount = 1; info.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(device_, &info, nullptr, &layout_) != VK_SUCCESS) throw std::runtime_error("Could not create bloom layout");
        GraphicsPipelineOptions options{}; options.colorFormat = HdrBuffer::Format; options.colorInitialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        options.colorFinalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; options.shader = "shaders/bloom_downsample.spv"; options.assetManager = &assets;
        options.pushConstantSize = sizeof(Settings); options.pushConstantStages = VK_SHADER_STAGE_FRAGMENT_BIT; options.cullMode = VK_CULL_MODE_NONE;
        options.depthTestEnable = VK_FALSE; options.depthWriteEnable = VK_FALSE; options.descriptorSetLayouts = {layout_}; pipeline_.create(device_, options);
        VkImageView view = result_.imageView(); VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fb.renderPass = pipeline_.renderPass(); fb.attachmentCount = 1; fb.pAttachments = &view; fb.width = extent_.width; fb.height = extent_.height; fb.layers = 1;
        if (vkCreateFramebuffer(device_, &fb, nullptr, &framebuffer_) != VK_SUCCESS) throw std::runtime_error("Could not create bloom framebuffer");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}; VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = &size;
        if (vkCreateDescriptorPool(device_, &pi, nullptr, &pool_) != VK_SUCCESS) throw std::runtime_error("Could not create bloom pool");
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = pool_; ai.descriptorSetCount = 1; ai.pSetLayouts = &layout_;
        if (vkAllocateDescriptorSets(device_, &ai, &set_) != VK_SUCCESS) throw std::runtime_error("Could not allocate bloom set");
    } catch (...) { destroy(); throw; }
}
void BloomPass::record(VkCommandBuffer commandBuffer, VkImageView source, VkSampler sampler) {
    if (!initialized_) { VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}; b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT; b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.image = result_.image(); b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount=1; d.pImageMemoryBarriers=&b; vkCmdPipelineBarrier2(commandBuffer,&d); VkClearColorValue c{}; vkCmdClearColorImage(commandBuffer,result_.image(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&c,1,&b.subresourceRange); b.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT; b.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT; b.dstStageMask=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; b.dstAccessMask=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT; b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; vkCmdPipelineBarrier2(commandBuffer,&d); initialized_=true; }
    VkDescriptorImageInfo image{sampler, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=set_; write.dstBinding=0; write.descriptorCount=1; write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo=&image; vkUpdateDescriptorSets(device_,1,&write,0,nullptr);
    VkClearValue clear{}; VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass=pipeline_.renderPass(); begin.framebuffer=framebuffer_; begin.renderArea.extent=extent_; begin.clearValueCount=1; begin.pClearValues=&clear; vkCmdBeginRenderPass(commandBuffer,&begin,VK_SUBPASS_CONTENTS_INLINE); vkCmdBindPipeline(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_.handle()); vkCmdBindDescriptorSets(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_.layout(),0,1,&set_,0,nullptr); const Settings settings{1.0F,0.5F,1.0F/static_cast<float>(extent_.width*2U),1.0F/static_cast<float>(extent_.height*2U)}; vkCmdPushConstants(commandBuffer,pipeline_.layout(),VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(settings),&settings); VkViewport vp{0,0,static_cast<float>(extent_.width),static_cast<float>(extent_.height),0,1}; VkRect2D sc{{0,0},extent_}; vkCmdSetViewport(commandBuffer,0,1,&vp); vkCmdSetScissor(commandBuffer,0,1,&sc); vkCmdDraw(commandBuffer,3,1,0,0); vkCmdEndRenderPass(commandBuffer);
}
void BloomPass::destroy() noexcept { if(device_) { if(framebuffer_) vkDestroyFramebuffer(device_,framebuffer_,nullptr); if(pool_) vkDestroyDescriptorPool(device_,pool_,nullptr); if(layout_) vkDestroyDescriptorSetLayout(device_,layout_,nullptr); } framebuffer_=VK_NULL_HANDLE; pool_=VK_NULL_HANDLE; layout_=VK_NULL_HANDLE; set_=VK_NULL_HANDLE; pipeline_.destroy(); result_.destroy(); device_=VK_NULL_HANDLE; initialized_=false; }
}
