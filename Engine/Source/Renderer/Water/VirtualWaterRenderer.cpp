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
#include <bit>
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
                                  VkDescriptorImageInfo opaqueDepth, std::span<const VkDescriptorImageInfo> previousHiZ,
                                  std::span<const VkBuffer> instanceBuffers,
                                  std::span<const VkBuffer> cullingUniformBuffers,
                                  const bool enableHiZ) {
    destroy();
    if(!device || !extent.width || !extent.height || instanceBuffers.size()<FramesInFlight ||
       cullingUniformBuffers.size()<FramesInFlight || previousHiZ.size()<FramesInFlight) throw std::invalid_argument("Invalid VirtualWaterRenderer resources");
    physicalDevice_=physicalDevice; device_=device; allocator_=allocator; assets_=&assets; extent_=extent; depthView_=depthView;
    opaqueColor_=opaqueColor; opaqueDepth_=opaqueDepth; std::copy_n(previousHiZ.begin(),FramesInFlight,previousHiZ_.begin());
    hiZEnabled_ = enableHiZ;
    std::copy_n(instanceBuffers.begin(),FramesInFlight,instanceBuffers_.begin());
    std::copy_n(cullingUniformBuffers.begin(),FramesInFlight,cullingUniformBuffers_.begin());
    try {
        createBuffers();
        createOceanGeometry();
        surface_.create(physicalDevice_,device_,extent_,allocator_,VK_FILTER_NEAREST,VK_FORMAT_R16G16B16A16_SFLOAT,false);
        meta_.create(physicalDevice_,device_,extent_,allocator_,VK_FILTER_NEAREST,VK_FORMAT_R32G32_SFLOAT,false);
        velocity_.create(physicalDevice_,device_,extent_,allocator_,VK_FILTER_NEAREST,VK_FORMAT_R16G16_SFLOAT,false);
        for (auto& lighting : lighting_)
            lighting.create(physicalDevice_,device_,extent_,allocator_,VK_FILTER_LINEAR,VK_FORMAT_R16G16B16A16_SFLOAT,true);
        sssrDepth_.create(physicalDevice_, device_, (extent_.width + 1U) / 2U, (extent_.height + 1U) / 2U, allocator_);
        createDescriptors(sceneLayout); createPipelines(sceneLayout,depthFormat); hdrTargetView_ = hdrTargetView; writeDescriptors();
    } catch (...) { destroy(); throw; }
}

void VirtualWaterRenderer::createBuffers() {
    const auto host=[&](Buffer& b,VkDeviceSize size,VkBufferUsageFlags usage){
        b.createHostVisible(physicalDevice_,device_,std::max<VkDeviceSize>(size,16),usage,allocator_);
    };
    host(pages_, sizeof(GPUVirtualWaterPage)*MaxPages, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    host(drawTemplates_, sizeof(GPUWaterDrawTemplate)*MaxDrawBins, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    host(farOceanConfig_, sizeof(GPUFarOceanConfig), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    const GPUFarOceanConfig emptyFar{}; farOceanConfig_.update(&emptyFar, sizeof(emptyFar));
    host(authoredWaterConfig_, sizeof(GPUAuthoredWaterConfig), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    const GPUAuthoredWaterConfig emptyAuthored{}; authoredWaterConfig_.update(&emptyAuthored, sizeof(emptyAuthored));

    tileSettings_.width=extent_.width;
    tileSettings_.height=extent_.height;
    tileSettings_.tilesX=(extent_.width+7U)/8U;
    tileSettings_.maxTiles=tileSettings_.tilesX*((extent_.height+7U)/8U);

    host(statePageTable_, sizeof(GPUWaterStatePhysicalRef) * MaxPages, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    host(statePhysical_, sizeof(GPUWaterPhysicalState) * MaxPhysicalStatePages, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    host(stateCells_, sizeof(GPUWaterStateCell) * MaxStateCells, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    host(stateCellsScratch_, sizeof(GPUWaterStateCell) * MaxStateCells, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    host(stateAllocator_, sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    std::vector<GPUWaterStatePhysicalRef> invalidRefs(MaxPages);
    for (auto& ref : invalidRefs) ref.slot = InvalidPhysicalPage;
    std::vector<GPUWaterPhysicalState> zeroPhysical(MaxPhysicalStatePages);
    for (auto& physical : zeroPhysical) {
        physical.ownerBodyIndex = InvalidPhysicalPage;
        physical.generation = 0U;
        physical.updateRatePacked = 1U;
    }
    std::vector<GPUWaterStateCell> zeroCells(MaxStateCells);
    const std::uint32_t zeroAllocator = 0;
    statePageTable_.update(invalidRefs.data(), sizeof(GPUWaterStatePhysicalRef) * invalidRefs.size());
    statePhysical_.update(zeroPhysical.data(), sizeof(GPUWaterPhysicalState) * zeroPhysical.size());
    stateCells_.update(zeroCells.data(), sizeof(GPUWaterStateCell) * zeroCells.size());
    stateCellsScratch_.update(zeroCells.data(), sizeof(GPUWaterStateCell) * zeroCells.size());
    stateAllocator_.update(&zeroAllocator, sizeof(zeroAllocator));

    const std::vector<GPUWaterPageHistory> histories(MaxPages);
    const std::vector<std::uint32_t> zeroHistogram(256U,0U);
    const std::vector<std::uint32_t> zeroCandidates(tileSettings_.maxTiles,0U);
    GPUWaterFrameBudget gpuBudget{};
    gpuBudget.targetGpuMs=frameBudget_.targetGpuMs;
    gpuBudget.qualityScale=frameBudget_.qualityScale;
    gpuBudget.feedbackGain=0.08F;
    gpuBudget.maxFullRateSamples=tileSettings_.maxTiles*64U;
    gpuBudget.maxReflectionWork=frameBudget_.maxReflectionWork;
    gpuBudget.maxRefractionWork=frameBudget_.maxRefractionWork;
    gpuBudget.maxStateSimulationCells=frameBudget_.maxStateSimulationCells;
    for(std::uint32_t f=0;f<FramesInFlight;++f){
        host(cullConfig_[f],sizeof(GPUWaterCullConfig),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        const GPUWaterCullConfig emptyCull{}; cullConfig_[f].update(&emptyCull,sizeof(emptyCull));
        host(visiblePages_[f],sizeof(GPUVisibleWaterPage)*MaxVisiblePageSlots,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        host(drawBinCounts_[f],sizeof(std::uint32_t)*MaxDrawBins,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        host(indirectCommands_[f],sizeof(VkDrawIndexedIndirectCommand)*MaxDrawBins,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        host(history_[f],sizeof(GPUWaterPageHistory)*MaxPages,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); history_[f].update(histories.data(),sizeof(GPUWaterPageHistory)*MaxPages);
        host(stats_[f],sizeof(GPUWaterPageStats),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        host(tileSettingsBuffer_[f],sizeof(GPUWaterTileSettings),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT); tileSettingsBuffer_[f].update(&tileSettings_,sizeof(tileSettings_));
        host(frameBudgetBuffer_[f],sizeof(GPUWaterFrameBudget),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT); frameBudgetBuffer_[f].update(&gpuBudget,sizeof(gpuBudget));
        host(importanceHistogram_[f],sizeof(std::uint32_t)*256U,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT); importanceHistogram_[f].update(zeroHistogram.data(),sizeof(std::uint32_t)*zeroHistogram.size());
        host(candidateTiles_[f],sizeof(std::uint32_t)*tileSettings_.maxTiles,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT); candidateTiles_[f].update(zeroCandidates.data(),sizeof(std::uint32_t)*zeroCandidates.size());
        host(tileLists_[f],sizeof(std::uint32_t)*4ull*tileSettings_.maxTiles,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        host(tileCounts_[f],sizeof(std::uint32_t)*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        host(tileDispatch_[f],sizeof(VkDispatchIndirectCommand)*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        host(interactionEvents_[f], sizeof(GPUWaterInteractionEvent) * MaxInteractionEvents, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        host(underwaterConfig_[f], sizeof(GPUWaterUnderwaterConfig), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        const GPUWaterUnderwaterConfig emptyUnderwater{};
        underwaterConfig_[f].update(&emptyUnderwater, sizeof(emptyUnderwater));
    }
}

void VirtualWaterRenderer::createOceanGeometry() {
    WaterBodyComponent ocean{};
    ocean.type = WaterBodyType::Ocean;
    const Mesh mesh = WaterSystem::buildMesh(ocean);
    if (mesh.vertices.empty() || mesh.indices.empty() ||
        mesh.drawRanges.size() != StitchVariantCount) {
        throw std::runtime_error("Virtual Water clipmap topology is invalid");
    }
    std::vector<GpuVertex> vertices;
    vertices.reserve(mesh.vertices.size());
    for (const Vertex& vertex : mesh.vertices) vertices.push_back(GpuVertex::pack(vertex));
    oceanVertexBuffer_.createHostVisible(physicalDevice_, device_,
        sizeof(GpuVertex) * vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, allocator_);
    oceanVertexBuffer_.update(vertices.data(), sizeof(GpuVertex) * vertices.size());
    oceanIndexBuffer_.createHostVisible(physicalDevice_, device_,
        sizeof(std::uint32_t) * mesh.indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, allocator_);
    oceanIndexBuffer_.update(mesh.indices.data(), sizeof(std::uint32_t) * mesh.indices.size());
    oceanDrawRanges_ = mesh.drawRanges;
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
        VkDescriptorSetLayoutBinding{9,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{10,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr}};
    cullLayout_=makeLayout(device_,cullBindings);
    const std::array buildBindings{
        VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{3,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,C,nullptr}};
    buildLayout_=makeLayout(device_,buildBindings);
    const std::array drawBindings{
        VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,V|F,nullptr},
        VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,V,nullptr},
        VkDescriptorSetLayoutBinding{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,V,nullptr},
        VkDescriptorSetLayoutBinding{3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,V,nullptr},
        VkDescriptorSetLayoutBinding{4,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,V,nullptr}};
    drawLayout_=makeLayout(device_,drawBindings);
    const std::array classifyBindings{
        VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{4,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{5,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{6,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{7,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr}};
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
        VkDescriptorSetLayoutBinding{11,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{12,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{13,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,C,nullptr},
        VkDescriptorSetLayoutBinding{14,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,C,nullptr}};
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
    const std::array farBindings{VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,V|F,nullptr}};
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
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,FramesInFlight*96U},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,FramesInFlight*24U},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,FramesInFlight*24U+64U},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,FramesInFlight*4U+64U}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets=FramesInFlight*10U+64U;pool.poolSizeCount=static_cast<std::uint32_t>(sizes.size());pool.pPoolSizes=sizes.data();
    if(vkCreateDescriptorPool(device_,&pool,nullptr,&descriptorPool_)!=VK_SUCCESS)throw std::runtime_error("Could not create virtual-water descriptor pool");
    auto alloc=[&](VkDescriptorSetLayout layout,auto& sets){std::array<VkDescriptorSetLayout,FramesInFlight> ls{};ls.fill(layout);VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};a.descriptorPool=descriptorPool_;a.descriptorSetCount=FramesInFlight;a.pSetLayouts=ls.data();if(vkAllocateDescriptorSets(device_,&a,sets.data())!=VK_SUCCESS)throw std::runtime_error("Could not allocate virtual-water descriptor sets");};
    alloc(cullLayout_,cullSets_);alloc(buildLayout_,buildSets_);alloc(classifyLayout_,classifySets_);alloc(buildDispatchLayout_,buildDispatchSets_);alloc(compositeLayout_,compositeSets_);alloc(farLayout_,farSets_);
    for (auto& sets : drawSets_) alloc(drawLayout_, sets);
    for (auto& sets : shadeSets_) alloc(shadeLayout_, sets);
    for (auto& sets : stateSets_) alloc(stateLayout_, sets);
    for (auto& sets : authoredSets_) alloc(authoredLayout_, sets);

    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    std::array cullLayouts{sceneLayout,cullLayout_}; pli.setLayoutCount=2;pli.pSetLayouts=cullLayouts.data();
    if(vkCreatePipelineLayout(device_,&pli,nullptr,&cullPipelineLayout_)!=VK_SUCCESS)throw std::runtime_error("water cull layout");
    pli.setLayoutCount=1;pli.pSetLayouts=&buildLayout_; if(vkCreatePipelineLayout(device_,&pli,nullptr,&buildPipelineLayout_)!=VK_SUCCESS)throw std::runtime_error("water indirect layout");
    pli.pSetLayouts=&classifyLayout_; if(vkCreatePipelineLayout(device_,&pli,nullptr,&classifyPipelineLayout_)!=VK_SUCCESS)throw std::runtime_error("water classify layout");
    pli.pSetLayouts=&buildDispatchLayout_; if(vkCreatePipelineLayout(device_,&pli,nullptr,&buildDispatchPipelineLayout_)!=VK_SUCCESS)throw std::runtime_error("water dispatch layout");
    std::array shadeLayouts{sceneLayout,shadeLayout_}; VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(ShadePush)}; pli.setLayoutCount=2;pli.pSetLayouts=shadeLayouts.data();pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&pc;
    if(vkCreatePipelineLayout(device_,&pli,nullptr,&shadePipelineLayout_)!=VK_SUCCESS)throw std::runtime_error("water shade layout");
    VkPushConstantRange statePc{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(StatePush)};
    std::array stateLayouts{sceneLayout,stateLayout_}; pli.setLayoutCount=2;pli.pSetLayouts=stateLayouts.data();pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&statePc;
    if(vkCreatePipelineLayout(device_,&pli,nullptr,&statePipelineLayout_)!=VK_SUCCESS)throw std::runtime_error("water state layout");
    pli.pushConstantRangeCount=0;pli.pPushConstantRanges=nullptr;
    std::array sssrInitLayouts{sceneLayout,sssrInitLayout_};pli.setLayoutCount=2;pli.pSetLayouts=sssrInitLayouts.data();
    if(vkCreatePipelineLayout(device_,&pli,nullptr,&sssrInitPipelineLayout_)!=VK_SUCCESS)throw std::runtime_error("water SSSR init layout");
    std::array sssrReduceLayouts{sceneLayout,sssrReduceLayout_};pli.pSetLayouts=sssrReduceLayouts.data();
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

void VirtualWaterRenderer::writeDescriptors() {
    const auto bufferInfo=[](VkBuffer buffer,VkDeviceSize size=VK_WHOLE_SIZE){return VkDescriptorBufferInfo{buffer,0,size};};
    const auto writeBuffer=[](VkWriteDescriptorSet& w,VkDescriptorSet set,std::uint32_t binding,VkDescriptorType type,const VkDescriptorBufferInfo* info){
        w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.dstBinding=binding;w.descriptorCount=1;w.descriptorType=type;w.pBufferInfo=info;
    };
    const auto writeImage=[](VkWriteDescriptorSet& w,VkDescriptorSet set,std::uint32_t binding,VkDescriptorType type,const VkDescriptorImageInfo* info){
        w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.dstBinding=binding;w.descriptorCount=1;w.descriptorType=type;w.pImageInfo=info;
    };
    for(std::uint32_t frame=0;frame<FramesInFlight;++frame){
        const VkDescriptorBufferInfo cullBuffers[] = {
            bufferInfo(pages_.handle()), bufferInfo(instanceBuffers_[frame]), bufferInfo(visiblePages_[frame].handle()),
            bufferInfo(drawBinCounts_[frame].handle()), bufferInfo(cullingUniformBuffers_[frame]),
            bufferInfo(history_[(frame+FramesInFlight-1U)%FramesInFlight].handle()), bufferInfo(history_[frame].handle()),
            bufferInfo(stats_[frame].handle()), bufferInfo(cullConfig_[frame].handle(),sizeof(GPUWaterCullConfig)),
            bufferInfo(candidateTiles_[frame].handle())};
        std::array<VkWriteDescriptorSet,11> cullWrites{};
        writeBuffer(cullWrites[0],cullSets_[frame],0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&cullBuffers[0]);
        writeBuffer(cullWrites[1],cullSets_[frame],1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&cullBuffers[1]);
        writeBuffer(cullWrites[2],cullSets_[frame],2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&cullBuffers[2]);
        writeBuffer(cullWrites[3],cullSets_[frame],3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&cullBuffers[3]);
        writeImage(cullWrites[4],cullSets_[frame],4,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&previousHiZ_[frame]);
        writeBuffer(cullWrites[5],cullSets_[frame],5,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,&cullBuffers[4]);
        writeBuffer(cullWrites[6],cullSets_[frame],6,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&cullBuffers[5]);
        writeBuffer(cullWrites[7],cullSets_[frame],7,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&cullBuffers[6]);
        writeBuffer(cullWrites[8],cullSets_[frame],8,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&cullBuffers[7]);
        writeBuffer(cullWrites[9],cullSets_[frame],9,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,&cullBuffers[8]);
        writeBuffer(cullWrites[10],cullSets_[frame],10,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&cullBuffers[9]);
        vkUpdateDescriptorSets(device_,static_cast<std::uint32_t>(cullWrites.size()),cullWrites.data(),0,nullptr);

        const VkDescriptorBufferInfo buildBuffers[]{bufferInfo(drawBinCounts_[frame].handle()),bufferInfo(drawTemplates_.handle()),bufferInfo(indirectCommands_[frame].handle()),bufferInfo(cullConfig_[frame].handle(),sizeof(GPUWaterCullConfig))};
        std::array<VkWriteDescriptorSet,4> buildWrites{};
        for(std::uint32_t i=0;i<4;++i)writeBuffer(buildWrites[i],buildSets_[frame],i,i==3?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&buildBuffers[i]);
        vkUpdateDescriptorSets(device_,4,buildWrites.data(),0,nullptr);

        for (std::uint32_t stateVariant = 0; stateVariant < 2; ++stateVariant) {
        const VkBuffer stateRead=stateVariant ? stateCellsScratch_.handle() : stateCells_.handle();
        const VkBuffer stateWrite=stateVariant ? stateCells_.handle() : stateCellsScratch_.handle();
        const VkDescriptorBufferInfo drawBuffers[]{bufferInfo(pages_.handle()),bufferInfo(visiblePages_[frame].handle()),bufferInfo(statePageTable_.handle()),bufferInfo(statePhysical_.handle()),bufferInfo(stateWrite)};
        std::array<VkWriteDescriptorSet,5> drawWrites{};
        for(std::uint32_t i=0;i<5;++i)writeBuffer(drawWrites[i],drawSets_[stateVariant][frame],i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&drawBuffers[i]);
        vkUpdateDescriptorSets(device_,5,drawWrites.data(),0,nullptr);

        const VkDescriptorImageInfo metaInfo{meta_.sampler(),meta_.imageView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo surfaceInfo{surface_.sampler(),surface_.imageView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo classifyBuffers[]{bufferInfo(tileLists_[frame].handle()),bufferInfo(tileCounts_[frame].handle()),bufferInfo(tileSettingsBuffer_[frame].handle(),sizeof(GPUWaterTileSettings)),bufferInfo(importanceHistogram_[frame].handle()),bufferInfo(frameBudgetBuffer_[frame].handle(),sizeof(GPUWaterFrameBudget)),bufferInfo(candidateTiles_[frame].handle())};
        std::array<VkWriteDescriptorSet,8> classifyWrites{};
        writeImage(classifyWrites[0],classifySets_[frame],0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&metaInfo);
        writeImage(classifyWrites[1],classifySets_[frame],1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&surfaceInfo);
        writeBuffer(classifyWrites[2],classifySets_[frame],2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&classifyBuffers[0]);
        writeBuffer(classifyWrites[3],classifySets_[frame],3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&classifyBuffers[1]);
        writeBuffer(classifyWrites[4],classifySets_[frame],4,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,&classifyBuffers[2]);
        writeBuffer(classifyWrites[5],classifySets_[frame],5,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&classifyBuffers[3]);
        writeBuffer(classifyWrites[6],classifySets_[frame],6,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&classifyBuffers[4]);
        writeBuffer(classifyWrites[7],classifySets_[frame],7,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&classifyBuffers[5]);
        vkUpdateDescriptorSets(device_,static_cast<std::uint32_t>(classifyWrites.size()),classifyWrites.data(),0,nullptr);

        const VkDescriptorBufferInfo dispatchBuffers[]{bufferInfo(tileCounts_[frame].handle()),bufferInfo(tileDispatch_[frame].handle())};
        std::array<VkWriteDescriptorSet,2> dispatchWrites{};
        writeBuffer(dispatchWrites[0],buildDispatchSets_[frame],0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&dispatchBuffers[0]);
        writeBuffer(dispatchWrites[1],buildDispatchSets_[frame],1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&dispatchBuffers[1]);
        vkUpdateDescriptorSets(device_,2,dispatchWrites.data(),0,nullptr);

        const VkDescriptorBufferInfo stateBuffers[]{
            bufferInfo(pages_.handle()), bufferInfo(instanceBuffers_[frame]), bufferInfo(history_[frame].handle()),
            bufferInfo(statePageTable_.handle()), bufferInfo(statePhysical_.handle()), bufferInfo(interactionEvents_[frame].handle()),
            bufferInfo(stateAllocator_.handle()), bufferInfo(stateRead), bufferInfo(stateWrite),
            bufferInfo(frameBudgetBuffer_[frame].handle(),sizeof(GPUWaterFrameBudget))};
        std::array<VkWriteDescriptorSet,10> stateWrites{};
        for(std::uint32_t i=0;i<stateWrites.size();++i)
            writeBuffer(stateWrites[i],stateSets_[stateVariant][frame],i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&stateBuffers[i]);
        vkUpdateDescriptorSets(device_,static_cast<std::uint32_t>(stateWrites.size()),stateWrites.data(),0,nullptr);

        const VkDescriptorImageInfo shadeImages[]{{surface_.sampler(),surface_.imageView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{meta_.sampler(),meta_.imageView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},opaqueColor_,opaqueDepth_};
        const std::uint32_t previousFrame=(frame+FramesInFlight-1U)%FramesInFlight;
        const VkDescriptorImageInfo lightingStorage{VK_NULL_HANDLE,lighting_[frame].imageView(),VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo previousLighting{lighting_[previousFrame].sampler(),lighting_[previousFrame].imageView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo waterVelocity{velocity_.sampler(),velocity_.imageView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo shadeBuffers[]{bufferInfo(tileLists_[frame].handle()),bufferInfo(tileSettingsBuffer_[frame].handle(),sizeof(GPUWaterTileSettings)),bufferInfo(statePageTable_.handle()),bufferInfo(statePhysical_.handle()),bufferInfo(pages_.handle()),bufferInfo(stateWrite)};
        std::array<VkWriteDescriptorSet,15> shadeWrites{};
        for(std::uint32_t i=0;i<4;++i)writeImage(shadeWrites[i],shadeSets_[stateVariant][frame],i,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&shadeImages[i]);
        writeBuffer(shadeWrites[4],shadeSets_[stateVariant][frame],4,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&shadeBuffers[0]);
        writeImage(shadeWrites[5],shadeSets_[stateVariant][frame],5,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,&lightingStorage);
        writeBuffer(shadeWrites[6],shadeSets_[stateVariant][frame],6,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,&shadeBuffers[1]);
        writeBuffer(shadeWrites[7],shadeSets_[stateVariant][frame],7,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&shadeBuffers[2]);
        writeBuffer(shadeWrites[8],shadeSets_[stateVariant][frame],8,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&shadeBuffers[3]);
        writeBuffer(shadeWrites[9],shadeSets_[stateVariant][frame],9,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&shadeBuffers[4]);
        writeBuffer(shadeWrites[10],shadeSets_[stateVariant][frame],10,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&shadeBuffers[5]);
        const VkDescriptorImageInfo sssrDepthInfo{sssrDepth_.sampler(),sssrDepth_.fullView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writeImage(shadeWrites[11],shadeSets_[stateVariant][frame],11,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&sssrDepthInfo);
        const VkDescriptorBufferInfo shadeBudget=bufferInfo(frameBudgetBuffer_[frame].handle(),sizeof(GPUWaterFrameBudget));
        writeBuffer(shadeWrites[12],shadeSets_[stateVariant][frame],12,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&shadeBudget);
        writeImage(shadeWrites[13],shadeSets_[stateVariant][frame],13,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&previousLighting);
        writeImage(shadeWrites[14],shadeSets_[stateVariant][frame],14,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&waterVelocity);
        vkUpdateDescriptorSets(device_,static_cast<std::uint32_t>(shadeWrites.size()),shadeWrites.data(),0,nullptr);

        const VkDescriptorBufferInfo farBuffer=bufferInfo(farOceanConfig_.handle(),sizeof(GPUFarOceanConfig));VkWriteDescriptorSet farWrite{};
        writeBuffer(farWrite,farSets_[frame],0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,&farBuffer);vkUpdateDescriptorSets(device_,1,&farWrite,0,nullptr);

        const VkDescriptorBufferInfo authoredBuffers[]{bufferInfo(authoredWaterConfig_.handle(),sizeof(GPUAuthoredWaterConfig)),bufferInfo(pages_.handle()),bufferInfo(statePageTable_.handle()),bufferInfo(statePhysical_.handle()),bufferInfo(stateWrite)};
        std::array<VkWriteDescriptorSet,5> authoredWrites{};
        writeBuffer(authoredWrites[0],authoredSets_[stateVariant][frame],0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,&authoredBuffers[0]);
        for(std::uint32_t i=1;i<5;++i)writeBuffer(authoredWrites[i],authoredSets_[stateVariant][frame],i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,&authoredBuffers[i]);
        vkUpdateDescriptorSets(device_,5,authoredWrites.data(),0,nullptr);
        }

        const VkDescriptorImageInfo compositeImages[]{opaqueColor_,{lighting_[frame].sampler(),lighting_[frame].imageView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{meta_.sampler(),meta_.imageView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},opaqueDepth_,{surface_.sampler(),surface_.imageView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        const VkDescriptorBufferInfo underwaterBuffer=bufferInfo(underwaterConfig_[frame].handle(),sizeof(GPUWaterUnderwaterConfig));
        std::array<VkWriteDescriptorSet,6> compositeWrites{};
        for(std::uint32_t i=0;i<4;++i)writeImage(compositeWrites[i],compositeSets_[frame],i,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&compositeImages[i]);
        writeBuffer(compositeWrites[4],compositeSets_[frame],4,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,&underwaterBuffer);
        writeImage(compositeWrites[5],compositeSets_[frame],5,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&compositeImages[4]);
        vkUpdateDescriptorSets(device_,6,compositeWrites.data(),0,nullptr);
    }
}

void VirtualWaterRenderer::updateFrameBindings(std::span<const VkBuffer> instances,std::span<const VkBuffer> culling,std::span<const VkDescriptorImageInfo> hiz){
    if(instances.size()<FramesInFlight||culling.size()<FramesInFlight||hiz.size()<FramesInFlight)return;std::copy_n(instances.begin(),FramesInFlight,instanceBuffers_.begin());std::copy_n(culling.begin(),FramesInFlight,cullingUniformBuffers_.begin());std::copy_n(hiz.begin(),FramesInFlight,previousHiZ_.begin());if(device_)writeDescriptors();
}

void VirtualWaterRenderer::rebuild(const WaterRenderWorld& world) {
    if (!device_) return;
    temporalHistoryValid_=false;

    std::vector<GPUVirtualWaterPage> pages;
    std::vector<GPUWaterDrawTemplate> templates;
    GPUFarOceanConfig farConfig{};
    GPUAuthoredWaterConfig authoredConfig{};
    bodyCount_=0U;
    authoredWaterActive_=false;
    if (world.generation() != waterWorldGeneration_) {
        if (!snapshotResources_.empty()) {
            retiredSnapshots_.push_back({
                .retireAfterCompletedFrame = completedFrameSerial_ + FramesInFlight,
                .resources = std::move(snapshotResources_),
            });
        }
        snapshotResources_.clear();
        snapshotResources_.reserve(world.bodies().size());
        for (const WaterRenderBody& body : world.bodies())
            if (body.meshResource) snapshotResources_.push_back(body.meshResource);
        waterWorldGeneration_ = world.generation();
    }

    const auto executionFlagsFor=[](const WaterBodyComponent& water, WaterGeometryMode resolvedGeometry){
        std::uint32_t flags=0U;
        WaterShadingMode shading=water.execution.shading;
        WaterStateMode state=water.execution.state;

        // A preset is an explicit authoring/benchmark request.  It overrides the
        // independent fields; Auto leaves those fields fully independent.
        switch(water.execution.tier){
        case WaterExecutionTier::Simple:
            shading=WaterShadingMode::Simple;
            state=WaterStateMode::None;
            break;
        case WaterExecutionTier::VirtualLite:
            shading=WaterShadingMode::Budgeted;
            state=WaterStateMode::None;
            break;
        case WaterExecutionTier::FullVirtual:
            shading=WaterShadingMode::Budgeted;
            state=WaterStateMode::Sparse;
            break;
        case WaterExecutionTier::Auto:
            break;
        }

        if(shading==WaterShadingMode::Auto){
            const bool virtualGeometry=resolvedGeometry==WaterGeometryMode::VirtualClipmap ||
                                       resolvedGeometry==WaterGeometryMode::SplinePages;
            shading=virtualGeometry?WaterShadingMode::Budgeted:WaterShadingMode::Simple;
        }
        if(shading==WaterShadingMode::Budgeted) flags|=ExecutionBudgetedShading;
        else flags|=ExecutionSimpleShading;
        if(state==WaterStateMode::Sparse) flags|=ExecutionSparseState;
        return flags;
    };
    const auto resolveGeometry=[](const WaterRenderBody& body){
        const WaterBodyComponent& water=body.water;

        // Explicit per-axis policy wins in Auto-tier mode. Forced presets are
        // deterministic benchmark presets, but still choose a representation
        // that can actually express the body (an unbounded ocean cannot become
        // an AuthoredMesh merely to make the preset cheaper).
        if(water.execution.tier==WaterExecutionTier::Auto &&
           water.execution.geometry!=WaterGeometryMode::Auto)
            return water.execution.geometry;

        if(water.execution.tier==WaterExecutionTier::Simple){
            const bool unbounded=body.shape.lakePolygon.size()<3U &&
                                 body.shape.riverSpline.size()<2U;
            return unbounded?WaterGeometryMode::VirtualClipmap:WaterGeometryMode::AuthoredMesh;
        }
        if(water.execution.tier==WaterExecutionTier::VirtualLite){
            if(body.shape.riverSpline.size()>24U) return WaterGeometryMode::SplinePages;
            const bool unbounded=body.shape.lakePolygon.size()<3U &&
                                 body.shape.riverSpline.size()<2U;
            return unbounded?WaterGeometryMode::VirtualClipmap:WaterGeometryMode::AuthoredMesh;
        }
        if(water.execution.tier==WaterExecutionTier::FullVirtual){
            if(body.shape.riverSpline.size()>=2U) return WaterGeometryMode::SplinePages;
            return WaterGeometryMode::VirtualClipmap;
        }

        // Auto is only a heuristic.  Body type may inform that heuristic, but it
        // can never override an explicit policy/preset.
        if(body.shape.riverSpline.size()>24U) return WaterGeometryMode::SplinePages;
        if(water.type==WaterBodyType::Ocean) return WaterGeometryMode::VirtualClipmap;
        return WaterGeometryMode::AuthoredMesh;
    };
    const auto appendTemplates=[&](const std::uint32_t virtualBodyIndex){
        if(oceanDrawRanges_.size()!=StitchVariantCount) return;
        for(std::uint32_t mask=0;mask<StitchVariantCount;++mask){
            const auto& range=oceanDrawRanges_[mask];
            templates.push_back({range.indexCount,range.firstIndex,0,
                virtualBodyIndex*StitchVariantCount*VisiblePagesPerBin+mask*VisiblePagesPerBin});
        }
    };

    std::uint32_t virtualBodyIndex=0U;
    for(const WaterRenderBody& body:world.bodies()){
        if(virtualBodyIndex>=MaxVirtualWaterBodies) break;
        const WaterBodyComponent& water=body.water;
        const WaterGeometryMode geometry=resolveGeometry(body);
        const std::uint32_t executionFlags=executionFlagsFor(water,geometry);
        bool ownsVirtualGeometry=false;

        if(geometry==WaterGeometryMode::VirtualClipmap &&
           body.shape.lakePolygon.size()<3U && body.shape.riverSpline.size()<2U){
            OceanDomainConfig domain{};
            domain.waves=water.waves;domain.waveCount=water.waveCount;domain.bodyIndex=virtualBodyIndex;
            domain.bodyId=body.id;domain.spectrumRevision=body.spectrumRevision;domain.instanceIndex=body.instanceIndex;
            domain.pageBaseIndex=static_cast<std::uint32_t>(pages.size());domain.geometryLevelCount=GeometryClipLevels;
            domain.executionFlags=executionFlags;
            auto bodyPages=buildOceanPageDomain(domain);
            if(!bodyPages.empty()&&pages.size()+bodyPages.size()<=MaxPages){
                pages.insert(pages.end(),bodyPages.begin(),bodyPages.end());appendTemplates(virtualBodyIndex);ownsVirtualGeometry=true;
                if(farConfig.bodyCount<MaxVirtualWaterBodies){
                    GPUFarOceanBody& far=farConfig.bodies[farConfig.bodyCount++];far.instanceIndex=domain.instanceIndex;
                    far.startDistance=OceanExtents[GeometryClipLevels-1U]*0.88F;
                    far.normalCellSize=(2.0F*OceanExtents[GeometryClipLevels-1U])/static_cast<float>(ClipmapResolution);
                    far.waveMask=(water.waveCount>=8U?0xffU:((1U<<water.waveCount)-1U)) | (executionFlags<<8U);
                }
            }
        } else if(geometry==WaterGeometryMode::VirtualClipmap && body.shape.lakePolygon.size()>=3U){
            FiniteWaterDomainConfig domain{};
            domain.boundary=body.shape.lakePolygon;domain.waves=water.waves;domain.waveCount=water.waveCount;
            domain.bodyIndex=virtualBodyIndex;domain.bodyId=body.id;domain.spectrumRevision=body.spectrumRevision;
            domain.instanceIndex=body.instanceIndex;domain.pageBaseIndex=static_cast<std::uint32_t>(pages.size());
            domain.executionFlags=executionFlags;domain.targetPageWorldSize=32.0F;
            auto bodyPages=buildFiniteWaterPageDomain(domain);
            if(!bodyPages.empty()&&pages.size()+bodyPages.size()<=MaxPages){
                pages.insert(pages.end(),bodyPages.begin(),bodyPages.end());appendTemplates(virtualBodyIndex);ownsVirtualGeometry=true;
            }
        } else if(geometry==WaterGeometryMode::SplinePages && body.shape.riverSpline.size()>=2U){
            SplineDomainConfig domain{};
            domain.points=body.shape.riverSpline;domain.waves=water.waves;domain.waveCount=water.waveCount;
            domain.bodyIndex=virtualBodyIndex;domain.bodyId=body.id;domain.spectrumRevision=body.spectrumRevision;
            domain.instanceIndex=body.instanceIndex;domain.pageBaseIndex=static_cast<std::uint32_t>(pages.size());domain.executionFlags=executionFlags;
            auto bodyPages=buildSplinePageDomain(domain);
            if(!bodyPages.empty()&&pages.size()+bodyPages.size()<=MaxPages){
                pages.insert(pages.end(),bodyPages.begin(),bodyPages.end());appendTemplates(virtualBodyIndex);ownsVirtualGeometry=true;
            }
        }
        if(ownsVirtualGeometry){++virtualBodyIndex;continue;}

        // Authored geometry shares exactly the same prepass/composite contract. A
        // single world-stable state page is attached only when sparse state is requested.
        if(authoredConfig.bodyCount>=MaxVirtualWaterBodies) continue;
        authoredWaterActive_=true;
        GPUAuthoredWaterBody& authored=authoredConfig.bodies[authoredConfig.bodyCount++];
        authored.instanceIndex=body.instanceIndex;
        authored.bodyType=static_cast<std::uint32_t>(water.type);
        authored.executionFlags=executionFlags;
        authored.statePageIndex=FarAnalyticPageId;
        if((executionFlags&ExecutionSparseState)==0U || pages.size()>=MaxPages) continue;

        float minX=std::numeric_limits<float>::max(),minZ=minX;
        float maxX=std::numeric_limits<float>::lowest(),maxZ=maxX;
        float heightSum=0.0F;std::uint32_t pointCount=0U;
        float riverFlowX=0.0F,riverFlowZ=0.0F,riverFlowWeight=0.0F;
        if(body.shape.lakePolygon.size()>=3U){
            for(const Vec3& point:body.shape.lakePolygon){minX=std::min(minX,point.x());minZ=std::min(minZ,point.z());maxX=std::max(maxX,point.x());maxZ=std::max(maxZ,point.z());heightSum+=point.y();++pointCount;}
        } else if(body.shape.riverSpline.size()>=2U){
            for(const RiverSplinePoint& point:body.shape.riverSpline){const float hw=std::max(point.width*0.5F,0.1F);minX=std::min(minX,point.position.x()-hw);minZ=std::min(minZ,point.position.z()-hw);maxX=std::max(maxX,point.position.x()+hw);maxZ=std::max(maxZ,point.position.z()+hw);heightSum+=point.position.y();++pointCount;}
            for(std::size_t i=0;i+1U<body.shape.riverSpline.size();++i){const auto& a=body.shape.riverSpline[i];const auto& b=body.shape.riverSpline[i+1U];const float dx=b.position.x()-a.position.x(),dz=b.position.z()-a.position.z();const float len=std::sqrt(dx*dx+dz*dz);if(len<=1e-4F)continue;const float speed=std::max(0.0F,0.5F*(a.flowSpeed+b.flowSpeed));riverFlowX+=(dx/len)*speed*len;riverFlowZ+=(dz/len)*speed*len;riverFlowWeight+=len;}
        } else {
            // Ocean cannot use the authored reusable-page mesh as a finite body;
            // resolve forced AuthoredMesh back to the virtual clipmap contract.
            continue;
        }
        if(pointCount==0U) continue;
        GPUVirtualWaterPage statePage{};
        statePage.originX=statePage.minX=minX;statePage.originZ=statePage.minZ=minZ;statePage.maxX=maxX;statePage.maxZ=maxZ;
        statePage.baseHeight=heightSum/static_cast<float>(pointCount);
        const float maxSpan=std::max(std::max(maxX-minX,maxZ-minZ),1.0F);
        statePage.cellSize=maxSpan/static_cast<float>(PageCells);statePage.stateWorldSizeClass=std::bit_cast<std::uint32_t>(maxSpan);
        statePage.verticalBound=MaxInteractionDisplacement;statePage.waveCandidateMask=water.waveCount>=8U?0xffU:((1U<<water.waveCount)-1U);
        statePage.instanceIndex=authored.instanceIndex;statePage.bodyIndex=authoredConfig.bodyCount-1U;
        statePage.bodyIdIndex=body.id.index;statePage.bodyIdGeneration=body.id.generation;statePage.spectrumRevision=body.spectrumRevision;
        statePage.localPageX=0;statePage.localPageZ=0;statePage.coverageClass=static_cast<std::uint32_t>(WaterCoverageClass::Wet);
        statePage.flags=PageStateOnly|PageSparseState;statePage.executionFlags=executionFlags;
        if(riverFlowWeight>0.0F){const float ax=riverFlowX/riverFlowWeight,az=riverFlowZ/riverFlowWeight;const float speed=std::sqrt(ax*ax+az*az);if(speed>1e-4F){statePage.flowX=ax/speed;statePage.flowZ=az/speed;statePage.flowSpeed=speed;}}
        authored.statePageIndex=static_cast<std::uint32_t>(pages.size());pages.push_back(statePage);
    }

    bodyCount_=virtualBodyIndex;
    pageCount_=static_cast<std::uint32_t>(pages.size());
    drawBinCount_=bodyCount_*StitchVariantCount;
    active_=bodyCount_!=0U||authoredWaterActive_;
    denseTileClassification_=authoredWaterActive_ || farConfig.bodyCount!=0U;
    if(!pages.empty())pages_.update(pages.data(),sizeof(GPUVirtualWaterPage)*pages.size());
    if(!templates.empty())drawTemplates_.update(templates.data(),sizeof(GPUWaterDrawTemplate)*templates.size());
    farOceanConfig_.update(&farConfig,sizeof(farConfig));authoredWaterConfig_.update(&authoredConfig,sizeof(authoredConfig));

    GPUWaterCullConfig config{};config.pageCount=pageCount_;config.drawBinCount=drawBinCount_;
    config.enableHiZ=hiZEnabled_&&previousHiZ_[0].imageView!=VK_NULL_HANDLE?1U:0U;config.qualityScale=frameBudget_.qualityScale;
    config.currentFrame=waterFrameCounter_;config.maxHighGeometryPages=frameBudget_.maxHighGeometryPages;
    for (auto& buffer : cullConfig_) buffer.update(&config,sizeof(config));

    // Geometry slots may reorder on scene edits; invalidate only their mapping.
    // Physical state/cells remain resident and are reconnected by WaterStatePageKey
    // (body generation + world state coordinate) on the next allocation pass.
    std::vector<GPUWaterStatePhysicalRef> invalidRefs(MaxPages);for(auto& ref:invalidRefs)ref.slot=InvalidPhysicalPage;
    statePageTable_.update(invalidRefs.data(),sizeof(GPUWaterStatePhysicalRef)*invalidRefs.size());
    const std::vector<GPUWaterPageHistory> zeroHistory(MaxPages);for(auto& history:history_)history.update(zeroHistory.data(),sizeof(GPUWaterPageHistory)*zeroHistory.size());
    pendingInteractions_.clear();

    if(!active_){
        const std::vector<GPUVirtualWaterPage> emptyPages(MaxPages);const std::vector<GPUWaterDrawTemplate> emptyTemplates(MaxDrawBins);
        const std::vector<GPUVisibleWaterPage> emptyVisible(MaxVisiblePageSlots);const std::vector<std::uint32_t> emptyCounts(MaxDrawBins);
        const std::vector<VkDrawIndexedIndirectCommand> emptyCommands(MaxDrawBins);const std::vector<std::uint32_t> emptyTileLists(4ULL*tileSettings_.maxTiles);
        const std::vector<std::uint32_t> emptyCandidates(tileSettings_.maxTiles);const std::array<std::uint32_t,4> emptyTileCounts{};const std::array<VkDispatchIndirectCommand,4> emptyDispatch{};const GPUWaterPageStats emptyStats{};
        pages_.update(emptyPages.data(),sizeof(GPUVirtualWaterPage)*emptyPages.size());drawTemplates_.update(emptyTemplates.data(),sizeof(GPUWaterDrawTemplate)*emptyTemplates.size());
        for(std::uint32_t f=0;f<FramesInFlight;++f){visiblePages_[f].update(emptyVisible.data(),sizeof(GPUVisibleWaterPage)*emptyVisible.size());drawBinCounts_[f].update(emptyCounts.data(),sizeof(std::uint32_t)*emptyCounts.size());indirectCommands_[f].update(emptyCommands.data(),sizeof(VkDrawIndexedIndirectCommand)*emptyCommands.size());stats_[f].update(&emptyStats,sizeof(emptyStats));tileLists_[f].update(emptyTileLists.data(),sizeof(std::uint32_t)*emptyTileLists.size());tileCounts_[f].update(emptyTileCounts.data(),sizeof(emptyTileCounts));tileDispatch_[f].update(emptyDispatch.data(),sizeof(emptyDispatch));candidateTiles_[f].update(emptyCandidates.data(),sizeof(std::uint32_t)*emptyCandidates.size());}
    }
}

void VirtualWaterRenderer::recordCull(VkCommandBuffer cmd, std::uint32_t frame, VkDescriptorSet sceneSet) {
    if (!active() || pageCount_ == 0U) return;
    frame %= FramesInFlight;
    ++waterFrameCounter_;
    GPUWaterCullConfig config{};
    config.pageCount=pageCount_;config.drawBinCount=drawBinCount_;
    config.enableHiZ=hiZEnabled_&&previousHiZ_[frame].imageView!=VK_NULL_HANDLE?1U:0U;
    config.qualityScale=frameBudget_.qualityScale;config.currentFrame=waterFrameCounter_;
    config.maxHighGeometryPages=frameBudget_.maxHighGeometryPages;
    cullConfig_[frame].update(&config,sizeof(config));
    tileSettings_.frameIndex=waterFrameCounter_;
    tileSettings_.denseMode=denseTileClassification_?1U:0U;
    tileSettings_.highThreshold=dynamicThresholds_.high;
    tileSettings_.mediumThreshold=dynamicThresholds_.medium;
    tileSettings_.cheapThreshold=dynamicThresholds_.cheap;
    const std::uint32_t previousFrame=(frame+FramesInFlight-1U)%FramesInFlight;
    tileSettings_.historyValid=(temporalHistoryValid_&&lightingInitialized_[previousFrame])?1U:0U;
    tileSettingsBuffer_[frame].update(&tileSettings_,sizeof(tileSettings_));
    GPUWaterFrameBudget gpuBudget{};
    gpuBudget.targetGpuMs=frameBudget_.targetGpuMs;gpuBudget.measuredGpuMs=measuredWaterGpuMs_;
    gpuBudget.qualityScale=frameBudget_.qualityScale;gpuBudget.feedbackGain=0.08F;
    gpuBudget.maxFullRateSamples=tileSettings_.maxTiles*64U;
    gpuBudget.maxReflectionWork=frameBudget_.maxReflectionWork;
    gpuBudget.maxRefractionWork=frameBudget_.maxRefractionWork;
    gpuBudget.maxStateSimulationCells=frameBudget_.maxStateSimulationCells;
    frameBudgetBuffer_[frame].update(&gpuBudget,sizeof(gpuBudget));

    if (drawBinCount_ != 0U) vkCmdFillBuffer(cmd, drawBinCounts_[frame].handle(), 0, sizeof(std::uint32_t) * drawBinCount_, 0);
    vkCmdFillBuffer(cmd, stats_[frame].handle(), 0, sizeof(GPUWaterPageStats), 0);
    vkCmdFillBuffer(cmd, candidateTiles_[frame].handle(), 0, sizeof(std::uint32_t)*tileSettings_.maxTiles, 0);
    VkMemoryBarrier2 clear{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    clear.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;clear.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
    clear.dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;clear.dstAccessMask=VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dependency.memoryBarrierCount=1;dependency.pMemoryBarriers=&clear;vkCmdPipelineBarrier2(cmd,&dependency);

    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,cullPipeline_);
    const std::array cullDescriptorSets{sceneSet,cullSets_[frame]};
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,cullPipelineLayout_,0,
                            static_cast<std::uint32_t>(cullDescriptorSets.size()),cullDescriptorSets.data(),0,nullptr);
    vkCmdDispatch(cmd,(pageCount_+63U)/64U,1,1);
    storageWriteBarrier(cmd,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    if(drawBinCount_!=0U){
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,buildPipeline_);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,buildPipelineLayout_,0,1,&buildSets_[frame],0,nullptr);
        vkCmdDispatch(cmd,(drawBinCount_+63U)/64U,1,1);
        storageWriteBarrier(cmd,VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
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

    const std::uint32_t stateVariant = stateCurrentScratch_ ? 1U : 0U;

    const StatePush push{
        .eventCount = eventCount,
        .pageCount = pageCount_,
        .frameIndex = frameIndex,
        .pad = 0U,
        .deltaTime = std::clamp(deltaTime, 0.0F, 0.1F),
    };
    const std::array descriptorSets{sceneSet, stateSets_[stateVariant][frameIndex]};
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
    VkRenderingAttachmentInfo surface{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    surface.imageView = surface_.imageView(); surface.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    surface.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; surface.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo meta = surface; meta.imageView = meta_.imageView();
    VkRenderingAttachmentInfo velocity = surface; velocity.imageView = velocity_.imageView();
    std::array colors{surface, meta, velocity};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = depthView_; depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = extent_; rendering.layerCount = 1;
    rendering.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
    rendering.pColorAttachments = colors.data(); rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &rendering);

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
        const std::array sets{sceneSet, drawSets_[stateCurrentScratch_ ? 1U : 0U][frame]};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, prepassPipeline_.layout(),
                                0, static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
        VkDeviceSize offset = 0;
        const VkBuffer oceanVertexBuffer = oceanVertexBuffer_.handle();
        vkCmdBindVertexBuffers(cmd, 0, 1, &oceanVertexBuffer, &offset);
        vkCmdBindIndexBuffer(cmd, oceanIndexBuffer_.handle(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexedIndirect(cmd, indirectCommands_[frame].handle(), 0, drawBinCount_,
                                 sizeof(VkDrawIndexedIndirectCommand));
    }

    // Lakes and rivers keep their authored mesh topology, but only rasterize a cheap
    // prepass here; their expensive optics now run through the same tile scheduler.
    if (authoredWaterActive_ && authoredPrepassPipeline_.handle() != VK_NULL_HANDLE && authoredWaterDraw.valid()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, authoredPrepassPipeline_.handle());
        const std::array authoredDescriptorSets{sceneSet, authoredSets_[stateCurrentScratch_ ? 1U : 0U][frame]};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, authoredPrepassPipeline_.layout(),
                                0, static_cast<std::uint32_t>(authoredDescriptorSets.size()),
                                authoredDescriptorSets.data(), 0, nullptr);
        VkDeviceSize offset = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
        vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
        authoredWaterDraw.record(cmd, authoredCommandOffset, authoredCountOffset);
    }
    vkCmdEndRendering(cmd);

    VkMemoryBarrier2 ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    ready.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    ready.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    ready.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
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
    sssrDepthPass_.record(cmd, sssrDepth_, sceneSet);
    sssrDepthInitialized_ = true;

    vkCmdFillBuffer(cmd, tileCounts_[frame].handle(), 0, sizeof(std::uint32_t) * 4, 0);
    vkCmdFillBuffer(cmd, importanceHistogram_[frame].handle(), 0, sizeof(std::uint32_t) * 256U, 0);
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

    const std::uint32_t previousFrame=(frame+FramesInFlight-1U)%FramesInFlight;
    if(!lightingInitialized_[previousFrame]) {
        imageBarrier(cmd, lighting_[previousFrame].image(), VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        lightingInitialized_[previousFrame]=true;
        tileSettings_.historyValid=0U;
        tileSettingsBuffer_[frame].update(&tileSettings_,sizeof(tileSettings_));
    }
    imageBarrier(cmd, lighting_[frame].image(),
                 lightingInitialized_[frame] ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_GENERAL,
                 lightingInitialized_[frame] ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                 lightingInitialized_[frame] ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    lightingInitialized_[frame] = true;
    // Invalidate pixels not touched by this frame's sparse dispatch.  Without
    // this clear a re-used frame slot could expose three-frame-old lighting to
    // the temporal fallback after a disocclusion. HdrBuffer carries TRANSFER_DST.
    const VkClearColorValue clearLighting{{0.0F,0.0F,0.0F,0.0F}};
    const VkImageSubresourceRange lightingRange{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(cmd,lighting_[frame].image(),VK_IMAGE_LAYOUT_GENERAL,&clearLighting,1,&lightingRange);
    VkMemoryBarrier2 clearLightingBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    clearLightingBarrier.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    clearLightingBarrier.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
    clearLightingBarrier.dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    clearLightingBarrier.dstAccessMask=VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo clearLightingDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    clearLightingDependency.memoryBarrierCount=1;
    clearLightingDependency.pMemoryBarriers=&clearLightingBarrier;
    vkCmdPipelineBarrier2(cmd,&clearLightingDependency);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shadePipeline_);
    const std::array sets{sceneSet, shadeSets_[stateCurrentScratch_ ? 1U : 0U][frame]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shadePipelineLayout_,
                            0, static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
    for (std::uint32_t tier = 0; tier < 4; ++tier) {
        ShadePush push{tier};
        vkCmdPushConstants(cmd, shadePipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatchIndirect(cmd, tileDispatch_[frame].handle(), tier * sizeof(VkDispatchIndirectCommand));
    }
    imageBarrier(cmd, lighting_[frame].image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    temporalHistoryValid_ = true;
}


void VirtualWaterRenderer::updateFrameBudget(const float measuredGpuMs) noexcept {
    if (!std::isfinite(measuredGpuMs) || measuredGpuMs < 0.0F) return;
    measuredGpuMsEma_ = measuredGpuMsEma_ <= 0.0F
        ? measuredGpuMs
        : std::lerp(measuredGpuMsEma_, measuredGpuMs, 0.12F);
    measuredWaterGpuMs_ = measuredGpuMsEma_;

    const float target = std::max(frameBudget_.targetGpuMs, 0.05F);
    const float error = (target - measuredGpuMsEma_) / target;
    const float requested = frameBudget_.qualityScale * (1.0F + std::clamp(error, -1.0F, 1.0F) * 0.08F);
    // Slow recovery and bounded degradation prevent visible quality pumping.
    const float alpha = requested < frameBudget_.qualityScale ? 0.22F : 0.06F;
    frameBudget_.qualityScale = std::clamp(
        std::lerp(frameBudget_.qualityScale, requested, alpha), 0.35F, 1.10F);

}

void VirtualWaterRenderer::onFrameCompleted() noexcept {
    ++completedFrameSerial_;
    while (!retiredSnapshots_.empty() &&
           retiredSnapshots_.front().retireAfterCompletedFrame <= completedFrameSerial_) {
        retiredSnapshots_.pop_front();
    }
}

void VirtualWaterRenderer::onFrameCompleted(const std::uint32_t frameIndex,
                                             const float measuredWaterGpuMs) {
    onFrameCompleted();
    updateFrameBudget(measuredWaterGpuMs);

    // The caller waits the frame-slot fence before entering this function, so
    // reading the host-coherent histogram cannot race the GPU.  Derive the
    // next frame's quality bands from what was actually visible rather than
    // from fixed score constants.  Timing pressure shifts the target
    // percentiles upward, preferentially dropping expensive quality tiers.
    const std::uint32_t frame = frameIndex % FramesInFlight;
    if (importanceHistogram_[frame].handle() == VK_NULL_HANDLE) return;

    std::array<std::uint32_t, 256> histogram{};
    importanceHistogram_[frame].read(histogram.data(), sizeof(histogram));
    std::uint64_t total = 0;
    for (const std::uint32_t count : histogram) total += count;
    if (total == 0U) {
        // No water tiles were classified in the completed frame.  Keep the
        // controller stable but still reflect sustained timing pressure.
        const float pressure = std::clamp(1.0F - frameBudget_.qualityScale, 0.0F, 0.65F);
        dynamicThresholds_.high = static_cast<std::uint32_t>(std::clamp(192.0F + pressure * 48.0F, 144.0F, 240.0F));
        dynamicThresholds_.medium = static_cast<std::uint32_t>(std::clamp(112.0F + pressure * 56.0F, 80.0F, 208.0F));
        dynamicThresholds_.cheap = static_cast<std::uint32_t>(std::clamp(40.0F + pressure * 48.0F, 24.0F, 128.0F));
        return;
    }

    const float pressure = std::clamp(1.0F - frameBudget_.qualityScale, 0.0F, 0.65F);
    const auto quantile = [&](const float q) -> std::uint32_t {
        const std::uint64_t target = static_cast<std::uint64_t>(
            std::ceil(std::clamp(q, 0.0F, 1.0F) * static_cast<float>(total)));
        std::uint64_t accumulated = 0;
        for (std::uint32_t score = 0; score < histogram.size(); ++score) {
            accumulated += histogram[score];
            if (accumulated >= std::max<std::uint64_t>(target, 1U)) return score;
        }
        return 255U;
    };

    WaterDynamicThresholds histogramThresholds{};
    histogramThresholds.cheap = quantile(0.25F + 0.18F * pressure);
    histogramThresholds.medium = quantile(0.58F + 0.18F * pressure);
    histogramThresholds.high = quantile(0.82F + 0.12F * pressure);
    // Preserve strict bands even for a nearly-flat histogram.
    histogramThresholds.medium = std::max(histogramThresholds.medium,
                                          std::min(histogramThresholds.cheap + 1U, 255U));
    histogramThresholds.high = std::max(histogramThresholds.high,
                                        std::min(histogramThresholds.medium + 1U, 255U));

    constexpr float kThresholdSmoothing = 0.22F;
    const auto smooth = [kThresholdSmoothing](const std::uint32_t current, const std::uint32_t target) {
        return static_cast<std::uint32_t>(std::lround(std::lerp(
            static_cast<float>(current), static_cast<float>(target), kThresholdSmoothing)));
    };
    dynamicThresholds_.cheap = smooth(dynamicThresholds_.cheap, histogramThresholds.cheap);
    dynamicThresholds_.medium = smooth(dynamicThresholds_.medium, histogramThresholds.medium);
    dynamicThresholds_.high = smooth(dynamicThresholds_.high, histogramThresholds.high);
}

void VirtualWaterRenderer::recordComposite(VkCommandBuffer cmd, std::uint32_t frame,
                                                  VkDescriptorSet sceneSet) const {
    if (!active()) return;
    frame %= FramesInFlight;
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = hdrTargetView_; color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = extent_; rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1; rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
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
    vkCmdEndRendering(cmd);
}

void VirtualWaterRenderer::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
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
    for (auto& lighting : lighting_) lighting.destroy();
    sssrDepth_.destroy();
    pages_.destroy();
    drawTemplates_.destroy();
    oceanVertexBuffer_.destroy();
    oceanIndexBuffer_.destroy();
    oceanDrawRanges_.clear();
    for (auto& buffer : cullConfig_) buffer.destroy();
    farOceanConfig_.destroy();
    authoredWaterConfig_.destroy();
    for (auto& buffer : tileSettingsBuffer_) buffer.destroy();
    for (auto& buffer : frameBudgetBuffer_) buffer.destroy();
    for (auto& buffer : importanceHistogram_) buffer.destroy();
    for (auto& buffer : candidateTiles_) buffer.destroy();
    statePageTable_.destroy();
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
    lightingInitialized_.fill(false);
    temporalHistoryValid_=false;
    stateCurrentScratch_ = false;
    sssrDepthInitialized_ = false;
    pendingInteractions_.clear();
    snapshotResources_.clear();
    retiredSnapshots_.clear();
    completedFrameSerial_=0;
    waterWorldGeneration_=0;
    stateInitialized_=false;
    measuredGpuMsEma_=0.0F;
    denseTileClassification_=true;

    cullLayout_ = buildLayout_ = drawLayout_ = classifyLayout_ = buildDispatchLayout_ =
        shadeLayout_ = compositeLayout_ = stateLayout_ = farLayout_ = authoredLayout_ = sssrInitLayout_ = sssrReduceLayout_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    cullPipelineLayout_ = buildPipelineLayout_ = classifyPipelineLayout_ = buildDispatchPipelineLayout_ =
        shadePipelineLayout_ = statePipelineLayout_ = sssrInitPipelineLayout_ = sssrReducePipelineLayout_ = VK_NULL_HANDLE;
    cullPipeline_ = buildPipeline_ = classifyPipeline_ = buildDispatchPipeline_ = shadePipeline_ =
        stateAllocatePipeline_ = statePipeline_ = sssrInitPipeline_ = sssrReducePipeline_ = VK_NULL_HANDLE;
    hdrTargetView_ = VK_NULL_HANDLE;

    cullSets_.fill(VK_NULL_HANDLE);
    buildSets_.fill(VK_NULL_HANDLE);
    for (auto& sets : drawSets_) sets.fill(VK_NULL_HANDLE);
    classifySets_.fill(VK_NULL_HANDLE);
    buildDispatchSets_.fill(VK_NULL_HANDLE);
    for (auto& sets : shadeSets_) sets.fill(VK_NULL_HANDLE);
    compositeSets_.fill(VK_NULL_HANDLE);
    for (auto& sets : stateSets_) sets.fill(VK_NULL_HANDLE);
    farSets_.fill(VK_NULL_HANDLE);
    for (auto& sets : authoredSets_) sets.fill(VK_NULL_HANDLE);
    instanceBuffers_.fill(VK_NULL_HANDLE);
    cullingUniformBuffers_.fill(VK_NULL_HANDLE);
    previousHiZ_.fill({});
    opaqueColor_ = {};
    opaqueDepth_ = {};
}
} // namespace Engine::Water
