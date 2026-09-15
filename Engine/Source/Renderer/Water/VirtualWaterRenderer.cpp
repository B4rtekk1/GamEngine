#include "Engine/Renderer/Water/VirtualWaterRenderer.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/ECS/Components/WaterBodyComponent.h"
#include "Engine/ECS/Components/TransformComponent.h"
#include "Engine/Renderer/Geometry/GpuVertex.h"
#include "Engine/Renderer/Water/VirtualWaterPageBuilder.h"
#include "Engine/Renderer/Water/WaterSystem.h"
#include "Engine/Renderer/shader_loader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Engine::Water {
namespace {
VkDescriptorSetLayout makeLayout(VkDevice device, std::span<const VkDescriptorSetLayoutBinding> bindings) {
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout{};
    if (vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) != VK_SUCCESS)
        throw std::runtime_error("Could not create virtual-water descriptor set layout");
    return layout;
}
void storageWriteBarrier(VkCommandBuffer commandBuffer,
                         VkPipelineStageFlags2 dstStage,
                         VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}
void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout; b.newLayout = newLayout; b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount=1; d.pImageMemoryBarriers=&b;
    vkCmdPipelineBarrier2(cmd,&d);
}
} // namespace

VkPipeline VirtualWaterRenderer::makeCompute(const char* shader, VkPipelineLayout layout) const {
    const auto module = Vkutil::loadShaderModule(device_, *assets_, shader);
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; stage.module=module.get(); stage.pName="main";
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage=stage; info.layout=layout;
    VkPipeline result{};
    if(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&info,nullptr,&result)!=VK_SUCCESS)
        throw std::runtime_error(std::string("Could not create virtual-water compute pipeline: ")+shader);
    return result;
}

void VirtualWaterRenderer::create(VkPhysicalDevice physicalDevice, VkDevice device, VmaAllocator allocator,
                                  Assets::AssetManager& assets, VkExtent2D extent, VkFormat depthFormat,
                                  VkImageView depthView, VkDescriptorSetLayout sceneLayout,
                                  VkImageView hdrTargetView, VkDescriptorImageInfo opaqueColor,
                                  VkDescriptorImageInfo opaqueDepth, VkDescriptorImageInfo previousHiZ,
                                  std::span<const VkBuffer> instanceBuffers,
                                  std::span<const VkBuffer> cullingUniformBuffers) {
    destroy();
    if(!device || !extent.width || !extent.height || instanceBuffers.size()<FramesInFlight ||
       cullingUniformBuffers.size()<FramesInFlight) throw std::invalid_argument("Invalid VirtualWaterRenderer resources");
    physicalDevice_=physicalDevice; device_=device; allocator_=allocator; assets_=&assets; extent_=extent; depthView_=depthView;
    opaqueColor_=opaqueColor; opaqueDepth_=opaqueDepth; previousHiZ_=previousHiZ;
    std::copy_n(instanceBuffers.begin(),FramesInFlight,instanceBuffers_.begin());
    std::copy_n(cullingUniformBuffers.begin(),FramesInFlight,cullingUniformBuffers_.begin());
    try {
        createBuffers();
        surface_.create(physicalDevice_,device_,extent_,allocator_,VK_FILTER_NEAREST,VK_FORMAT_R16G16B16A16_SFLOAT,false);
        meta_.create(physicalDevice_,device_,extent_,allocator_,VK_FILTER_NEAREST,VK_FORMAT_R32G32_SFLOAT,false);
        velocity_.create(physicalDevice_,device_,extent_,allocator_,VK_FILTER_NEAREST,VK_FORMAT_R16G16_SFLOAT,false);
        lighting_.create(physicalDevice_,device_,extent_,allocator_,VK_FILTER_LINEAR,VK_FORMAT_R16G16B16A16_SFLOAT,true);
        sssrDepth_.create(physicalDevice_, device_, (extent_.width + 1U) / 2U, (extent_.height + 1U) / 2U, allocator_);
        createDescriptors(sceneLayout); createPipelines(sceneLayout,depthFormat); createFramebuffers(hdrTargetView); writeDescriptors();
    } catch (...) { destroy(); throw; }
}

void VirtualWaterRenderer::createBuffers() {
    const auto host=[&](Buffer& b,VkDeviceSize size,VkBufferUsageFlags usage){
        b.createHostVisible(physicalDevice_,device_,std::max<VkDeviceSize>(size,16),usage,allocator_);
    };
    host(pages_, sizeof(GPUVirtualWaterPage)*MaxPages, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    host(drawTemplates_, sizeof(GPUWaterDrawTemplate)*MaxDrawBins, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    host(cullConfig_, sizeof(GPUWaterCullConfig), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    host(farOceanConfig_, sizeof(GPUFarOceanConfig), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    const GPUFarOceanConfig emptyFar{}; farOceanConfig_.update(&emptyFar, sizeof(emptyFar));
    host(authoredWaterConfig_, sizeof(GPUAuthoredWaterConfig), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    const GPUAuthoredWaterConfig emptyAuthored{}; authoredWaterConfig_.update(&emptyAuthored, sizeof(emptyAuthored));
    tileSettings_.width=extent_.width; tileSettings_.height=extent_.height; tileSettings_.tilesX=(extent_.width+7)/8;
    tileSettings_.maxTiles=tileSettings_.tilesX*((extent_.height+7)/8);
    host(tileSettingsBuffer_,sizeof(TileSettings),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT); tileSettingsBuffer_.update(&tileSettings_,sizeof(tileSettings_));

    host(statePageTable_, sizeof(std::uint32_t) * MaxPages, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    host(stateOwners_, sizeof(std::uint32_t) * MaxPhysicalStatePages, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    host(statePhysical_, sizeof(GPUWaterPhysicalState) * MaxPhysicalStatePages, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    host(stateCells_, sizeof(GPUWaterStateCell) * MaxStateCells, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    host(stateCellsScratch_, sizeof(GPUWaterStateCell) * MaxStateCells, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    host(stateAllocator_, sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::vector<std::uint32_t> invalidPages(MaxPages, InvalidPhysicalPage);
    std::vector<std::uint32_t> invalidOwners(MaxPhysicalStatePages, InvalidPhysicalPage);
    std::vector<GPUWaterPhysicalState> zeroPhysical(MaxPhysicalStatePages);
    std::vector<GPUWaterStateCell> zeroCells(MaxStateCells);
    const std::uint32_t zeroAllocator = 0;
    statePageTable_.update(invalidPages.data(), sizeof(std::uint32_t) * invalidPages.size());
    stateOwners_.update(invalidOwners.data(), sizeof(std::uint32_t) * invalidOwners.size());
    statePhysical_.update(zeroPhysical.data(), sizeof(GPUWaterPhysicalState) * zeroPhysical.size());
    stateCells_.update(zeroCells.data(), sizeof(GPUWaterStateCell) * zeroCells.size());
    stateCellsScratch_.update(zeroCells.data(), sizeof(GPUWaterStateCell) * zeroCells.size());
    stateAllocator_.update(&zeroAllocator, sizeof(zeroAllocator));

    std::vector<GPUWaterPageHistory> zeros(MaxPages);
    for(std::uint32_t f=0;f<FramesInFlight;++f){
        host(visiblePages_[f],sizeof(GPUVisibleWaterPage)*MaxVisiblePageSlots,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        host(drawBinCounts_[f],sizeof(std::uint32_t)*MaxDrawBins,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        host(indirectCommands_[f],sizeof(VkDrawIndexedIndirectCommand)*MaxDrawBins,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        host(history_[f],sizeof(GPUWaterPageHistory)*MaxPages,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); history_[f].update(zeros.data(),sizeof(GPUWaterPageHistory)*MaxPages);
        host(stats_[f],sizeof(GPUWaterPageStats),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        host(tileLists_[f],sizeof(std::uint32_t)*4ull*tileSettings_.maxTiles,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        host(tileCounts_[f],sizeof(std::uint32_t)*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        host(tileDispatch_[f],sizeof(VkDispatchIndirectCommand)*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        host(interactionEvents_[f], sizeof(GPUWaterInteractionEvent) * MaxInteractionEvents, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        host(underwaterConfig_[f], sizeof(GPUWaterUnderwaterConfig), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        const GPUWaterUnderwaterConfig emptyUnderwater{};
        underwaterConfig_[f].update(&emptyUnderwater, sizeof(emptyUnderwater));
    }
}

void VirtualWaterRenderer::createDescriptors(VkDescriptorSetLayout sceneLayout) {
    const VkShaderStageFlags C=VK_SHADER_STAGE_COMPUTE_BIT,V=VK_SHADER_STAGE_VERTEX_BIT,F=VK_SHADER_STAGE_FRAGMENT_BIT;
    const std::array cullBindings{
        VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{4,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{5,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{6,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{7,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{8,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{9,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,C,nullptr}};
    cullLayout_=makeLayout(device_,cullBindings);
    const std::array buildBindings{
        VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{3,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,C,nullptr}};
    buildLayout_=makeLayout(device_,buildBindings);
    const std::array drawBindings{
        VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,V,nullptr},
        VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,V,nullptr},
        VkDescriptorSetLayoutBinding{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,V,nullptr},
        VkDescriptorSetLayoutBinding{3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,V,nullptr},
        VkDescriptorSetLayoutBinding{4,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,V,nullptr}};
    drawLayout_=makeLayout(device_,drawBindings);
    const std::array classifyBindings{
        VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{3,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,C,nullptr}};
    classifyLayout_=makeLayout(device_,classifyBindings);
    const std::array dispatchBindings{
        VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr}};
    buildDispatchLayout_=makeLayout(device_,dispatchBindings);
    const std::array shadeBindings{
        VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{3,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{4,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{5,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,C,nullptr},
        VkDescriptorSetLayoutBinding{6,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{7,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{8,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{9,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{10,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{11,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,C,nullptr}};
    shadeLayout_=makeLayout(device_,shadeBindings);
    const std::array compositeBindings{
        VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,F,nullptr},
        VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,F,nullptr},
        VkDescriptorSetLayoutBinding{2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,F,nullptr},
        VkDescriptorSetLayoutBinding{3,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,F,nullptr},
        VkDescriptorSetLayoutBinding{4,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,F,nullptr},
        VkDescriptorSetLayoutBinding{5,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,F,nullptr}};
    compositeLayout_=makeLayout(device_,compositeBindings);
    const std::array stateBindings{
        VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{4,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{5,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{6,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{7,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{8,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{9,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr}};
    stateLayout_=makeLayout(device_,stateBindings);
    const std::array farBindings{
        VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,V|F,nullptr}};
    farLayout_=makeLayout(device_,farBindings);
    const std::array authoredBindings{
        VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,V,nullptr},
        VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,V,nullptr},
        VkDescriptorSetLayoutBinding{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,V,nullptr},
        VkDescriptorSetLayoutBinding{3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,V,nullptr},
        VkDescriptorSetLayoutBinding{4,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,V,nullptr}};
    authoredLayout_=makeLayout(device_,authoredBindings);
    const std::array sssrBindings{
        VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,C,nullptr}};
    sssrInitLayout_=makeLayout(device_,sssrBindings);
    sssrReduceLayout_=makeLayout(device_,sssrBindings);

    const std::array sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,FramesInFlight*64U},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,FramesInFlight*18U},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,FramesInFlight*20U+32U},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,FramesInFlight*4U+32U}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=FramesInFlight*10U+32U;pool.poolSizeCount=sizes.size();pool.pPoolSizes=sizes.data();
    if(vkCreateDescriptorPool(device_,&pool,nullptr,&descriptorPool_)!=VK_SUCCESS)throw std::runtime_error("Could not create virtual-water descriptor pool");
    auto alloc=[&](VkDescriptorSetLayout layout,auto& sets){std::array<VkDescriptorSetLayout,FramesInFlight> ls{};ls.fill(layout);VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};a.descriptorPool=descriptorPool_;a.descriptorSetCount=FramesInFlight;a.pSetLayouts=ls.data();if(vkAllocateDescriptorSets(device_,&a,sets.data())!=VK_SUCCESS)throw std::runtime_error("Could not allocate virtual-water descriptor sets");};
    alloc(cullLayout_,cullSets_);alloc(buildLayout_,buildSets_);alloc(drawLayout_,drawSets_);alloc(classifyLayout_,classifySets_);alloc(buildDispatchLayout_,buildDispatchSets_);alloc(shadeLayout_,shadeSets_);alloc(compositeLayout_,compositeSets_);alloc(stateLayout_,stateSets_);alloc(farLayout_,farSets_);alloc(authoredLayout_,authoredSets_);

    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount=1; pli.pSetLayouts=&cullLayout_; if(vkCreatePipelineLayout(device_,&pli,nullptr,&cullPipelineLayout_)!=VK_SUCCESS)throw std::runtime_error("water cull layout");
    pli.pSetLayouts=&buildLayout_; if(vkCreatePipelineLayout(device_,&pli,nullptr,&buildPipelineLayout_)!=VK_SUCCESS)throw std::runtime_error("water indirect layout");
    pli.pSetLayouts=&classifyLayout_; if(vkCreatePipelineLayout(device_,&pli,nullptr,&classifyPipelineLayout_)!=VK_SUCCESS)throw std::runtime_error("water classify layout");
    pli.pSetLayouts=&buildDispatchLayout_; if(vkCreatePipelineLayout(device_,&pli,nullptr,&buildDispatchPipelineLayout_)!=VK_SUCCESS)throw std::runtime_error("water dispatch layout");
    std::array shadeLayouts{sceneLayout,shadeLayout_}; VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(ShadePush)}; pli.setLayoutCount=2;pli.pSetLayouts=shadeLayouts.data();pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&pc;
    if(vkCreatePipelineLayout(device_,&pli,nullptr,&shadePipelineLayout_)!=VK_SUCCESS)throw std::runtime_error("water shade layout");
    VkPushConstantRange statePc{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(StatePush)};
    std::array stateLayouts{sceneLayout,stateLayout_};
    pli.setLayoutCount=2; pli.pSetLayouts=stateLayouts.data(); pli.pushConstantRangeCount=1; pli.pPushConstantRanges=&statePc;
    if(vkCreatePipelineLayout(device_,&pli,nullptr,&statePipelineLayout_)!=VK_SUCCESS)throw std::runtime_error("water state layout");
    pli.pushConstantRangeCount=0; pli.pPushConstantRanges=nullptr; pli.setLayoutCount=1;
    pli.pSetLayouts=&sssrInitLayout_;
    if(vkCreatePipelineLayout(device_,&pli,nullptr,&sssrInitPipelineLayout_)!=VK_SUCCESS)throw std::runtime_error("water SSSR init layout");
    pli.pSetLayouts=&sssrReduceLayout_;
    if(vkCreatePipelineLayout(device_,&pli,nullptr,&sssrReducePipelineLayout_)!=VK_SUCCESS)throw std::runtime_error("water SSSR reduce layout");
}

void VirtualWaterRenderer::createPipelines(VkDescriptorSetLayout sceneLayout,VkFormat depthFormat) {
    cullPipeline_=makeCompute("shaders/water_page_cull.spv",cullPipelineLayout_);
    buildPipeline_=makeCompute("shaders/water_page_build_indirect.spv",buildPipelineLayout_);
    classifyPipeline_=makeCompute("shaders/water_tile_classify.spv",classifyPipelineLayout_);
    buildDispatchPipeline_=makeCompute("shaders/water_tile_build_dispatch.spv",buildDispatchPipelineLayout_);
    shadePipeline_=makeCompute("shaders/water_virtual_shade.spv",shadePipelineLayout_);
    stateAllocatePipeline_=makeCompute("shaders/water_state_allocate.spv",statePipelineLayout_);
    statePipeline_=makeCompute("shaders/water_state_update.spv",statePipelineLayout_);
    sssrInitPipeline_=makeCompute("shaders/water_sssr_depth_init.spv",sssrInitPipelineLayout_);
    sssrReducePipeline_=makeCompute("shaders/water_sssr_depth_reduce.spv",sssrReducePipelineLayout_);
    sssrDepthPass_.create(device_, descriptorPool_, sssrInitPipeline_, sssrInitPipelineLayout_, sssrInitLayout_,
                          sssrReducePipeline_, sssrReducePipelineLayout_, sssrReduceLayout_,
                          sssrDepth_, opaqueDepth_.imageView, opaqueDepth_.sampler);

    GraphicsPipelineOptions p{};p.colorFormat=VK_FORMAT_R16G16B16A16_SFLOAT;p.additionalColorFormat=VK_FORMAT_R32G32_SFLOAT;p.thirdColorFormat=VK_FORMAT_R16G16_SFLOAT;p.depthFormat=depthFormat;p.samples=VK_SAMPLE_COUNT_1_BIT;
    p.colorLoadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;p.additionalColorLoadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;p.thirdColorLoadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;p.depthLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
    p.colorInitialLayout=VK_IMAGE_LAYOUT_UNDEFINED;p.colorFinalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;p.depthInitialLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;p.depthFinalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    p.depthWriteEnable=VK_FALSE;p.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;p.cullMode=VK_CULL_MODE_NONE;p.shader="shaders/water_virtual_prepass.spv";p.assetManager=assets_;p.descriptorSetLayouts={sceneLayout,drawLayout_};
    p.vertexBindings={{0,sizeof(GpuVertex),VK_VERTEX_INPUT_RATE_VERTEX}};
    p.vertexAttributes={{0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(GpuVertex,px)},{2,0,VK_FORMAT_R16G16_SFLOAT,offsetof(GpuVertex,texCoord)},{8,0,VK_FORMAT_R32_UINT,offsetof(GpuVertex,materialIndex)}};
    prepassPipeline_.create(device_,p);

    // Far ocean is analytic: a fullscreen triangle intersects the base water plane
    // only beyond the last geometry clip level. It is rendered first in the same
    // compatible prepass render pass, then near/mid virtual pages overwrite it.
    GraphicsPipelineOptions far=p;
    far.shader="shaders/water_far_ocean_prepass.spv";
    far.descriptorSetLayouts={sceneLayout,farLayout_};
    far.vertexBindings.clear();
    far.vertexAttributes.clear();
    farPrepassPipeline_.create(device_,far);

    GraphicsPipelineOptions authored=p;
    authored.shader="shaders/water_authored_prepass.spv";
    authored.descriptorSetLayouts={sceneLayout,authoredLayout_};
    authored.vertexBindings={{0,sizeof(GpuVertex),VK_VERTEX_INPUT_RATE_VERTEX}};
    authored.vertexAttributes={
        {0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(GpuVertex,px)},
        {2,0,VK_FORMAT_R16G16_SFLOAT,offsetof(GpuVertex,texCoord)},
        {4,0,VK_FORMAT_R16G16_SFLOAT,offsetof(GpuVertex,texCoord1)},
        {8,0,VK_FORMAT_R32_UINT,offsetof(GpuVertex,materialIndex)}};
    authoredPrepassPipeline_.create(device_,authored);

    GraphicsPipelineOptions c{};c.colorFormat=HdrBuffer::Format;c.samples=VK_SAMPLE_COUNT_1_BIT;c.colorLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;c.colorInitialLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;c.colorFinalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;c.depthTestEnable=VK_FALSE;c.depthWriteEnable=VK_FALSE;c.cullMode=VK_CULL_MODE_NONE;c.shader="shaders/water_virtual_composite.spv";c.assetManager=assets_;c.descriptorSetLayouts={sceneLayout,compositeLayout_};compositePipeline_.create(device_,c);
}

void VirtualWaterRenderer::createFramebuffers(VkImageView hdrTargetView) {
    std::array<VkImageView,4> views{surface_.imageView(),meta_.imageView(),velocity_.imageView(),depthView_};
    VkFramebufferCreateInfo f{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};f.renderPass=prepassPipeline_.renderPass();f.attachmentCount=views.size();f.pAttachments=views.data();f.width=extent_.width;f.height=extent_.height;f.layers=1;
    if(vkCreateFramebuffer(device_,&f,nullptr,&prepassFramebuffer_)!=VK_SUCCESS)throw std::runtime_error("Could not create virtual-water prepass framebuffer");
    f.renderPass=compositePipeline_.renderPass();f.attachmentCount=1;f.pAttachments=&hdrTargetView;
    if(vkCreateFramebuffer(device_,&f,nullptr,&compositeFramebuffer_)!=VK_SUCCESS)throw std::runtime_error("Could not create virtual-water composite framebuffer");
}

void VirtualWaterRenderer::writeDescriptors() {
    const auto bufferInfo = [](const VkBuffer buffer, const VkDeviceSize size = VK_WHOLE_SIZE) {
        return VkDescriptorBufferInfo{buffer, 0, size};
    };
    const auto writeBuffer = [](VkWriteDescriptorSet& write, const VkDescriptorSet set,
                                const std::uint32_t binding, const VkDescriptorType type,
                                const VkDescriptorBufferInfo* info) {
        write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = type;
        write.pBufferInfo = info;
    };
    const auto writeImage = [](VkWriteDescriptorSet& write, const VkDescriptorSet set,
                               const std::uint32_t binding, const VkDescriptorType type,
                               const VkDescriptorImageInfo* info) {
        write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = type;
        write.pImageInfo = info;
    };

    for (std::uint32_t frame = 0; frame < FramesInFlight; ++frame) {
        // Page culling.
        const VkDescriptorBufferInfo cullBuffers[] = {
            bufferInfo(pages_.handle()),
            bufferInfo(instanceBuffers_[frame]),
            bufferInfo(visiblePages_[frame].handle()),
            bufferInfo(drawBinCounts_[frame].handle()),
            bufferInfo(cullingUniformBuffers_[frame]),
            bufferInfo(history_[(frame + FramesInFlight - 1U) % FramesInFlight].handle()),
            bufferInfo(history_[frame].handle()),
            bufferInfo(stats_[frame].handle()),
            bufferInfo(cullConfig_.handle(), sizeof(GPUWaterCullConfig)),
        };
        std::array<VkWriteDescriptorSet, 10> cullWrites{};
        writeBuffer(cullWrites[0], cullSets_[frame], 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &cullBuffers[0]);
        writeBuffer(cullWrites[1], cullSets_[frame], 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &cullBuffers[1]);
        writeBuffer(cullWrites[2], cullSets_[frame], 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &cullBuffers[2]);
        writeBuffer(cullWrites[3], cullSets_[frame], 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &cullBuffers[3]);
        writeImage(cullWrites[4], cullSets_[frame], 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &previousHiZ_);
        writeBuffer(cullWrites[5], cullSets_[frame], 5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &cullBuffers[4]);
        writeBuffer(cullWrites[6], cullSets_[frame], 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &cullBuffers[5]);
        writeBuffer(cullWrites[7], cullSets_[frame], 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &cullBuffers[6]);
        writeBuffer(cullWrites[8], cullSets_[frame], 8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &cullBuffers[7]);
        writeBuffer(cullWrites[9], cullSets_[frame], 9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &cullBuffers[8]);
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(cullWrites.size()), cullWrites.data(), 0, nullptr);

        // Draw-command construction.
        const VkDescriptorBufferInfo buildBuffers[] = {
            bufferInfo(drawBinCounts_[frame].handle()),
            bufferInfo(drawTemplates_.handle()),
            bufferInfo(indirectCommands_[frame].handle()),
            bufferInfo(cullConfig_.handle(), sizeof(GPUWaterCullConfig)),
        };
        std::array<VkWriteDescriptorSet, 4> buildWrites{};
        for (std::uint32_t i = 0; i < buildWrites.size(); ++i) {
            writeBuffer(buildWrites[i], buildSets_[frame], i,
                        i == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                        &buildBuffers[i]);
        }
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(buildWrites.size()), buildWrites.data(), 0, nullptr);

        // Page data consumed by the prepass vertex shader.
        // recordState() rewrites these three bindings for the current safe frame slot
        // according to stateCurrentScratch_. Use A->scratch as the deterministic default.
        const VkBuffer stateRead = stateCells_.handle();
        const VkBuffer stateWrite = stateCellsScratch_.handle();
        const VkDescriptorBufferInfo drawBuffers[] = {
            bufferInfo(pages_.handle()),
            bufferInfo(visiblePages_[frame].handle()),
            bufferInfo(statePageTable_.handle()),
            bufferInfo(statePhysical_.handle()),
            bufferInfo(stateWrite),
        };
        std::array<VkWriteDescriptorSet, 5> drawWrites{};
        for (std::uint32_t i = 0; i < drawWrites.size(); ++i) {
            writeBuffer(drawWrites[i], drawSets_[frame], i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &drawBuffers[i]);
        }
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(drawWrites.size()), drawWrites.data(), 0, nullptr);

        // Screen-space tile classification.
        const VkDescriptorImageInfo metaInfo{meta_.sampler(), meta_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo classifyBuffers[] = {
            bufferInfo(tileLists_[frame].handle()),
            bufferInfo(tileCounts_[frame].handle()),
            bufferInfo(tileSettingsBuffer_.handle(), sizeof(TileSettings)),
        };
        std::array<VkWriteDescriptorSet, 4> classifyWrites{};
        writeImage(classifyWrites[0], classifySets_[frame], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &metaInfo);
        writeBuffer(classifyWrites[1], classifySets_[frame], 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &classifyBuffers[0]);
        writeBuffer(classifyWrites[2], classifySets_[frame], 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &classifyBuffers[1]);
        writeBuffer(classifyWrites[3], classifySets_[frame], 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &classifyBuffers[2]);
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(classifyWrites.size()), classifyWrites.data(), 0, nullptr);

        const VkDescriptorBufferInfo dispatchBuffers[] = {
            bufferInfo(tileCounts_[frame].handle()),
            bufferInfo(tileDispatch_[frame].handle()),
        };
        std::array<VkWriteDescriptorSet, 2> dispatchWrites{};
        writeBuffer(dispatchWrites[0], buildDispatchSets_[frame], 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &dispatchBuffers[0]);
        writeBuffer(dispatchWrites[1], buildDispatchSets_[frame], 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &dispatchBuffers[1]);
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(dispatchWrites.size()), dispatchWrites.data(), 0, nullptr);

        // Persistent virtual-state simulation.
        const VkDescriptorBufferInfo stateBuffers[] = {
            bufferInfo(pages_.handle()),
            bufferInfo(instanceBuffers_[frame]),
            bufferInfo(history_[frame].handle()),
            bufferInfo(statePageTable_.handle()),
            bufferInfo(stateOwners_.handle()),
            bufferInfo(statePhysical_.handle()),
            bufferInfo(interactionEvents_[frame].handle()),
            bufferInfo(stateAllocator_.handle()),
            bufferInfo(stateRead),
            bufferInfo(stateWrite),
        };
        std::array<VkWriteDescriptorSet, 10> stateWrites{};
        for (std::uint32_t i = 0; i < stateWrites.size(); ++i) {
            writeBuffer(stateWrites[i], stateSets_[frame], i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &stateBuffers[i]);
        }
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(stateWrites.size()), stateWrites.data(), 0, nullptr);

        // Adaptive water shading.
        const VkDescriptorImageInfo shadeImages[] = {
            {surface_.sampler(), surface_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {meta_.sampler(), meta_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            opaqueColor_,
            opaqueDepth_,
        };
        const VkDescriptorImageInfo lightingStorage{VK_NULL_HANDLE, lighting_.imageView(), VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorBufferInfo shadeBuffers[] = {
            bufferInfo(tileLists_[frame].handle()),
            bufferInfo(tileSettingsBuffer_.handle(), sizeof(TileSettings)),
            bufferInfo(statePageTable_.handle()),
            bufferInfo(statePhysical_.handle()),
            bufferInfo(pages_.handle()),
            bufferInfo(stateWrite),
        };
        std::array<VkWriteDescriptorSet, 12> shadeWrites{};
        for (std::uint32_t i = 0; i < 4; ++i) {
            writeImage(shadeWrites[i], shadeSets_[frame], i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shadeImages[i]);
        }
        writeBuffer(shadeWrites[4], shadeSets_[frame], 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &shadeBuffers[0]);
        writeImage(shadeWrites[5], shadeSets_[frame], 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &lightingStorage);
        writeBuffer(shadeWrites[6], shadeSets_[frame], 6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &shadeBuffers[1]);
        writeBuffer(shadeWrites[7], shadeSets_[frame], 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &shadeBuffers[2]);
        writeBuffer(shadeWrites[8], shadeSets_[frame], 8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &shadeBuffers[3]);
        writeBuffer(shadeWrites[9], shadeSets_[frame], 9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &shadeBuffers[4]);
        writeBuffer(shadeWrites[10], shadeSets_[frame], 10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &shadeBuffers[5]);
        const VkDescriptorImageInfo sssrDepthInfo{sssrDepth_.sampler(), sssrDepth_.fullView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writeImage(shadeWrites[11], shadeSets_[frame], 11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sssrDepthInfo);
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(shadeWrites.size()), shadeWrites.data(), 0, nullptr);

        // Analytic far-ocean configuration.
        const VkDescriptorBufferInfo farBuffer =
            bufferInfo(farOceanConfig_.handle(), sizeof(GPUFarOceanConfig));
        VkWriteDescriptorSet farWrite{};
        writeBuffer(farWrite, farSets_[frame], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &farBuffer);
        vkUpdateDescriptorSets(device_, 1, &farWrite, 0, nullptr);

        const VkDescriptorBufferInfo authoredBuffers[] = {
            bufferInfo(authoredWaterConfig_.handle(), sizeof(GPUAuthoredWaterConfig)),
            bufferInfo(pages_.handle()),
            bufferInfo(statePageTable_.handle()),
            bufferInfo(statePhysical_.handle()),
            bufferInfo(stateWrite),
        };
        std::array<VkWriteDescriptorSet, 5> authoredWrites{};
        writeBuffer(authoredWrites[0], authoredSets_[frame], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &authoredBuffers[0]);
        for (std::uint32_t binding = 1; binding < authoredWrites.size(); ++binding) {
            writeBuffer(authoredWrites[binding], authoredSets_[frame], binding,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &authoredBuffers[binding]);
        }
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(authoredWrites.size()), authoredWrites.data(), 0, nullptr);

        // Full-screen composite and underwater volume approximation.
        const VkDescriptorImageInfo compositeImages[] = {
            opaqueColor_,
            {lighting_.sampler(), lighting_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {meta_.sampler(), meta_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            opaqueDepth_,
            {surface_.sampler(), surface_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        };
        const VkDescriptorBufferInfo underwaterBuffer =
            bufferInfo(underwaterConfig_[frame].handle(), sizeof(GPUWaterUnderwaterConfig));
        std::array<VkWriteDescriptorSet, 6> compositeWrites{};
        for (std::uint32_t i = 0; i < 4; ++i) {
            writeImage(compositeWrites[i], compositeSets_[frame], i,
                       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &compositeImages[i]);
        }
        writeBuffer(compositeWrites[4], compositeSets_[frame], 4,
                    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &underwaterBuffer);
        writeImage(compositeWrites[5], compositeSets_[frame], 5,
                   VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &compositeImages[4]);
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(compositeWrites.size()), compositeWrites.data(), 0, nullptr);
    }
}

void VirtualWaterRenderer::updateFrameBindings(std::span<const VkBuffer> instances,std::span<const VkBuffer> culling,VkDescriptorImageInfo hiz){
    if(instances.size()<FramesInFlight||culling.size()<FramesInFlight)return;std::copy_n(instances.begin(),FramesInFlight,instanceBuffers_.begin());std::copy_n(culling.begin(),FramesInFlight,cullingUniformBuffers_.begin());previousHiZ_=hiz;if(device_)writeDescriptors();
}

void VirtualWaterRenderer::rebuild(const WaterRenderWorld& world) {
    if (!device_) return;

    std::vector<GPUVirtualWaterPage> pages;
    std::vector<GPUWaterDrawTemplate> templates;
    GPUFarOceanConfig farConfig{};
    GPUAuthoredWaterConfig authoredConfig{};
    bodyCount_ = 0;
    authoredWaterActive_ = false;

    // Oceans own virtual geometry pages and an analytic horizon.
    for (const WaterRenderBody& body : world.bodies()) {
            const WaterBodyComponent& water = body.water;
            if (water.type != WaterBodyType::Ocean ||
                bodyCount_ >= MaxVirtualWaterBodies) continue;
            const auto& resource = body.meshResource;
            if (!resource || body.drawRanges.size() != StitchVariantCount) continue;

            OceanDomainConfig domain{};
            domain.waves = water.waves;
            domain.waveCount = water.waveCount;
            domain.bodyIndex = bodyCount_;
            domain.instanceIndex = body.instanceIndex;
            domain.pageBaseIndex = static_cast<std::uint32_t>(pages.size());
            domain.geometryLevelCount = GeometryClipLevels;
            auto bodyPages = buildOceanPageDomain(domain);
            pages.insert(pages.end(), bodyPages.begin(), bodyPages.end());

            for (std::uint32_t mask = 0; mask < StitchVariantCount; ++mask) {
                const auto& range = body.drawRanges[mask];
                templates.push_back({
                    range.indexCount,
                    resource->firstIndex + range.firstIndex,
                    static_cast<std::int32_t>(resource->firstVertex),
                    bodyCount_ * StitchVariantCount * VisiblePagesPerBin + mask * VisiblePagesPerBin
                });
            }

            GPUFarOceanBody& far = farConfig.bodies[farConfig.bodyCount++];
            far.instanceIndex = domain.instanceIndex;
            far.startDistance = OceanExtents[GeometryClipLevels - 1U] * 0.90F;
            far.normalCellSize = (2.0F * OceanExtents[GeometryClipLevels - 1U]) /
                                 static_cast<float>(ClipmapResolution);
            far.waveMask = 0xffU;
            ++bodyCount_;
    }

    // Lakes and rivers retain authored mesh topology, but get a state-only virtual
    // page so persistent wakes/foam use the same bounded physical cache as ocean.
    for (const WaterRenderBody& body : world.bodies()) {
            const WaterBodyComponent& water = body.water;
            if (water.type == WaterBodyType::Ocean ||
                authoredConfig.bodyCount >= MaxVirtualWaterBodies) continue;
            authoredWaterActive_ = true;

            GPUAuthoredWaterBody& authored = authoredConfig.bodies[authoredConfig.bodyCount++];
            authored.instanceIndex = body.instanceIndex;
            authored.bodyType = static_cast<std::uint32_t>(water.type);
            authored.statePageIndex = FarAnalyticPageId;

            float minX = std::numeric_limits<float>::max();
            float minZ = std::numeric_limits<float>::max();
            float maxX = std::numeric_limits<float>::lowest();
            float maxZ = std::numeric_limits<float>::lowest();
            float heightSum = 0.0F;
            std::uint32_t pointCount = 0U;
            float riverFlowX = 0.0F;
            float riverFlowZ = 0.0F;
            float riverFlowWeight = 0.0F;
            if (water.type == WaterBodyType::Lake) {
                for (const Vec3& point : water.lakeBoundary) {
                    minX = std::min(minX, point.x()); minZ = std::min(minZ, point.z());
                    maxX = std::max(maxX, point.x()); maxZ = std::max(maxZ, point.z());
                    heightSum += point.y(); ++pointCount;
                }
            } else {
                for (const RiverSplinePoint& point : water.riverSpline) {
                    const float halfWidth = std::max(point.width * 0.5F, 0.1F);
                    minX = std::min(minX, point.position.x() - halfWidth);
                    minZ = std::min(minZ, point.position.z() - halfWidth);
                    maxX = std::max(maxX, point.position.x() + halfWidth);
                    maxZ = std::max(maxZ, point.position.z() + halfWidth);
                    heightSum += point.position.y(); ++pointCount;
                }
                for (std::size_t segment = 0; segment + 1U < water.riverSpline.size(); ++segment) {
                    const RiverSplinePoint& a = water.riverSpline[segment];
                    const RiverSplinePoint& b = water.riverSpline[segment + 1U];
                    const float dx = b.position.x() - a.position.x();
                    const float dz = b.position.z() - a.position.z();
                    const float length = std::sqrt(dx * dx + dz * dz);
                    if (length <= 1.0e-4F) continue;
                    const float speed = std::max(0.0F, 0.5F * (a.flowSpeed + b.flowSpeed));
                    riverFlowX += (dx / length) * speed * length;
                    riverFlowZ += (dz / length) * speed * length;
                    riverFlowWeight += length;
                }
            }
            if (pointCount == 0U || pages.size() >= MaxPages) continue;

            GPUVirtualWaterPage statePage{};
            statePage.originX = statePage.minX = minX;
            statePage.originZ = statePage.minZ = minZ;
            statePage.maxX = maxX;
            statePage.maxZ = maxZ;
            statePage.baseHeight = heightSum / static_cast<float>(pointCount);
            const float maxSpan = std::max(std::max(maxX - minX, maxZ - minZ), 1.0F);
            statePage.cellSize = maxSpan / static_cast<float>(PageCells);
            statePage.verticalBound = MaxInteractionDisplacement;
            statePage.waveCandidateMask = water.waveCount >= 8U ? 0xffU : ((1U << water.waveCount) - 1U);
            statePage.instanceIndex = authored.instanceIndex;
            statePage.bodyIndex = authoredConfig.bodyCount - 1U;
            if (riverFlowWeight > 0.0F) {
                const float averageX = riverFlowX / riverFlowWeight;
                const float averageZ = riverFlowZ / riverFlowWeight;
                const float averageSpeed = std::sqrt(averageX * averageX + averageZ * averageZ);
                if (averageSpeed > 1.0e-4F) {
                    statePage.flowX = averageX / averageSpeed;
                    statePage.flowZ = averageZ / averageSpeed;
                    statePage.flowSpeed = averageSpeed;
                }
            }
            // PageActive intentionally remains clear: this page owns simulation state,
            // not geometry. The authored mesh is rasterized by water_authored_prepass.
            authored.statePageIndex = static_cast<std::uint32_t>(pages.size());
            pages.push_back(statePage);
    }

    pageCount_ = static_cast<std::uint32_t>(pages.size());
    drawBinCount_ = bodyCount_ * StitchVariantCount;
    active_ = bodyCount_ != 0U || authoredWaterActive_;
    if (!pages.empty()) pages_.update(pages.data(), sizeof(GPUVirtualWaterPage) * pages.size());
    if (!templates.empty()) drawTemplates_.update(templates.data(), sizeof(GPUWaterDrawTemplate) * templates.size());
    farOceanConfig_.update(&farConfig, sizeof(farConfig));
    authoredWaterConfig_.update(&authoredConfig, sizeof(authoredConfig));

    GPUWaterCullConfig config{};
    config.pageCount = pageCount_;
    config.drawBinCount = drawBinCount_;
    config.enableHiZ = previousHiZ_.imageView != VK_NULL_HANDLE ? 1U : 0U;
    cullConfig_.update(&config, sizeof(config));

    std::vector<GPUWaterPageHistory> zeroHistory(MaxPages);
    for (auto& history : history_) history.update(zeroHistory.data(), sizeof(GPUWaterPageHistory) * MaxPages);

    std::vector<std::uint32_t> invalidPages(MaxPages, InvalidPhysicalPage);
    std::vector<std::uint32_t> invalidOwners(MaxPhysicalStatePages, InvalidPhysicalPage);
    std::vector<GPUWaterPhysicalState> zeroPhysical(MaxPhysicalStatePages);
    std::vector<GPUWaterStateCell> zeroCells(MaxStateCells);
    const std::uint32_t zeroAllocator = 0;
    statePageTable_.update(invalidPages.data(), sizeof(std::uint32_t) * invalidPages.size());
    stateOwners_.update(invalidOwners.data(), sizeof(std::uint32_t) * invalidOwners.size());
    statePhysical_.update(zeroPhysical.data(), sizeof(GPUWaterPhysicalState) * zeroPhysical.size());
    stateCells_.update(zeroCells.data(), sizeof(GPUWaterStateCell) * zeroCells.size());
    stateCellsScratch_.update(zeroCells.data(), sizeof(GPUWaterStateCell) * zeroCells.size());
    stateAllocator_.update(&zeroAllocator, sizeof(zeroAllocator));
    stateCurrentScratch_ = false;
    pendingInteractions_.clear();

    // `active_` prevents these buffers from being consumed in the next
    // frame, but leaving their contents behind makes a 1 -> 0 water
    // transition fragile: a stale indirect command can be observed by any
    // future pass or diagnostic readback.  A rebuild is globally retired by
    // the caller, so explicitly reset every per-frame water output now.
    if (!active_) {
        std::vector<GPUVirtualWaterPage> emptyPages(MaxPages);
        std::vector<GPUWaterDrawTemplate> emptyTemplates(MaxDrawBins);
        std::vector<GPUVisibleWaterPage> emptyVisiblePages(MaxVisiblePageSlots);
        std::vector<std::uint32_t> emptyDrawBinCounts(MaxDrawBins);
        std::vector<VkDrawIndexedIndirectCommand> emptyIndirectCommands(MaxDrawBins);
        std::vector<std::uint32_t> emptyTileLists(4ULL * tileSettings_.maxTiles);
        std::array<std::uint32_t, 4> emptyTileCounts{};
        std::array<VkDispatchIndirectCommand, 4> emptyTileDispatch{};
        const GPUWaterPageStats emptyStats{};

        pages_.update(emptyPages.data(), sizeof(GPUVirtualWaterPage) * emptyPages.size());
        drawTemplates_.update(emptyTemplates.data(), sizeof(GPUWaterDrawTemplate) * emptyTemplates.size());
        for (std::uint32_t frame = 0; frame < FramesInFlight; ++frame) {
            visiblePages_[frame].update(emptyVisiblePages.data(),
                                        sizeof(GPUVisibleWaterPage) * emptyVisiblePages.size());
            drawBinCounts_[frame].update(emptyDrawBinCounts.data(),
                                         sizeof(std::uint32_t) * emptyDrawBinCounts.size());
            indirectCommands_[frame].update(emptyIndirectCommands.data(),
                                            sizeof(VkDrawIndexedIndirectCommand) * emptyIndirectCommands.size());
            stats_[frame].update(&emptyStats, sizeof(emptyStats));
            tileLists_[frame].update(emptyTileLists.data(),
                                     sizeof(std::uint32_t) * emptyTileLists.size());
            tileCounts_[frame].update(emptyTileCounts.data(), sizeof(emptyTileCounts));
            tileDispatch_[frame].update(emptyTileDispatch.data(), sizeof(emptyTileDispatch));
        }
    }
}

void VirtualWaterRenderer::recordCull(VkCommandBuffer cmd, std::uint32_t frame) const {
    if (!active() || pageCount_ == 0U) return;
    frame %= FramesInFlight;
    if (drawBinCount_ != 0U)
        vkCmdFillBuffer(cmd, drawBinCounts_[frame].handle(), 0,
                        sizeof(std::uint32_t) * drawBinCount_, 0);
    vkCmdFillBuffer(cmd, stats_[frame].handle(), 0, sizeof(GPUWaterPageStats), 0);
    VkMemoryBarrier2 clear{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    clear.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    clear.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    clear.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    clear.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1; dependency.pMemoryBarriers = &clear;
    vkCmdPipelineBarrier2(cmd, &dependency);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipelineLayout_,
                            0, 1, &cullSets_[frame], 0, nullptr);
    vkCmdDispatch(cmd, (pageCount_ + 63U) / 64U, 1, 1);
    storageWriteBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    if (drawBinCount_ != 0U) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, buildPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, buildPipelineLayout_,
                                0, 1, &buildSets_[frame], 0, nullptr);
        vkCmdDispatch(cmd, (drawBinCount_ + 63U) / 64U, 1, 1);
        storageWriteBarrier(cmd,
                            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
}


void VirtualWaterRenderer::queueInteraction(const Vec3& worldPosition, const float radius, const float strength) {
    if (radius <= 0.0F || std::abs(strength) <= 1.0e-6F) return;
    if (pendingInteractions_.size() == MaxInteractionEvents) {
        pendingInteractions_.erase(pendingInteractions_.begin());
    }
    pendingInteractions_.push_back({worldPosition.x(), worldPosition.z(), radius, strength});
}

void VirtualWaterRenderer::recordState(const VkCommandBuffer commandBuffer,
                                       std::uint32_t frameIndex,
                                       VkDescriptorSet sceneSet,
                                       const float deltaTime) {
    if (!active() || statePipeline_ == VK_NULL_HANDLE || pageCount_ == 0U) return;
    frameIndex %= FramesInFlight;

    const std::uint32_t eventCount = static_cast<std::uint32_t>(
        std::min<std::size_t>(pendingInteractions_.size(), MaxInteractionEvents));
    if (eventCount != 0U) {
        interactionEvents_[frameIndex].update(
            pendingInteractions_.data(), sizeof(GPUWaterInteractionEvent) * eventCount);
    }
    pendingInteractions_.clear();

    // FramesInFlight is three, therefore frame-slot parity cannot represent temporal
    // ping-pong. Update only the current, fence-safe descriptor sets from the actual
    // committed state buffer. The simulation writes the other atlas.
    const VkBuffer readBuffer = stateCurrentScratch_ ? stateCellsScratch_.handle() : stateCells_.handle();
    const VkBuffer writeBufferHandle = stateCurrentScratch_ ? stateCells_.handle() : stateCellsScratch_.handle();
    const VkDescriptorBufferInfo readInfo{readBuffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo writeInfo{writeBufferHandle, 0, VK_WHOLE_SIZE};
    std::array<VkWriteDescriptorSet, 5> writes{};
    auto setStorage = [](VkWriteDescriptorSet& w, VkDescriptorSet set, std::uint32_t binding,
                         const VkDescriptorBufferInfo* info) {
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set; w.dstBinding = binding; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = info;
    };
    setStorage(writes[0], stateSets_[frameIndex], 8, &readInfo);
    setStorage(writes[1], stateSets_[frameIndex], 9, &writeInfo);
    setStorage(writes[2], drawSets_[frameIndex], 4, &writeInfo);
    setStorage(writes[3], shadeSets_[frameIndex], 10, &writeInfo);
    setStorage(writes[4], authoredSets_[frameIndex], 4, &writeInfo);
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    const StatePush push{
        .eventCount = eventCount,
        .pageCount = pageCount_,
        .frameIndex = frameIndex,
        .pad = 0U,
        .deltaTime = std::clamp(deltaTime, 0.0F, 0.1F),
    };
    const std::array descriptorSets{sceneSet, stateSets_[frameIndex]};
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, statePipelineLayout_,
                            0, static_cast<std::uint32_t>(descriptorSets.size()),
                            descriptorSets.data(), 0, nullptr);
    vkCmdPushConstants(commandBuffer, statePipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);

    // Allocation/eviction is intentionally a separate deterministic pass. It
    // prevents two page workgroups from claiming the same physical slot when
    // the bounded cache wraps during a large first-frame allocation burst.
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, stateAllocatePipeline_);
    vkCmdDispatch(commandBuffer, 1, 1, 1);
    storageWriteBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, statePipeline_);
    vkCmdDispatch(commandBuffer, 1, 1, pageCount_);
    storageWriteBarrier(commandBuffer,
                        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    stateCurrentScratch_ = !stateCurrentScratch_;
}

void VirtualWaterRenderer::updateUnderwaterState(std::uint32_t frameIndex,
                                                  Registry& registry,
                                                  const Vec3& cameraPosition,
                                                  const float time) {
    if (!device_) return;
    frameIndex %= FramesInFlight;
    GPUWaterUnderwaterConfig config{};
    const auto query = WaterSystem::query(registry, cameraPosition, time);
    if (query && query->immersion > 0.0F && query->waterBody != NullEntity &&
        registry.has<WaterBodyComponent>(query->waterBody)) {
        const WaterBodyComponent& water = registry.get<WaterBodyComponent>(query->waterBody);
        if (water.enableUnderwater) {
            config.shallowR = water.shallowColor.x();
            config.shallowG = water.shallowColor.y();
            config.shallowB = water.shallowColor.z();
            config.immersion = query->immersion;
            config.deepR = water.deepColor.x();
            config.deepG = water.deepColor.y();
            config.deepB = water.deepColor.z();
            config.maxDepth = std::max(query->depth > 0.0F ? query->depth : water.maxDepth, 0.01F);
            config.absorptionR = std::max(water.absorptionCoefficient.x(), 0.0F);
            config.absorptionG = std::max(water.absorptionCoefficient.y(), 0.0F);
            config.absorptionB = std::max(water.absorptionCoefficient.z(), 0.0F);
            config.time = time;
            config.scatteringR = std::max(water.scatteringCoefficient.x(), 0.0F);
            config.scatteringG = std::max(water.scatteringCoefficient.y(), 0.0F);
            config.scatteringB = std::max(water.scatteringCoefficient.z(), 0.0F);
            config.surfaceHeight = query->surfaceHeight;
            config.active = 1U;
            config.causticsEnabled = water.enableCaustics ? 1U : 0U;
        }
    }
    underwaterConfig_[frameIndex].update(&config, sizeof(config));
}

void VirtualWaterRenderer::recordPrepass(VkCommandBuffer cmd, std::uint32_t frame,
                                             VkDescriptorSet sceneSet, VkBuffer vb, VkBuffer ib,
                                             const Culling::IndexedIndirectDrawCount& authoredWaterDraw,
                                             VkDeviceSize authoredCommandOffset,
                                             VkDeviceSize authoredCountOffset) const {
    if (!active()) return;
    frame %= FramesInFlight;
    std::array<VkClearValue, 4> clears{};
    clears[0].color = {{0, 0, 0, 0}};
    clears[1].color = {{0, 0, 0, 0}};
    clears[2].color = {{0, 0, 0, 0}};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = prepassPipeline_.renderPass(); rp.framebuffer = prepassFramebuffer_;
    rp.renderArea.extent = extent_; rp.clearValueCount = static_cast<std::uint32_t>(clears.size());
    rp.pClearValues = clears.data();
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    const VkViewport viewport{0, 0, float(extent_.width), float(extent_.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(cmd, 0, 1, &viewport); vkCmdSetScissor(cmd, 0, 1, &scissor);

    if (bodyCount_ != 0U && farPrepassPipeline_.handle() != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, farPrepassPipeline_.handle());
        const std::array sets{sceneSet, farSets_[frame]};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, farPrepassPipeline_.layout(),
                                0, static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    if (pageCount_ != 0U) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, prepassPipeline_.handle());
        const std::array sets{sceneSet, drawSets_[frame]};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, prepassPipeline_.layout(),
                                0, static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
        VkDeviceSize offset = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
        vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexedIndirect(cmd, indirectCommands_[frame].handle(), 0, drawBinCount_,
                                 sizeof(VkDrawIndexedIndirectCommand));
    }

    // Lakes and rivers keep their authored mesh topology, but only rasterize a cheap
    // prepass here; their expensive optics now run through the same tile scheduler.
    if (authoredWaterActive_ && authoredPrepassPipeline_.handle() != VK_NULL_HANDLE && authoredWaterDraw.valid()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, authoredPrepassPipeline_.handle());
        const std::array authoredDescriptorSets{sceneSet, authoredSets_[frame]};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, authoredPrepassPipeline_.layout(),
                                0, static_cast<std::uint32_t>(authoredDescriptorSets.size()),
                                authoredDescriptorSets.data(), 0, nullptr);
        VkDeviceSize offset = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
        vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
        authoredWaterDraw.record(cmd, authoredCommandOffset, authoredCountOffset);
    }
    vkCmdEndRenderPass(cmd);

    VkMemoryBarrier2 ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    ready.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    ready.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    ready.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    ready.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1; dependency.pMemoryBarriers = &ready;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void VirtualWaterRenderer::recordAdaptiveShading(VkCommandBuffer cmd, std::uint32_t frame,
                                                   VkDescriptorSet sceneSet) {
    if (!active()) return;
    frame %= FramesInFlight;

    // Build a MIN-depth hierarchy dedicated to water rays. The renderer's
    // ordinary Hi-Z is a MAX hierarchy for conservative object occlusion and
    // has the opposite reduction semantics required by SSSR/refraction.
    VkImageMemoryBarrier2 depthToGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    depthToGeneral.srcStageMask = sssrDepthInitialized_ ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE;
    depthToGeneral.srcAccessMask = sssrDepthInitialized_ ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0;
    depthToGeneral.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    depthToGeneral.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    depthToGeneral.oldLayout = sssrDepthInitialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    depthToGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    depthToGeneral.image = sssrDepth_.image();
    depthToGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, sssrDepth_.mipCount(), 0, 1};
    VkDependencyInfo depthDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depthDependency.imageMemoryBarrierCount = 1;
    depthDependency.pImageMemoryBarriers = &depthToGeneral;
    vkCmdPipelineBarrier2(cmd, &depthDependency);
    sssrDepthPass_.record(cmd, sssrDepth_);
    sssrDepthInitialized_ = true;

    vkCmdFillBuffer(cmd, tileCounts_[frame].handle(), 0, sizeof(std::uint32_t) * 4, 0);
    VkMemoryBarrier2 clear{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    clear.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    clear.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    clear.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    clear.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &clear;
    vkCmdPipelineBarrier2(cmd, &d);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, classifyPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, classifyPipelineLayout_,
                            0, 1, &classifySets_[frame], 0, nullptr);
    vkCmdDispatch(cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
    storageWriteBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, buildDispatchPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, buildDispatchPipelineLayout_,
                            0, 1, &buildDispatchSets_[frame], 0, nullptr);
    vkCmdDispatch(cmd, 1, 1, 1);
    storageWriteBarrier(cmd, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);

    imageBarrier(cmd, lighting_.image(),
                 lightingInitialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_GENERAL,
                 lightingInitialized_ ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                 lightingInitialized_ ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    lightingInitialized_ = true;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shadePipeline_);
    const std::array sets{sceneSet, shadeSets_[frame]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shadePipelineLayout_,
                            0, static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
    for (std::uint32_t tier = 0; tier < 4; ++tier) {
        ShadePush push{tier};
        vkCmdPushConstants(cmd, shadePipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatchIndirect(cmd, tileDispatch_[frame].handle(), tier * sizeof(VkDispatchIndirectCommand));
    }
    imageBarrier(cmd, lighting_.image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void VirtualWaterRenderer::recordComposite(VkCommandBuffer cmd, std::uint32_t frame,
                                                  VkDescriptorSet sceneSet) const {
    if (!active()) return;
    frame %= FramesInFlight;
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = compositePipeline_.renderPass();
    rp.framebuffer = compositeFramebuffer_;
    rp.renderArea.extent = extent_;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipeline_.handle());
    const std::array descriptorSets{sceneSet, compositeSets_[frame]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipeline_.layout(),
                            0, static_cast<std::uint32_t>(descriptorSets.size()),
                            descriptorSets.data(), 0, nullptr);
    const VkViewport viewport{0, 0, float(extent_.width), float(extent_.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

void VirtualWaterRenderer::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        if (prepassFramebuffer_ != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, prepassFramebuffer_, nullptr);
        if (compositeFramebuffer_ != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, compositeFramebuffer_, nullptr);
        sssrDepthPass_.destroy();
        for (const VkPipeline pipeline : {cullPipeline_, buildPipeline_, classifyPipeline_,
                                          buildDispatchPipeline_, shadePipeline_, stateAllocatePipeline_, statePipeline_,
                                          sssrInitPipeline_, sssrReducePipeline_}) {
            if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline, nullptr);
        }
        for (const VkPipelineLayout layout : {cullPipelineLayout_, buildPipelineLayout_, classifyPipelineLayout_,
                                               buildDispatchPipelineLayout_, shadePipelineLayout_, statePipelineLayout_,
                                               sssrInitPipelineLayout_, sssrReducePipelineLayout_}) {
            if (layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, layout, nullptr);
        }
        if (descriptorPool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        for (const VkDescriptorSetLayout layout : {cullLayout_, buildLayout_, drawLayout_, classifyLayout_,
                                                    buildDispatchLayout_, shadeLayout_, compositeLayout_, stateLayout_,
                                                    farLayout_, authoredLayout_, sssrInitLayout_, sssrReduceLayout_}) {
            if (layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, layout, nullptr);
        }
    }

    prepassPipeline_.destroy();
    farPrepassPipeline_.destroy();
    authoredPrepassPipeline_.destroy();
    compositePipeline_.destroy();
    surface_.destroy();
    meta_.destroy();
    velocity_.destroy();
    lighting_.destroy();
    sssrDepth_.destroy();
    pages_.destroy();
    drawTemplates_.destroy();
    cullConfig_.destroy();
    farOceanConfig_.destroy();
    authoredWaterConfig_.destroy();
    tileSettingsBuffer_.destroy();
    statePageTable_.destroy();
    stateOwners_.destroy();
    statePhysical_.destroy();
    stateCells_.destroy();
    stateCellsScratch_.destroy();
    stateAllocator_.destroy();
    for (auto& buffer : interactionEvents_) buffer.destroy();
    for (auto& buffer : underwaterConfig_) buffer.destroy();
    for (auto* array : {&visiblePages_, &drawBinCounts_, &indirectCommands_, &history_, &stats_,
                        &tileLists_, &tileCounts_, &tileDispatch_}) {
        for (auto& buffer : *array) buffer.destroy();
    }

    physicalDevice_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    allocator_ = VK_NULL_HANDLE;
    assets_ = nullptr;
    extent_ = {};
    depthView_ = VK_NULL_HANDLE;
    pageCount_ = drawBinCount_ = bodyCount_ = 0;
    active_ = false;
    authoredWaterActive_ = false;
    lightingInitialized_ = false;
    stateCurrentScratch_ = false;
    sssrDepthInitialized_ = false;
    pendingInteractions_.clear();

    cullLayout_ = buildLayout_ = drawLayout_ = classifyLayout_ = buildDispatchLayout_ =
        shadeLayout_ = compositeLayout_ = stateLayout_ = farLayout_ = authoredLayout_ = sssrInitLayout_ = sssrReduceLayout_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    cullPipelineLayout_ = buildPipelineLayout_ = classifyPipelineLayout_ = buildDispatchPipelineLayout_ =
        shadePipelineLayout_ = statePipelineLayout_ = sssrInitPipelineLayout_ = sssrReducePipelineLayout_ = VK_NULL_HANDLE;
    cullPipeline_ = buildPipeline_ = classifyPipeline_ = buildDispatchPipeline_ = shadePipeline_ =
        stateAllocatePipeline_ = statePipeline_ = sssrInitPipeline_ = sssrReducePipeline_ = VK_NULL_HANDLE;
    prepassFramebuffer_ = compositeFramebuffer_ = VK_NULL_HANDLE;

    cullSets_.fill(VK_NULL_HANDLE);
    buildSets_.fill(VK_NULL_HANDLE);
    drawSets_.fill(VK_NULL_HANDLE);
    classifySets_.fill(VK_NULL_HANDLE);
    buildDispatchSets_.fill(VK_NULL_HANDLE);
    shadeSets_.fill(VK_NULL_HANDLE);
    compositeSets_.fill(VK_NULL_HANDLE);
    stateSets_.fill(VK_NULL_HANDLE);
    farSets_.fill(VK_NULL_HANDLE);
    authoredSets_.fill(VK_NULL_HANDLE);
    instanceBuffers_.fill(VK_NULL_HANDLE);
    cullingUniformBuffers_.fill(VK_NULL_HANDLE);
    previousHiZ_ = {};
    opaqueColor_ = {};
    opaqueDepth_ = {};
}
} // namespace Engine::Water
