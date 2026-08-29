#include "Engine/Renderer/Passes/ShadowPass.h"

#include "Engine/Math/Mat4.h"
#include "Engine/Renderer/Culling/GPUCullingPass.h"
#include "Engine/Renderer/Culling/IndexedIndirectDrawCount.h"
#include "Engine/Renderer/Geometry/Vertex.h"
#include "Engine/Renderer/Materials/MaterialBuffer.h"
#include "Engine/Renderer/shader_loader.h"
#include "Engine/Renderer/Vulkan/renderer_types.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Engine {
namespace {
constexpr float DepthBiasConstant = 0.05F;
constexpr float DepthBiasSlope = 0.10F;

bool sameMatrix(const Mat4& left, const Mat4& right) {
    return std::memcmp(&left.native(), &right.native(), sizeof(glm::mat4)) == 0;
}

bool clipPageShift(const Mat4& previous, const Mat4& current,
                   std::int32_t& shiftX, std::int32_t& shiftY) {
    const glm::mat4& oldMatrix = previous.native();
    const glm::mat4& newMatrix = current.native();
    for (glm::length_t column = 0; column < 4; ++column) {
        for (glm::length_t row = 0; row < 4; ++row) {
            if (column == 3 && (row == 0 || row == 1)) continue;
            if (std::abs(oldMatrix[column][row] - newMatrix[column][row]) > 1.0e-5F)
                return false;
        }
    }
    const glm::vec4 oldOrigin = oldMatrix * glm::vec4{0.0F, 0.0F, 0.0F, 1.0F};
    const glm::vec4 newOrigin = newMatrix * glm::vec4{0.0F, 0.0F, 0.0F, 1.0F};
    const glm::vec2 pageDelta =
        ((glm::vec2{newOrigin} / newOrigin.w) - (glm::vec2{oldOrigin} / oldOrigin.w)) *
        (0.5F * static_cast<float>(ShadowMap::VirtualPagesPerAxis));
    shiftX = static_cast<std::int32_t>(std::round(pageDelta.x));
    shiftY = static_cast<std::int32_t>(std::round(pageDelta.y));
    return std::abs(pageDelta.x - static_cast<float>(shiftX)) < 1.0e-3F &&
           std::abs(pageDelta.y - static_cast<float>(shiftY)) < 1.0e-3F;
}

glm::mat4 gpuMatrix(const Culling::GPUMat4& source) {
    glm::mat4 result{};
    std::memcpy(&result, source.data, sizeof(result));
    return result;
}
}

ShadowPass::~ShadowPass() {
    destroy();
}

void ShadowPass::create(VkPhysicalDevice physicalDevice, VkDevice device,
                        const std::vector<VkBuffer>& uniformBuffers,
                        const std::vector<VkBuffer>& materialBuffers,
                        const std::vector<VkDescriptorImageInfo>& materialTextures,
                        const VkDeviceSize uniformBufferRange,
                        const VmaAllocator allocator,
                        Assets::AssetManager& assets) {
    destroy();
    device_ = device;

    try {
        shadowMap_.create(physicalDevice, device_);
        pageTable_.fill(ShadowMap::InvalidPage);
        pagesToRender_.reserve(ShadowMap::PhysicalPageCount);

        if (materialBuffers.size() != uniformBuffers.size() ||
            materialTextures.size() != MaxMaterialTextures) {
            throw std::invalid_argument("Invalid material descriptor resources");
        }

        VkDescriptorSetLayoutBinding bindings[5]{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MaxMaterialTextures,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        const VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
            5, bindings};
        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr,
                                        &descriptorSetLayout_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create shadow descriptor-set layout");
        }

        const std::uint32_t frameCount = static_cast<std::uint32_t>(uniformBuffers.size());
        const VkDescriptorPoolSize poolSizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, frameCount},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frameCount},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frameCount},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, frameCount * MaxMaterialTextures},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frameCount},
        };
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = frameCount;
        poolInfo.poolSizeCount = std::size(poolSizes);
        poolInfo.pPoolSizes = poolSizes;
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create shadow descriptor pool");
        }

        std::vector<VkDescriptorSetLayout> layouts(frameCount, descriptorSetLayout_);
        descriptorSets_.resize(frameCount);
        VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocateInfo.descriptorPool = descriptorPool_;
        allocateInfo.descriptorSetCount = frameCount;
        allocateInfo.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(device_, &allocateInfo, descriptorSets_.data()) != VK_SUCCESS) {
            throw std::runtime_error("Could not allocate shadow descriptor sets");
        }

        pageTableBuffers_.resize(frameCount);
        for (std::unique_ptr<Buffer>& buffer : pageTableBuffers_) {
            buffer = std::make_unique<Buffer>();
            buffer->createHostVisible(physicalDevice, device_,
                                      sizeof(std::uint32_t) * pageTable_.size(),
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, allocator);
            buffer->update(pageTable_.data(), sizeof(std::uint32_t) * pageTable_.size());
        }

        const VkDescriptorImageInfo imageInfo{
            shadowMap_.sampler(), shadowMap_.imageView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
            const VkDescriptorBufferInfo bufferInfo{
                uniformBuffers[frame], 0, uniformBufferRange};
            const VkDescriptorBufferInfo materialInfo{
                materialBuffers[frame], 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo pageTableInfo{
                pageTableBuffers_[frame]->handle(), 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet writes[5]{};
            writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                         descriptorSets_[frame], 0, 0, 1,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo, nullptr, nullptr};
            writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                         descriptorSets_[frame], 1, 0, 1,
                         VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bufferInfo, nullptr};
            writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                         descriptorSets_[frame], 2, 0, 1,
                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &materialInfo, nullptr};
            writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                         descriptorSets_[frame], 3, 0, MaxMaterialTextures,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, materialTextures.data(), nullptr};
            writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                         descriptorSets_[frame], 4, 0, 1,
                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &pageTableInfo, nullptr};
            vkUpdateDescriptorSets(device_, std::size(writes), writes, 0, nullptr);
        }

        const auto shader = Vkutil::loadShaderModule(device_, assets, "shaders/shadow_map.spv");
        const std::array stages{
            VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_VERTEX_BIT, shader.get(), "vertexMain", nullptr},
            VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_FRAGMENT_BIT, shader.get(), "fragmentMain", nullptr},
        };
        const VkPushConstantRange pushConstantRange{
            VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout_;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
        if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr,
                                   &pipelineLayout_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create shadow pipeline layout");
        }

        const VkVertexInputBindingDescription vertexBindings[] = {
            {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
            {1, sizeof(RendererInstanceData), VK_VERTEX_INPUT_RATE_INSTANCE},
        };
        const VkVertexInputAttributeDescription attributes[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, texCoord)},
            {8, 0, VK_FORMAT_R32_UINT, offsetof(Vertex, materialIndex)},
            {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RendererInstanceData, positionMaterial)},
            {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RendererInstanceData, rotation)},
            {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RendererInstanceData, scaleBase)},
            {13, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RendererInstanceData, grassDeformation)},
        };
        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = std::size(vertexBindings);
        vertexInput.pVertexBindingDescriptions = vertexBindings;
        vertexInput.vertexAttributeDescriptionCount = std::size(attributes);
        vertexInput.pVertexAttributeDescriptions = attributes;
        VkPipelineInputAssemblyStateCreateInfo assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rasterizer{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        // The main pass renders glTF double-sided foliage. Its transparent
        // pixels are already discarded by shadow_map::fragmentMain, so it must also cast
        // a shadow when the light sees the back of a leaf card.
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rasterizer.lineWidth = 1.0F;
        rasterizer.depthBiasEnable = VK_TRUE;
        VkPipelineMultisampleStateCreateInfo multisampling{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;
        constexpr VkDynamicState dynamicStates[] = {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_DEPTH_BIAS};
        VkPipelineDynamicStateCreateInfo dynamic{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = std::size(dynamicStates);
        dynamic.pDynamicStates = dynamicStates;
        VkGraphicsPipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.stageCount = std::size(stages);
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &assembly;
        pipelineInfo.pViewportState = &viewport;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depth;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = pipelineLayout_;
        pipelineInfo.renderPass = shadowMap_.renderPass();
        if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo,
                                      nullptr, &pipeline_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create shadow pipeline");
        }
    } catch (...) {
        destroy();
        throw;
    }
}

void ShadowPass::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (pipelineLayout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (descriptorPool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        if (descriptorSetLayout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
    }
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    descriptorSetLayout_ = VK_NULL_HANDLE;
    descriptorSets_.clear();
    pageTableBuffers_.clear();
    shadowMap_.destroy();
    atlasInitialized_ = false;
    atlasContentValid_ = false;
    invalidateCache();
    device_ = VK_NULL_HANDLE;
}

VkDescriptorSet ShadowPass::descriptorSet(const std::uint32_t frameIndex) const {
    return descriptorSets_.at(frameIndex);
}

std::uint32_t ShadowPass::virtualPageIndex(const std::uint32_t level,
                                           const std::uint32_t x,
                                           const std::uint32_t y) noexcept {
    constexpr std::uint32_t pagesPerLevel =
        ShadowMap::VirtualPagesPerAxis * ShadowMap::VirtualPagesPerAxis;
    return level * pagesPerLevel + y * ShadowMap::VirtualPagesPerAxis + x;
}

void ShadowPass::invalidateCache() noexcept {
    pageTable_.fill(ShadowMap::InvalidPage);
    physicalPages_.fill({});
    cachedClipMatricesValid_.fill(false);
    pagesToRender_.clear();
    atlasContentValid_ = false;
}

void ShadowPass::preparePages(
    const std::array<Mat4, ShadowMap::ClipLevelCount>& clipMatrices,
    const Mat4& cameraViewProjection,
    const std::span<const Culling::GPUObjectData> objects,
    const std::span<const Culling::GPUObjectData> dirtyObjects,
    const std::uint32_t frameIndex) {
    constexpr std::int32_t pageCount =
        static_cast<std::int32_t>(ShadowMap::VirtualPagesPerAxis);
    ++cacheClock_;
    pagesToRender_.clear();

    // Dynamic transforms used to invalidate the whole virtual atlas. In play
    // mode even a single rigid body therefore redrew every cached terrain and
    // grass page on every physics tick. Mark only pages touched by the old or
    // new batch bounds. Their previous contents remain sampleable until the
    // bounded refresh below has rendered the replacement.
    constexpr glm::vec3 unitCorners[8] = {
        {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1},
    };
    const auto invalidateDirtyPages = [&](const auto& matrices,
                                          const bool requireCachedMatrix) {
        for (const Culling::GPUObjectData& object : dirtyObjects) {
            const glm::vec3 minimum{object.localAabbMin.x, object.localAabbMin.y,
                                    object.localAabbMin.z};
            const glm::vec3 maximum{object.localAabbMax.x, object.localAabbMax.y,
                                    object.localAabbMax.z};
            const glm::mat4 model = gpuMatrix(object.model);
            for (std::uint32_t level = 0; level < ShadowMap::ClipLevelCount; ++level) {
                if (requireCachedMatrix && !cachedClipMatricesValid_[level]) continue;
                glm::vec2 minimumUv{std::numeric_limits<float>::max()};
                glm::vec2 maximumUv{-std::numeric_limits<float>::max()};
                for (const glm::vec3& corner : unitCorners) {
                    const glm::vec3 local = glm::mix(minimum, maximum, corner);
                    const glm::vec4 clip = matrices[level].native() * model *
                                           glm::vec4{local, 1.0F};
                    const glm::vec2 uv = glm::vec2{clip} / clip.w * 0.5F + 0.5F;
                    minimumUv = glm::min(minimumUv, uv);
                    maximumUv = glm::max(maximumUv, uv);
                }
                if (maximumUv.x <= 0.0F || maximumUv.y <= 0.0F ||
                    minimumUv.x >= 1.0F || minimumUv.y >= 1.0F) continue;
                const std::int32_t minimumX = std::clamp(
                    static_cast<std::int32_t>(std::floor(minimumUv.x * pageCount)) - 1,
                    0, pageCount - 1);
                const std::int32_t minimumY = std::clamp(
                    static_cast<std::int32_t>(std::floor(minimumUv.y * pageCount)) - 1,
                    0, pageCount - 1);
                const std::int32_t maximumX = std::clamp(
                    static_cast<std::int32_t>(std::floor(maximumUv.x * pageCount)) + 1,
                    0, pageCount - 1);
                const std::int32_t maximumY = std::clamp(
                    static_cast<std::int32_t>(std::floor(maximumUv.y * pageCount)) + 1,
                    0, pageCount - 1);
                for (std::int32_t y = minimumY; y <= maximumY; ++y) {
                    for (std::int32_t x = minimumX; x <= maximumX; ++x) {
                        const std::uint32_t key = virtualPageIndex(
                            level, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                        const std::uint32_t physical = pageTable_[key];
                        if (physical == ShadowMap::InvalidPage) continue;
                        physicalPages_[physical].dirty = true;
                    }
                }
            }
        }
    };
    // Cached pages are still addressed in the previous clipmap coordinates.
    // Clear there first; after scrolling, clear the new coordinates as well.
    invalidateDirtyPages(cachedClipMatrices_, true);

    for (std::uint32_t level = 0; level < ShadowMap::ClipLevelCount; ++level) {
        if (cachedClipMatricesValid_[level] &&
            sameMatrix(cachedClipMatrices_[level], clipMatrices[level])) continue;
        std::int32_t shiftX{};
        std::int32_t shiftY{};
        if (cachedClipMatricesValid_[level] &&
            clipPageShift(cachedClipMatrices_[level], clipMatrices[level], shiftX, shiftY)) {
            std::array<std::uint32_t,
                ShadowMap::VirtualPagesPerAxis * ShadowMap::VirtualPagesPerAxis> remapped{};
            remapped.fill(ShadowMap::InvalidPage);
            for (std::int32_t y = 0; y < pageCount; ++y) {
                for (std::int32_t x = 0; x < pageCount; ++x) {
                    const std::uint32_t oldIndex = virtualPageIndex(
                        level, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                    const std::uint32_t physical = pageTable_[oldIndex];
                    if (physical == ShadowMap::InvalidPage) continue;
                    const std::int32_t newX = x + shiftX;
                    const std::int32_t newY = y + shiftY;
                    if (newX < 0 || newY < 0 || newX >= pageCount || newY >= pageCount) {
                        physicalPages_[physical] = {};
                        continue;
                    }
                    remapped[static_cast<std::size_t>(newY * pageCount + newX)] = physical;
                    physicalPages_[physical].virtualX = static_cast<std::uint16_t>(newX);
                    physicalPages_[physical].virtualY = static_cast<std::uint16_t>(newY);
                }
            }
            for (std::uint32_t y = 0; y < ShadowMap::VirtualPagesPerAxis; ++y)
                for (std::uint32_t x = 0; x < ShadowMap::VirtualPagesPerAxis; ++x)
                    pageTable_[virtualPageIndex(level, x, y)] =
                        remapped[y * ShadowMap::VirtualPagesPerAxis + x];
            cachedClipMatrices_[level] = clipMatrices[level];
            continue;
        }
        for (std::uint32_t y = 0; y < ShadowMap::VirtualPagesPerAxis; ++y) {
            for (std::uint32_t x = 0; x < ShadowMap::VirtualPagesPerAxis; ++x) {
                const std::uint32_t index = virtualPageIndex(level, x, y);
                const std::uint32_t physical = pageTable_[index];
                if (physical != ShadowMap::InvalidPage) physicalPages_[physical] = {};
                pageTable_[index] = ShadowMap::InvalidPage;
            }
        }
        cachedClipMatrices_[level] = clipMatrices[level];
        cachedClipMatricesValid_[level] = true;
    }
    invalidateDirtyPages(clipMatrices, false);

    struct VisibleObject {
        std::array<glm::vec4, 8> worldCorners{};
        float nearestDepth{};
    };
    std::vector<VisibleObject> visibleObjects;
    visibleObjects.reserve(objects.size());
    const glm::mat4 cameraMatrix = cameraViewProjection.native();
    for (const Culling::GPUObjectData& object : objects) {
        const glm::vec3 minimum{object.localAabbMin.x, object.localAabbMin.y,
                                object.localAabbMin.z};
        const glm::vec3 maximum{object.localAabbMax.x, object.localAabbMax.y,
                                object.localAabbMax.z};
        const glm::mat4 model = gpuMatrix(object.model);
        VisibleObject candidate{};
        bool outsideLeft = true, outsideRight = true, outsideBottom = true;
        bool outsideTop = true, outsideNear = true, outsideFar = true;
        candidate.nearestDepth = std::numeric_limits<float>::max();
        for (std::uint32_t corner = 0; corner < 8; ++corner) {
            const glm::vec3 local = glm::mix(minimum, maximum, unitCorners[corner]);
            candidate.worldCorners[corner] = model * glm::vec4{local, 1.0F};
            const glm::vec4 clip = cameraMatrix * candidate.worldCorners[corner];
            outsideLeft &= clip.x < -clip.w;
            outsideRight &= clip.x > clip.w;
            outsideBottom &= clip.y < -clip.w;
            outsideTop &= clip.y > clip.w;
            outsideNear &= clip.z < 0.0F;
            outsideFar &= clip.z > clip.w;
            if (clip.w > 1.0e-5F) candidate.nearestDepth =
                std::min(candidate.nearestDepth, clip.z / clip.w);
        }
        if (!(outsideLeft || outsideRight || outsideBottom || outsideTop ||
              outsideNear || outsideFar)) visibleObjects.push_back(candidate);
    }
    std::ranges::sort(visibleObjects, {}, &VisibleObject::nearestDepth);

    std::array<bool, ShadowMap::VirtualPageCount> requested{};
    std::vector<std::uint32_t> requests;
    requests.reserve(ShadowMap::PhysicalPageCount);
    constexpr std::array<std::uint32_t, ShadowMap::ClipLevelCount> levelBudgets{
        112, 64, 48, 32};
    // Large receivers such as a terrain cover many more virtual pages than
    // the per-object budget. Keep a camera-centred window at every level so
    // nearby grass does not fall straight through to the coarsest clipmap.
    constexpr std::array<std::int32_t, ShadowMap::ClipLevelCount> focusedWindowSizes{
        10, 8, 6, 5};
    std::array<std::uint32_t, ShadowMap::ClipLevelCount> levelCounts{};
    const auto requestRectangle = [&](const std::uint32_t level,
                                      const std::int32_t minimumX, const std::int32_t minimumY,
                                      const std::int32_t maximumX, const std::int32_t maximumY) {
        for (std::int32_t y = minimumY; y <= maximumY; ++y) {
            for (std::int32_t x = minimumX; x <= maximumX; ++x) {
                if (levelCounts[level] >= levelBudgets[level] ||
                    requests.size() >= ShadowMap::PhysicalPageCount) return;
                const std::uint32_t key = virtualPageIndex(
                    level, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                if (requested[key]) continue;
                requested[key] = true;
                requests.push_back(key);
                ++levelCounts[level];
            }
        }
    };
    for (const VisibleObject& object : visibleObjects) {
        for (std::uint32_t level = 0; level < ShadowMap::ClipLevelCount; ++level) {
            glm::vec2 minimumUv{std::numeric_limits<float>::max()};
            glm::vec2 maximumUv{-std::numeric_limits<float>::max()};
            for (const glm::vec4& world : object.worldCorners) {
                const glm::vec4 clip = clipMatrices[level].native() * world;
                const glm::vec2 uv = glm::vec2{clip} / clip.w * 0.5F + 0.5F;
                minimumUv = glm::min(minimumUv, uv);
                maximumUv = glm::max(maximumUv, uv);
            }
            if (maximumUv.x <= 0.0F || maximumUv.y <= 0.0F ||
                minimumUv.x >= 1.0F || minimumUv.y >= 1.0F) continue;
            const std::int32_t minimumX = std::clamp(
                static_cast<std::int32_t>(std::floor(minimumUv.x * pageCount)) - 1,
                0, pageCount - 1);
            const std::int32_t minimumY = std::clamp(
                static_cast<std::int32_t>(std::floor(minimumUv.y * pageCount)) - 1,
                0, pageCount - 1);
            const std::int32_t maximumX = std::clamp(
                static_cast<std::int32_t>(std::floor(maximumUv.x * pageCount)) + 1,
                0, pageCount - 1);
            const std::int32_t maximumY = std::clamp(
                static_cast<std::int32_t>(std::floor(maximumUv.y * pageCount)) + 1,
                0, pageCount - 1);
            const std::uint32_t rectanglePages =
                static_cast<std::uint32_t>((maximumX - minimumX + 1) *
                                           (maximumY - minimumY + 1));
            if (rectanglePages > 64) {
                const std::int32_t windowWidth = std::min(
                    focusedWindowSizes[level], maximumX - minimumX + 1);
                const std::int32_t windowHeight = std::min(
                    focusedWindowSizes[level], maximumY - minimumY + 1);
                const std::int32_t targetX = std::clamp(pageCount / 2, minimumX, maximumX);
                const std::int32_t targetY = std::clamp(pageCount / 2, minimumY, maximumY);
                const std::int32_t focusedMinimumX = std::clamp(
                    targetX - windowWidth / 2, minimumX, maximumX - windowWidth + 1);
                const std::int32_t focusedMinimumY = std::clamp(
                    targetY - windowHeight / 2, minimumY, maximumY - windowHeight + 1);
                requestRectangle(level, focusedMinimumX, focusedMinimumY,
                                 focusedMinimumX + windowWidth - 1,
                                 focusedMinimumY + windowHeight - 1);
                continue;
            }
            requestRectangle(level, minimumX, minimumY, maximumX, maximumY);
            break;
        }
    }

    constexpr std::uint32_t pagesPerLevel =
        ShadowMap::VirtualPagesPerAxis * ShadowMap::VirtualPagesPerAxis;
    for (const std::uint32_t key : requests) {
        std::uint32_t physical = pageTable_[key];
        if (physical == ShadowMap::InvalidPage) {
            // Do not map a page until it can be rendered. Otherwise the
            // sampling shader could observe stale atlas contents.
            if (pagesToRender_.size() >= ShadowMap::MaxPageUpdatesPerFrame) continue;
            physical = ShadowMap::InvalidPage;
            for (std::uint32_t slot = 0; slot < physicalPages_.size(); ++slot) {
                if (!physicalPages_[slot].allocated) { physical = slot; break; }
            }
            if (physical == ShadowMap::InvalidPage) {
                std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
                for (std::uint32_t slot = 0; slot < physicalPages_.size(); ++slot) {
                    const PhysicalPage& page = physicalPages_[slot];
                    const std::uint32_t owner = virtualPageIndex(
                        page.level, page.virtualX, page.virtualY);
                    if (!requested[owner] && page.lastUsed < oldest) {
                        oldest = page.lastUsed;
                        physical = slot;
                    }
                }
            }
            if (physical == ShadowMap::InvalidPage) continue;
            if (physicalPages_[physical].allocated) {
                const PhysicalPage& evicted = physicalPages_[physical];
                pageTable_[virtualPageIndex(evicted.level, evicted.virtualX,
                                             evicted.virtualY)] = ShadowMap::InvalidPage;
            }
            const std::uint32_t level = key / pagesPerLevel;
            const std::uint32_t local = key % pagesPerLevel;
            physicalPages_[physical] = PhysicalPage{
                static_cast<std::uint16_t>(local % ShadowMap::VirtualPagesPerAxis),
                static_cast<std::uint16_t>(local / ShadowMap::VirtualPagesPerAxis),
                static_cast<std::uint8_t>(level), true, false, cacheClock_};
            pageTable_[key] = physical;
            pagesToRender_.push_back(physical);
        } else {
            physicalPages_[physical].lastUsed = cacheClock_;
            if (physicalPages_[physical].dirty &&
                pagesToRender_.size() < ShadowMap::MaxPageUpdatesPerFrame) {
                physicalPages_[physical].dirty = false;
                pagesToRender_.push_back(physical);
            }
        }
    }
    pageTableBuffers_.at(frameIndex)->update(pageTable_.data(),
                                              sizeof(std::uint32_t) * pageTable_.size());
}

void ShadowPass::record(const VkCommandBuffer commandBuffer,
                        const std::array<Mat4, ShadowMap::ClipLevelCount>& clipMatrices,
                        std::uint32_t updateMask,
                        const VkBuffer vertexBuffer, const VkBuffer instanceBuffer,
                        const VkBuffer indexBuffer, const VkDescriptorSet sceneDescriptorSet,
                        const Culling::GPUCullingPass& cullingPass,
                        const Culling::IndexedIndirectDrawCount& indirectDraw,
                        const std::uint32_t objectCount) {
    (void)updateMask;
    if (objectCount == 0) {
        invalidateCache();
        atlasContentValid_ = false;
    }
    // The atlas already stays in shader-read layout after a completed pass.
    // Avoid opening a 4096x4096 LOAD render pass when every requested page is
    // cached; this is the steady state for both editor and play mode.
    if (atlasInitialized_ && pagesToRender_.empty()) return;
    // Compute all page-specific indirect lists before the render pass:
    // dispatches, fills and their barriers are invalid inside a render pass.
    if (objectCount != 0) {
        for (std::size_t pageIndex = 0; pageIndex < pagesToRender_.size(); ++pageIndex) {
            const PhysicalPage& page = physicalPages_[pagesToRender_[pageIndex]];
            glm::mat4 pageTransform{1.0F};
            pageTransform[0][0] = static_cast<float>(ShadowMap::VirtualPagesPerAxis);
            pageTransform[1][1] = static_cast<float>(ShadowMap::VirtualPagesPerAxis);
            pageTransform[3][0] = static_cast<float>(ShadowMap::VirtualPagesPerAxis) -
                                  2.0F * static_cast<float>(page.virtualX) - 1.0F;
            pageTransform[3][1] = static_cast<float>(ShadowMap::VirtualPagesPerAxis) -
                                  2.0F * static_cast<float>(page.virtualY) - 1.0F;
            const Mat4 pageMatrix{pageTransform * clipMatrices[page.level].native()};
            cullingPass.record(commandBuffer, objectCount, &pageMatrix,
                               static_cast<std::uint32_t>(pageIndex));
        }
    }

    VkImageMemoryBarrier2 atlasBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    atlasBarrier.srcStageMask = atlasInitialized_ ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                                  : VK_PIPELINE_STAGE_2_NONE;
    atlasBarrier.srcAccessMask = atlasInitialized_ ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0;
    atlasBarrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    atlasBarrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    atlasBarrier.oldLayout = atlasInitialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                               : VK_IMAGE_LAYOUT_UNDEFINED;
    atlasBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    atlasBarrier.image = shadowMap_.image();
    atlasBarrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo atlasDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    atlasDependency.imageMemoryBarrierCount = 1;
    atlasDependency.pImageMemoryBarriers = &atlasBarrier;
    vkCmdPipelineBarrier2(commandBuffer, &atlasDependency);

    VkRenderPassBeginInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    passInfo.renderPass = shadowMap_.renderPass();
    passInfo.framebuffer = shadowMap_.framebuffer();
    passInfo.renderArea.extent = {ShadowMap::Resolution, ShadowMap::Resolution};
    vkCmdBeginRenderPass(commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &sceneDescriptorSet, 0, nullptr);
    const VkBuffer vertexBuffers[] = {vertexBuffer, instanceBuffer};
    constexpr VkDeviceSize offsets[] = {0, 0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdSetDepthBias(commandBuffer, DepthBiasConstant, 0.0F, DepthBiasSlope);
    for (std::size_t pageIndex = 0; pageIndex < pagesToRender_.size(); ++pageIndex) {
        const std::uint32_t physical = pagesToRender_[pageIndex];
        const PhysicalPage& page = physicalPages_[physical];
        const std::int32_t x = static_cast<std::int32_t>(
            (physical % ShadowMap::PhysicalPagesPerAxis) * ShadowMap::PageResolution);
        const std::int32_t y = static_cast<std::int32_t>(
            (physical / ShadowMap::PhysicalPagesPerAxis) * ShadowMap::PageResolution);
        const VkRect2D tile{{x, y}, {ShadowMap::PageResolution, ShadowMap::PageResolution}};
        const VkClearAttachment clear{VK_IMAGE_ASPECT_DEPTH_BIT, 0, {.depthStencil = {1.0F, 0}}};
        const VkClearRect clearRect{tile, 0, 1};
        vkCmdClearAttachments(commandBuffer, 1, &clear, 1, &clearRect);
        const VkViewport viewport{static_cast<float>(x), static_cast<float>(y),
            static_cast<float>(ShadowMap::PageResolution), static_cast<float>(ShadowMap::PageResolution),
            0.0F, 1.0F};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &tile);
        glm::mat4 pageTransform{1.0F};
        pageTransform[0][0] = static_cast<float>(ShadowMap::VirtualPagesPerAxis);
        pageTransform[1][1] = static_cast<float>(ShadowMap::VirtualPagesPerAxis);
        pageTransform[3][0] = static_cast<float>(ShadowMap::VirtualPagesPerAxis) -
                              2.0F * static_cast<float>(page.virtualX) - 1.0F;
        pageTransform[3][1] = static_cast<float>(ShadowMap::VirtualPagesPerAxis) -
                              2.0F * static_cast<float>(page.virtualY) - 1.0F;
        const Mat4 pageMatrix{pageTransform * clipMatrices[page.level].native()};
        vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(Mat4), &pageMatrix);
        if (objectCount != 0 && indirectDraw.valid()) {
            indirectDraw.record(commandBuffer,
                sizeof(VkDrawIndexedIndirectCommand) * objectCount * pageIndex,
                sizeof(std::uint32_t) * pageIndex);
        }
    }
    vkCmdEndRenderPass(commandBuffer);
    atlasInitialized_ = true;
    if (objectCount != 0) atlasContentValid_ = true;
    pagesToRender_.clear();
}

} // namespace Engine
