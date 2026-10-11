#include "Engine/Renderer/Passes/GlassPass.h"

#include "Engine/Renderer/Culling/IndexedIndirectDrawCount.h"
#include "Engine/Renderer/Geometry/GpuVertex.h"

#include <cstddef>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include "Engine/Scene/SceneView.h"
#include "Engine/Physics/GlassDestructionSystem.h"
#include "Engine/Renderer/shader_loader.h"
#include "Engine/Renderer/Vulkan/PipelineCache.h"

namespace Engine {
    void GlassPass::create(const VkDevice device, VmaAllocator allocator, const VkFormat colorFormat, const VkFormat depthFormat,
                           const VkSampleCountFlagBits samples, const VkFormat depthResolveFormat,
                           const VkResolveModeFlagBits depthResolveMode, const VkDescriptorSetLayout sceneLayout,
                           Assets::AssetManager &assets, const bool velocity,
                           const VkDescriptorImageInfo &opaqueColor, const VkDescriptorImageInfo &opaqueDepth, unsigned paneCount) {
        if (opaqueColor.imageView == VK_NULL_HANDLE || opaqueDepth.imageView == VK_NULL_HANDLE) {
            throw std::invalid_argument("GlassPass requires valid opaque scene color/depth views");
        }
        device_ = device;
        allocator_ = allocator;
        hasVelocity_ = velocity;
        GraphicsPipelineOptions options{};
        options.colorFormat = colorFormat;
        options.dynamicRendering = true;
        options.additionalColorFormat = velocity ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_UNDEFINED;
        options.additionalColorLoadOp = velocity ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        options.depthFormat = depthFormat;
        options.samples = samples;
        options.depthResolveFormat = depthResolveFormat;
        options.depthResolveMode = depthResolveMode;
        // The opaque lighting pass has already produced both attachments.
        options.colorLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        options.depthLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        options.colorInitialLayout = samples == VK_SAMPLE_COUNT_1_BIT
                                         ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                         : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        options.colorFinalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        options.depthInitialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        options.depthFinalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        options.depthWriteEnable = VK_FALSE;
        options.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        options.shader = velocity ? "shaders/forward_glass.spv" : "shaders/forward_glass_no_velocity.spv";
        options.assetManager = &assets;
        options.cullMode = VK_CULL_MODE_NONE;
        const VkDescriptorSetLayoutBinding bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        const VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            nullptr, 0, static_cast<std::uint32_t>(std::size(bindings)), bindings
        };
        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &sceneTextureLayout_) != VK_SUCCESS)
            throw std::runtime_error("Could not create glass scene descriptor layout");
        options.descriptorSetLayouts = {sceneLayout, sceneTextureLayout_};
        options.vertexBindings = {{0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX}};
        options.vertexAttributes = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, px)},
            {1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(GpuVertex, color)},
            {2, 0, VK_FORMAT_R16G16_SFLOAT, offsetof(GpuVertex, texCoord)},
            // forward_glass's vertex entry point consumes the complete GpuVertex
            // layout.  Every declared input needs an attribute on devices without
            // vertex attribute robustness/maintenance9, even when a variant does
            // not visibly use a value in its final fragment output.
            {4, 0, VK_FORMAT_R16G16_SFLOAT, offsetof(GpuVertex, texCoord1)},
            {3, 0, VK_FORMAT_A2B10G10R10_SNORM_PACK32, offsetof(GpuVertex, normal)},
            {8, 0, VK_FORMAT_R32_UINT, offsetof(GpuVertex, materialIndex)},
            {9, 0, VK_FORMAT_A2B10G10R10_SNORM_PACK32, offsetof(GpuVertex, tangent)},
        };
        pipeline_.create(device, options);
        samples_ = samples;
        depthResolveMode_ = depthResolveMode;
        createCrackResources(assets, paneCount);
        createSceneDescriptors(opaqueColor, opaqueDepth);
    }

    void GlassPass::createSceneDescriptors(const VkDescriptorImageInfo &opaqueColor,
                                           const VkDescriptorImageInfo &opaqueDepth) {
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FramesInFlight * 3U},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, FramesInFlight},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, FramesInFlight},
        };
        const VkDescriptorPoolCreateInfo poolInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0,
            FramesInFlight, 3, sizes
        };
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS)
            throw std::runtime_error("Could not create glass scene descriptor pool");
        std::array<VkDescriptorSetLayout, FramesInFlight> layouts{};
        layouts.fill(sceneTextureLayout_);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = descriptorPool_;
        allocate.descriptorSetCount = FramesInFlight;
        allocate.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(device_, &allocate, sceneTextureSets_.data()) != VK_SUCCESS)
            throw std::runtime_error("Could not allocate glass scene descriptor sets");
        for (unsigned i = 0; i < FramesInFlight; ++i) {
            const auto set = sceneTextureSets_[i];
            const VkDescriptorImageInfo atlas{crackSampler_, crackAtlasView_, VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo storage{VK_NULL_HANDLE, crackAtlasView_, VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorBufferInfo metadata{crackMetadata_[i], 0, VK_WHOLE_SIZE};
            const VkWriteDescriptorSet writes[] = {
                {
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &opaqueColor, nullptr, nullptr
                },
                {
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &opaqueDepth, nullptr, nullptr
                },
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 2, 0, 1,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &atlas, nullptr, nullptr},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 3, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &storage, nullptr, nullptr},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 4, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &metadata, nullptr},
            };
            vkUpdateDescriptorSets(device_, std::size(writes), writes, 0, nullptr);
        }
    }

    void GlassPass::destroyCrackStorage() noexcept {
        if (device_ && crackAtlasView_) vkDestroyImageView(device_, crackAtlasView_, nullptr);
        if (allocator_ && crackAtlas_) vmaDestroyImage(allocator_, crackAtlas_, crackAtlasMemory_);
        for (unsigned i=0; i<FramesInFlight; ++i)
            if (allocator_ && crackMetadata_[i]) vmaDestroyBuffer(allocator_, crackMetadata_[i], crackMetadataMemory_[i]);
        crackAtlasView_=VK_NULL_HANDLE; crackAtlas_=VK_NULL_HANDLE; crackAtlasMemory_=nullptr;
        crackMetadata_.fill(VK_NULL_HANDLE); crackMetadataMemory_.fill(nullptr); crackMetadataMapped_.fill(nullptr);
        atlasLayers_=0; crackAtlasInitialized_=false;
    }

    void GlassPass::createCrackStorage(unsigned layers) {
        VkImage atlas{}; VmaAllocation atlasMemory{}; VkImageView atlasView{};
        std::array<VkBuffer,FramesInFlight> buffers{};
        std::array<VmaAllocation,FramesInFlight> bufferMemory{};
        std::array<void *,FramesInFlight> mapped{};
        try {
            VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            image.imageType=VK_IMAGE_TYPE_2D; image.format=VK_FORMAT_R32_SFLOAT;
            image.extent={AtlasResolution,AtlasResolution,1};
            image.mipLevels=1; image.arrayLayers=layers; image.samples=VK_SAMPLE_COUNT_1_BIT;
            image.tiling=VK_IMAGE_TILING_OPTIMAL;
            image.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo allocation{}; allocation.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            if (vmaCreateImage(allocator_,&image,&allocation,&atlas,&atlasMemory,nullptr)!=VK_SUCCESS)
                throw std::runtime_error("Could not allocate glass crack atlas");
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image=atlas; view.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY; view.format=image.format;
            view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,layers};
            if (vkCreateImageView(device_,&view,nullptr,&atlasView)!=VK_SUCCESS)
                throw std::runtime_error("Could not create glass crack atlas view");
            for (unsigned i=0; i<FramesInFlight; ++i) {
                VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                // Interleaved pane header + 128 fragment exposure masks.
                buffer.size=layers*129*sizeof(glm::vec4); buffer.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                VmaAllocationCreateInfo host{}; host.usage=VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
                host.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo details{};
                if (vmaCreateBuffer(allocator_,&buffer,&host,&buffers[i],&bufferMemory[i],&details)!=VK_SUCCESS)
                    throw std::runtime_error("Could not allocate glass crack metadata");
                mapped[i]=details.pMappedData;
                std::memset(mapped[i],0,static_cast<std::size_t>(buffer.size));
            }
        } catch (...) {
            for (unsigned i=0; i<FramesInFlight; ++i)
                if (buffers[i]) vmaDestroyBuffer(allocator_,buffers[i],bufferMemory[i]);
            if (atlasView) vkDestroyImageView(device_,atlasView,nullptr);
            if (atlas) vmaDestroyImage(allocator_,atlas,atlasMemory);
            throw;
        }
        destroyCrackStorage();
        crackAtlas_=atlas; crackAtlasMemory_=atlasMemory; crackAtlasView_=atlasView;
        crackMetadata_=buffers; crackMetadataMemory_=bufferMemory; crackMetadataMapped_=mapped;
        atlasLayers_=layers; crackSlots_={};
    }

    bool GlassPass::needsCrackAtlasResize(unsigned paneCount) const noexcept {
        return device_ && atlasLayers_!=std::clamp(paneCount,1U,AtlasSlots);
    }

    void GlassPass::resizeCrackAtlas(unsigned paneCount) {
        if (!needsCrackAtlasResize(paneCount)) return;
        createCrackStorage(std::clamp(paneCount,1U,AtlasSlots));
        for (unsigned i=0; i<FramesInFlight; ++i) {
            const VkDescriptorImageInfo atlas{crackSampler_,crackAtlasView_,VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo storage{VK_NULL_HANDLE,crackAtlasView_,VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorBufferInfo metadata{crackMetadata_[i],0,VK_WHOLE_SIZE};
            const VkWriteDescriptorSet writes[] = {
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,sceneTextureSets_[i],2,0,1,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&atlas,nullptr,nullptr},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,sceneTextureSets_[i],3,0,1,
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,&storage,nullptr,nullptr},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,sceneTextureSets_[i],4,0,1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&metadata,nullptr},
            };
            vkUpdateDescriptorSets(device_,std::size(writes),writes,0,nullptr);
        }
    }

    void GlassPass::createCrackResources(Assets::AssetManager &assets, unsigned paneCount) {
        createCrackStorage(std::clamp(paneCount,1U,AtlasSlots));
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR;
        sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(device_,&sampler,nullptr,&crackSampler_)!=VK_SUCCESS)
            throw std::runtime_error("Could not create glass crack sampler");
        // Empty set zero preserves the same set-one binding numbers as GlassThin.
        VkDescriptorSetLayout empty{};
        VkDescriptorSetLayoutCreateInfo emptyInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        if (vkCreateDescriptorSetLayout(device_, &emptyInfo, nullptr, &empty) != VK_SUCCESS)
            throw std::runtime_error("Could not create glass compute layout");
        const std::array layouts{empty, sceneTextureLayout_};
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 2; layout.pSetLayouts = layouts.data();
        layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
        const auto result = vkCreatePipelineLayout(device_, &layout, nullptr, &crackPipelineLayout_);
        vkDestroyDescriptorSetLayout(device_, empty, nullptr);
        if (result != VK_SUCCESS) throw std::runtime_error("Could not create glass crack pipeline layout");
        const auto shader = Vkutil::loadShaderModule(device_, assets, "shaders/glass_crack_stamp.spv");
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module = shader.get(); pipeline.stage.pName = "main";
        pipeline.layout = crackPipelineLayout_;
        if (vkCreateComputePipelines(device_, PipelineCache::get(device_), 1, &pipeline, nullptr, &crackPipeline_) != VK_SUCCESS)
            throw std::runtime_error("Could not create glass crack compute pipeline");
    }

    void GlassPass::recordCracks(VkCommandBuffer commandBuffer, const Scene &scene, unsigned frameIndex) {
        const unsigned frame = frameIndex % FramesInFlight;
        auto *metadata = static_cast<glm::vec4 *>(crackMetadataMapped_[frame]);
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = crackAtlasInitialized_ ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = crackAtlasInitialized_ ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : 0;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = crackAtlasInitialized_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = crackAtlas_; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, atlasLayers_};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1; dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
        if (!crackAtlasInitialized_) {
            const VkClearColorValue clear{};
            vkCmdClearColorImage(commandBuffer, crackAtlas_, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &barrier.subresourceRange);
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
            crackAtlasInitialized_ = true;
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, crackPipeline_);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, crackPipelineLayout_, 1,
            1, &sceneTextureSets_[frame], 0, nullptr);
        struct alignas(16) Stamp { glm::vec4 positionRadius, sizeWidth; glm::uvec4 tile, region; };
        static_assert(sizeof(Stamp) == 64);
        unsigned budget = 256;
        scene.view().view<GlassDestructionComponent, UUIDComponent>([&](Entity, const auto &glass, const auto &uuid) {
            const unsigned slot = glass.crackAtlasIndex;
            if (slot >= atlasLayers_) return;
            auto *paneMetadata=metadata+slot*129;
            paneMetadata[0] = {glass.size, glass.crackWidth, 0};
            auto &state = crackSlots_[slot];
            const auto historyStart = glass.impactCount - static_cast<unsigned>(glass.cracks.size());
            const bool changedOwner = state.uuid != uuid.value || state.submitted > glass.cracks.size() || state.historyStart != historyStart;
            if (changedOwner && budget == 0) { paneMetadata[0].w = 0; return; }
            if (changedOwner) state = {uuid.value, 0, 0, historyStart, historyStart != 0};
            std::vector<std::uint8_t> exposureState;
            exposureState.reserve(glass.fragments.size()+glass.bonds.size());
            for (const auto &fragment:glass.fragments) exposureState.push_back(fragment.attached);
            for (const auto &bond:glass.bonds) exposureState.push_back(bond.broken);
            if (state.geometry!=glass.fractureRenderGeometry.get() || state.exposureState!=exposureState) {
                // Pending segments were built from a previous bond graph. Replay
                // against the current graph, newest impacts first, so newly
                // exposed cracks also appear after support loss without a shot.
                state.submitted = 0;
                state.segmentOffset = 0;
                state.segments.clear();
                state.reverse = true;
                state.geometry=glass.fractureRenderGeometry.get();
                state.exposureState=std::move(exposureState);
                state.exposureMasks={};
                for (unsigned fragment=0; fragment<glass.fragments.size(); ++fragment) {
                    auto &exposed=state.exposureMasks[fragment];
                    const auto count=std::min<std::size_t>(glass.fragments[fragment].polygon.size(),128);
                    for (unsigned edge=0; edge<count; ++edge)
                        if (GlassDestructionSystem::edgeExposed(glass,fragment,edge)) exposed[edge/32]|=1U<<(edge%32);
                }
            }
            std::memcpy(&paneMetadata[1],state.exposureMasks.data(),sizeof(state.exposureMasks));
            paneMetadata[0].w = 1;
            bool reset = changedOwner;
            // Clear recycled slots even when the new pane has no cracks.
            do {
                if (!reset && state.submitted == glass.cracks.size()) break;
                if (budget == 0) break;
                const bool hasCrack = state.submitted < glass.cracks.size();
                const auto index = state.reverse ? glass.cracks.size()-1-state.submitted : state.submitted;
                const GlassCrack crack = hasCrack ? glass.cracks[index] : GlassCrack{};
                if (hasCrack && !reset && state.segments.empty())
                    state.segments=GlassDestructionSystem::crackSegments(glass,crack);
                const auto &segments=state.segments;
                if (!reset && state.segmentOffset>=segments.size()) {
                    if (hasCrack) ++state.submitted;
                    state.segmentOffset=0; state.segments.clear();
                    continue;
                }
                const auto segment=!reset && state.segmentOffset<segments.size() ?
                    segments[state.segmentOffset] : GlassCrackSegment{};
                Stamp stamp{{segment.start, segment.end}, {glass.size, glass.crackWidth, segment.intensity},
                    {slot, 0, reset ? 1U : 0U, AtlasResolution}, {0, 0, AtlasResolution, AtlasResolution}};
                if (!reset) {
                    const float padding=glass.crackWidth+std::max(glass.size.x,glass.size.y)/float(AtlasResolution);
                    const auto low = glm::clamp((glm::min(segment.start,segment.end)-glm::vec2{padding}+glass.size*0.5F)/glass.size, glm::vec2{0}, glm::vec2{1});
                    const auto high = glm::clamp((glm::max(segment.start,segment.end)+glm::vec2{padding}+glass.size*0.5F)/glass.size, glm::vec2{0}, glm::vec2{1});
                    const auto start = glm::uvec2{glm::floor(low * float(AtlasResolution))};
                    const auto end = glm::uvec2{glm::ceil(high * float(AtlasResolution))};
                    stamp.region = {start, end-start};
                }
                vkCmdPushConstants(commandBuffer, crackPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(stamp), &stamp);
                if (stamp.region.z && stamp.region.w) vkCmdDispatch(commandBuffer, (stamp.region.z+7)/8, (stamp.region.w+7)/8, 1);
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                vkCmdPipelineBarrier2(commandBuffer, &dependency);
                if (!reset && hasCrack) {
                    ++state.segmentOffset;
                    if (state.segmentOffset>=segments.size()) {++state.submitted; state.segmentOffset=0; state.segments.clear();}
                }
                --budget; reset = false;
            } while (state.submitted < glass.cracks.size());
        });
        vmaFlushAllocation(allocator_, crackMetadataMemory_[frame], 0, VK_WHOLE_SIZE);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    }

    std::vector<GpuSceneMemoryAllocation> GlassPass::memoryAllocations() const {
        std::vector<GpuSceneMemoryAllocation> result;
        if (!allocator_) return result;
        VmaAllocatorInfo allocatorInfo{}; vmaGetAllocatorInfo(allocator_, &allocatorInfo);
        VkPhysicalDeviceMemoryProperties memory{}; vkGetPhysicalDeviceMemoryProperties(allocatorInfo.physicalDevice, &memory);
        auto append = [&](const char *name, VmaAllocation allocation) {
            if (!allocation) return;
            VmaAllocationInfo info{}; vmaGetAllocationInfo(allocator_, allocation, &info);
            const auto &type = memory.memoryTypes[info.memoryType];
            result.push_back({name, type.heapIndex,
                (type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0,
                (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0,
                (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0, info.size});
        };
        append("Glass crack atlas", crackAtlasMemory_);
        for (auto allocation : crackMetadataMemory_) append("Glass crack metadata", allocation);
        return result;
    }

    void GlassPass::destroy() noexcept {
        pipeline_.destroy();
        if (device_ && crackPipeline_) vkDestroyPipeline(device_, crackPipeline_, nullptr);
        if (device_ && crackPipelineLayout_) vkDestroyPipelineLayout(device_, crackPipelineLayout_, nullptr);
        if (device_ && crackSampler_) vkDestroySampler(device_, crackSampler_, nullptr);
        destroyCrackStorage();
        crackPipeline_ = VK_NULL_HANDLE; crackPipelineLayout_ = VK_NULL_HANDLE;
        crackSampler_ = VK_NULL_HANDLE; crackAtlasView_ = VK_NULL_HANDLE; crackAtlas_ = VK_NULL_HANDLE;
        crackAtlasMemory_ = nullptr; crackMetadata_.fill(VK_NULL_HANDLE);
        crackMetadataMemory_.fill(nullptr); crackMetadataMapped_.fill(nullptr);
        crackSlots_ = {}; crackAtlasInitialized_ = false; allocator_ = nullptr;
        if (device_ != VK_NULL_HANDLE) {
            if (descriptorPool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
            if (sceneTextureLayout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(
                device_, sceneTextureLayout_, nullptr);
        }
        device_ = VK_NULL_HANDLE;
        descriptorPool_ = VK_NULL_HANDLE;
        sceneTextureLayout_ = VK_NULL_HANDLE;
        sceneTextureSets_.fill(VK_NULL_HANDLE);
        hasVelocity_ = false;
        samples_ = VK_SAMPLE_COUNT_1_BIT;
        depthResolveMode_ = VK_RESOLVE_MODE_NONE;
    }

    void GlassPass::begin(const VkCommandBuffer commandBuffer, const VkImageView colorView, const VkImageView depthView,
                          const VkImageView velocityView, const VkImageView colorResolveView,
                          const VkImageView depthResolveView, const VkExtent2D extent,
                          const VkDescriptorSet descriptorSet,
                          const std::uint32_t frameIndex, const VkBuffer vertexBuffer,
                          const VkBuffer indexBuffer) const {
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = colorView;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (samples_ != VK_SAMPLE_COUNT_1_BIT) {
            color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            color.resolveImageView = colorResolveView;
            color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        VkRenderingAttachmentInfo velocity{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        velocity.imageView = velocityView;
        velocity.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        velocity.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        velocity.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        std::array colors{color, velocity};
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depthView;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (samples_ != VK_SAMPLE_COUNT_1_BIT && depthResolveMode_ != VK_RESOLVE_MODE_NONE) {
            depth.resolveMode = depthResolveMode_;
            depth.resolveImageView = depthResolveView;
            depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        }
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = extent;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = hasVelocity_ ? 2U : 1U;
        rendering.pColorAttachments = colors.data();
        rendering.pDepthAttachment = &depth;
        vkCmdBeginRendering(commandBuffer, &rendering);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
        const std::array sets{descriptorSet, sceneTextureSets_.at(frameIndex % FramesInFlight)};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(), 0,
                                static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
        constexpr VkDeviceSize offset{};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &offset);
        vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        const VkViewport viewport{
            0.0F, 0.0F, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0F, 1.0F
        };
        const VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    }

    void GlassPass::draw(const VkCommandBuffer commandBuffer, const VkDescriptorSet descriptorSet,
                         const Culling::IndexedIndirectDrawCount &indirectDraw,
                         const VkDeviceSize commandOffset, const VkDeviceSize countOffset) const {
        if (!indirectDraw.valid()) return;
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(), 0, 1,
                                &descriptorSet, 0, nullptr);
        indirectDraw.record(commandBuffer, commandOffset, countOffset);
    }

    void GlassPass::end(const VkCommandBuffer commandBuffer) { vkCmdEndRendering(commandBuffer); }
} // namespace Engine
