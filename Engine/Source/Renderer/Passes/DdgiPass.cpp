#include "Engine/Renderer/Passes/DdgiPass.h"

#include "Engine/Renderer/RayTracing/ClusterRayTracingScene.h"
#include "Engine/Renderer/Vulkan/PipelineCache.h"
#include "Engine/Renderer/shader_loader.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

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
                                             .color = {0.4F, 0.9F, 0.4F, 1.0F}};
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

struct TracePush {
    VkDeviceAddress sceneAddress;
};
struct SelectPush {
    std::uint32_t updateInterval;
};

constexpr VkPipelineStageFlags2 ComputeStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags2 RayStage = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
constexpr VkPipelineStageFlags2 TransferStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
constexpr VkPipelineStageFlags2 IndirectStage = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
constexpr VkAccessFlags2 ShaderRead = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
constexpr VkAccessFlags2 ShaderWrite = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
// The raygen shader reads frame data, materials and the volume uniforms.
constexpr VkAccessFlags2 AnyShaderRead = ShaderRead | VK_ACCESS_2_UNIFORM_READ_BIT;

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

VkImageMemoryBarrier2 imageBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                   VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

void submitImageBarriers(VkCommandBuffer cmd, std::span<const VkImageMemoryBarrier2> barriers) {
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
    dependency.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dependency);
}

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

VkDescriptorSetLayout makeSetLayout(VkDevice device, std::span<const VkDescriptorSetLayoutBinding> bindings) {
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout{};
    if (vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) != VK_SUCCESS)
        throw std::runtime_error("Could not create DDGI descriptor set layout");
    return layout;
}

VkPipelineLayout makePipelineLayout(VkDevice device, std::span<const VkDescriptorSetLayout> sets,
                                    VkShaderStageFlags pushStages, std::uint32_t pushBytes) {
    const VkPushConstantRange push{pushStages, 0, pushBytes};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = static_cast<std::uint32_t>(sets.size());
    info.pSetLayouts = sets.data();
    info.pushConstantRangeCount = pushBytes ? 1U : 0U;
    info.pPushConstantRanges = pushBytes ? &push : nullptr;
    VkPipelineLayout layout{};
    if (vkCreatePipelineLayout(device, &info, nullptr, &layout) != VK_SUCCESS)
        throw std::runtime_error("Could not create DDGI pipeline layout");
    return layout;
}

VkPipeline makeCompute(VkDevice device, Assets::AssetManager& assets, const char* path, VkPipelineLayout layout) {
    auto shader = Vkutil::loadShaderModule(device, assets, path);
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = shader.get();
    info.stage.pName = "main";
    info.layout = layout;
    VkPipeline pipeline{};
    if (vkCreateComputePipelines(device, PipelineCache::get(device), 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        throw std::runtime_error("Could not create DDGI compute pipeline");
    return pipeline;
}

// A uniformly distributed rotation (Shoemake) from a per-frame hash, so the
// fixed spherical Fibonacci rays cover new directions every update.
void randomRotation(std::uint64_t seed, float rows[3][4]) {
    const auto hash = [](std::uint64_t x) {
        x ^= x >> 33U;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33U;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33U;
        return static_cast<float>(x >> 40U) / static_cast<float>(1U << 24U);
    };
    const float u1 = hash(seed * 3 + 1), u2 = hash(seed * 3 + 2), u3 = hash(seed * 3 + 3);
    constexpr float TwoPi = 6.28318530718F;
    const float a = std::sqrt(1.0F - u1), b = std::sqrt(u1);
    const float x = a * std::sin(TwoPi * u2), y = a * std::cos(TwoPi * u2);
    const float z = b * std::sin(TwoPi * u3), w = b * std::cos(TwoPi * u3);
    const float m[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                           {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                           {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            rows[r][c] = m[r][c];
        rows[r][3] = 0.0F;
    }
}
} // namespace

DdgiPass::~DdgiPass() {
    destroy();
}

void DdgiPass::create(VkPhysicalDevice physical, VkDevice device, VmaAllocator allocator, Assets::AssetManager& assets,
                      bool rayTracing, VkDescriptorSetLayout forwardLayout,
                      std::span<const std::uint32_t> sharingFamilies) {
    destroy();
    physicalDevice_ = physical;
    device_ = device;
    allocator_ = allocator;
    try {
        for (Buffer& volume : volumes_) {
            volume.createHostVisible(physical, device, sizeof(VolumeData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                     allocator);
            const VolumeData disabled{};
            volume.update(&disabled, sizeof(disabled));
        }
        if (!rayTracing)
            return;
        cmdTraceRaysIndirect_ = reinterpret_cast<PFN_vkCmdTraceRaysIndirectKHR>(
            vkGetDeviceProcAddr(device, "vkCmdTraceRaysIndirectKHR"));
        beginLabel_ = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
            vkGetDeviceProcAddr(device, "vkCmdBeginDebugUtilsLabelEXT"));
        endLabel_ =
            reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(vkGetDeviceProcAddr(device, "vkCmdEndDebugUtilsLabelEXT"));
        if (!cmdTraceRaysIndirect_)
            throw std::runtime_error("vkCmdTraceRaysIndirectKHR is unavailable");
        constexpr std::uint32_t tilesPerRow = ProbesX * ProbesY;
        // Cascades stack their tile rows vertically.
        constexpr std::uint32_t tileRows = ProbesZ * CascadeCount;
        irradianceExtent_ = {tilesPerRow * (IrradianceTexels + 2), tileRows * (IrradianceTexels + 2)};
        distanceExtent_ = {tilesPerRow * (DistanceTexels + 2), tileRows * (DistanceTexels + 2)};
        irradiance_.create(physical, device, irradianceExtent_, allocator, VK_FILTER_LINEAR, IrradianceFormat, true,
                           sharingFamilies);
        distance_.create(physical, device, distanceExtent_, allocator, VK_FILTER_LINEAR, DistanceFormat, true,
                         sharingFamilies);
        rays_.create(physical, device, {MaxRaysPerProbe, ProbeCount}, allocator, VK_FILTER_NEAREST,
                     VK_FORMAT_R32G32B32A32_SFLOAT, true);
        for (Buffer& count : tracedCounts_)
            count.createHostVisible(physical, device, sizeof(std::uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    allocator);
        probeData_.createDeviceLocalEmpty(device, sizeof(std::int32_t) * 4 * ProbeCount,
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          allocator);
        probeList_.createDeviceLocalEmpty(device, sizeof(std::uint32_t) * ProbeCount,
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, allocator);
        indirectArgs_.createDeviceLocalEmpty(device, 32,
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                             allocator, true);

        const std::array<VkDescriptorPoolSize, 4> poolSizes{{
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 * FramesInFlight},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * FramesInFlight},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 * FramesInFlight},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 12 * FramesInFlight},
        }};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 3 * FramesInFlight;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS)
            throw std::runtime_error("Could not create DDGI descriptor pool");
        createTracePipeline(assets, forwardLayout);
        createComputeKernels(assets);
    } catch (...) {
        destroy();
        throw;
    }
}

void DdgiPass::createTracePipeline(Assets::AssetManager& assets, VkDescriptorSetLayout forwardLayout) {
    constexpr VkShaderStageFlags Raygen = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    std::array<VkDescriptorSetLayoutBinding, 11> bindings{{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, Raygen, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, Raygen, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, Raygen, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, Raygen, nullptr},
    }};
    for (std::uint32_t b = 4; b < bindings.size(); ++b)
        bindings[b] = {b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, Raygen, nullptr};
    traceSetLayout_ = makeSetLayout(device_, bindings);
    const std::array sets{forwardLayout, traceSetLayout_};
    tracePipelineLayout_ = makePipelineLayout(device_, sets, Raygen, sizeof(TracePush));

    auto shader = Vkutil::loadShaderModule(device_, assets, "shaders/ddgi_trace.spv");
    std::array<VkPipelineShaderStageCreateInfo, 3> stages{};
    const std::array<const char*, 3> names{"rayGen", "miss", "closestHit"};
    const std::array<VkShaderStageFlagBits, 3> stageBits{VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
                                                         VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};
    for (std::size_t i = 0; i < stages.size(); ++i) {
        stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[i].stage = stageBits[i];
        stages[i].module = shader.get();
        stages[i].pName = names[i];
    }
    std::array<VkRayTracingShaderGroupCreateInfoKHR, 3> groups{};
    for (auto& group : groups) {
        group = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        group.generalShader = group.closestHitShader = group.anyHitShader = group.intersectionShader =
            VK_SHADER_UNUSED_KHR;
    }
    groups[0].type = groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[1].generalShader = 1;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].closestHitShader = 2;
    VkRayTracingPipelineClusterAccelerationStructureCreateInfoNV clusters{
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CLUSTER_ACCELERATION_STRUCTURE_CREATE_INFO_NV};
    clusters.allowClusterAccelerationStructure = VK_TRUE;
    VkRayTracingPipelineCreateInfoKHR info{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    info.pNext = &clusters;
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.groupCount = static_cast<std::uint32_t>(groups.size());
    info.pGroups = groups.data();
    // Shading and the shadow ray run in raygen; nothing recurses.
    info.maxPipelineRayRecursionDepth = 1;
    info.layout = tracePipelineLayout_;
    const auto createPipelines = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
        vkGetDeviceProcAddr(device_, "vkCreateRayTracingPipelinesKHR"));
    const auto getHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
        vkGetDeviceProcAddr(device_, "vkGetRayTracingShaderGroupHandlesKHR"));
    if (!createPipelines || !getHandles ||
        createPipelines(device_, VK_NULL_HANDLE, PipelineCache::get(device_), 1, &info, nullptr, &tracePipeline_) !=
            VK_SUCCESS)
        throw std::runtime_error("Could not create the DDGI ray tracing pipeline");

    VkPhysicalDeviceRayTracingPipelinePropertiesKHR properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties2.pNext = &properties;
    vkGetPhysicalDeviceProperties2(physicalDevice_, &properties2);
    const VkDeviceSize handleSize = properties.shaderGroupHandleSize;
    const VkDeviceSize stride = alignUp(handleSize, properties.shaderGroupHandleAlignment);
    const VkDeviceSize regionSize = alignUp(stride, properties.shaderGroupBaseAlignment);
    std::vector<std::uint8_t> handles(handleSize * groups.size());
    if (getHandles(device_, tracePipeline_, 0, static_cast<std::uint32_t>(groups.size()), handles.size(),
                   handles.data()) != VK_SUCCESS)
        throw std::runtime_error("Could not read DDGI shader group handles");
    const VkDeviceSize baseAlignment = properties.shaderGroupBaseAlignment;
    shaderBindingTable_.createHostVisible(physicalDevice_, device_, regionSize * groups.size() + baseAlignment,
                                          VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR, allocator_, true);
    const VkDeviceAddress base = alignUp(shaderBindingTable_.deviceAddress(), baseAlignment);
    const VkDeviceSize baseOffset = base - shaderBindingTable_.deviceAddress();
    for (std::size_t group = 0; group < groups.size(); ++group)
        shaderBindingTable_.update(handles.data() + group * handleSize, handleSize, baseOffset + group * regionSize);
    raygenRegion_ = {base, regionSize, regionSize};
    missRegion_ = {base + regionSize, stride, stride};
    hitRegion_ = {base + 2 * regionSize, stride, stride};
    callableRegion_ = {};

    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptorPool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &traceSetLayout_;
    for (std::uint32_t frame = 0; frame < FramesInFlight; ++frame) {
        if (vkAllocateDescriptorSets(device_, &allocate, &traceSets_[frame]) != VK_SUCCESS)
            throw std::runtime_error("Could not allocate DDGI trace descriptors");
        const VkDescriptorBufferInfo volume{volumes_[frame].handle(), 0, sizeof(VolumeData)};
        const VkDescriptorImageInfo irradiance{irradiance_.sampler(), irradiance_.imageView(), VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo distance{distance_.sampler(), distance_.imageView(), VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo rays{VK_NULL_HANDLE, rays_.imageView(), VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorBufferInfo list{probeList_.handle(), 0, VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet, 5> writes{};
        for (auto& write : writes) {
            write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = traceSets_[frame];
            write.descriptorCount = 1;
        }
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &volume;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &irradiance;
        writes[2].dstBinding = 2;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &distance;
        writes[3].dstBinding = 3;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[3].pImageInfo = &rays;
        writes[4].dstBinding = 4;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[4].pBufferInfo = &list;
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void DdgiPass::createComputeKernels(Assets::AssetManager& assets) {
    constexpr VkShaderStageFlags Compute = VK_SHADER_STAGE_COMPUTE_BIT;
    const std::array<VkDescriptorSetLayoutBinding, 4> selectBindings{{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, Compute, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, Compute, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, Compute, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, Compute, nullptr},
    }};
    select_.setLayout = makeSetLayout(device_, selectBindings);
    select_.pipelineLayout = makePipelineLayout(device_, std::array{select_.setLayout}, Compute, sizeof(SelectPush));
    select_.pipeline = makeCompute(device_, assets, "shaders/ddgi_probe_select.spv", select_.pipelineLayout);
    const std::array<VkDescriptorSetLayoutBinding, 6> blendBindings{{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, Compute, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, Compute, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, Compute, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, Compute, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, Compute, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, Compute, nullptr},
    }};
    blend_.setLayout = makeSetLayout(device_, blendBindings);
    blend_.pipelineLayout = makePipelineLayout(device_, std::array{blend_.setLayout}, Compute, 0);
    blend_.pipeline = makeCompute(device_, assets, "shaders/ddgi_probe_blend.spv", blend_.pipelineLayout);

    for (Kernel* kernel : {&select_, &blend_}) {
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = descriptorPool_;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &kernel->setLayout;
        for (VkDescriptorSet& set : kernel->sets)
            if (vkAllocateDescriptorSets(device_, &allocate, &set) != VK_SUCCESS)
                throw std::runtime_error("Could not allocate DDGI compute descriptors");
    }
    for (std::uint32_t frame = 0; frame < FramesInFlight; ++frame) {
        const VkDescriptorBufferInfo volume{volumes_[frame].handle(), 0, sizeof(VolumeData)};
        const VkDescriptorBufferInfo data{probeData_.handle(), 0, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo list{probeList_.handle(), 0, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo args{indirectArgs_.handle(), 0, VK_WHOLE_SIZE};
        const VkDescriptorImageInfo rays{VK_NULL_HANDLE, rays_.imageView(), VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo irradiance{VK_NULL_HANDLE, irradiance_.imageView(), VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo distance{VK_NULL_HANDLE, distance_.imageView(), VK_IMAGE_LAYOUT_GENERAL};
        const auto bufferWrite = [](VkDescriptorSet set, std::uint32_t binding, VkDescriptorType type,
                                    const VkDescriptorBufferInfo* info) {
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = set;
            write.dstBinding = binding;
            write.descriptorCount = 1;
            write.descriptorType = type;
            write.pBufferInfo = info;
            return write;
        };
        const auto imageWrite = [](VkDescriptorSet set, std::uint32_t binding, const VkDescriptorImageInfo* info) {
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = set;
            write.dstBinding = binding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            write.pImageInfo = info;
            return write;
        };
        const VkDescriptorSet selectSet = select_.sets[frame], blendSet = blend_.sets[frame];
        const std::array writes{
            bufferWrite(selectSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &volume),
            bufferWrite(selectSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &data),
            bufferWrite(selectSet, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &list),
            bufferWrite(selectSet, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &args),
            bufferWrite(blendSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &volume),
            imageWrite(blendSet, 1, &rays),
            bufferWrite(blendSet, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &list),
            bufferWrite(blendSet, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &data),
            imageWrite(blendSet, 4, &irradiance),
            imageWrite(blendSet, 5, &distance),
        };
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void DdgiPass::destroy() noexcept {
    if (device_) {
        if (descriptorPool_)
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        for (Kernel* kernel : {&select_, &blend_}) {
            if (kernel->pipeline)
                vkDestroyPipeline(device_, kernel->pipeline, nullptr);
            if (kernel->pipelineLayout)
                vkDestroyPipelineLayout(device_, kernel->pipelineLayout, nullptr);
            if (kernel->setLayout)
                vkDestroyDescriptorSetLayout(device_, kernel->setLayout, nullptr);
            *kernel = {};
        }
        if (tracePipeline_)
            vkDestroyPipeline(device_, tracePipeline_, nullptr);
        if (tracePipelineLayout_)
            vkDestroyPipelineLayout(device_, tracePipelineLayout_, nullptr);
        if (traceSetLayout_)
            vkDestroyDescriptorSetLayout(device_, traceSetLayout_, nullptr);
    }
    descriptorPool_ = VK_NULL_HANDLE;
    tracePipeline_ = VK_NULL_HANDLE;
    tracePipelineLayout_ = VK_NULL_HANDLE;
    traceSetLayout_ = VK_NULL_HANDLE;
    traceSets_ = {};
    traceBuffers_ = {};
    for (Buffer& volume : volumes_)
        volume.destroy();
    for (Buffer& count : tracedCounts_)
        count.destroy();
    tracedCountPending_ = {};
    tracedProbes_ = 0;
    irradiance_.destroy();
    distance_.destroy();
    rays_.destroy();
    probeData_.destroy();
    probeList_.destroy();
    indirectArgs_.destroy();
    shaderBindingTable_.destroy();
    cmdTraceRaysIndirect_ = nullptr;
    device_ = VK_NULL_HANDLE;
    allocator_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
    gridValid_ = false;
    reset();
}

void DdgiPass::reset() noexcept {
    initialized_ = false;
    traced_ = false;
}

void DdgiPass::clearImages(VkCommandBuffer cmd, bool graphOwnsAtlases) {
    if (initialized_ || !available())
        return;
    // Atlases rest in SHADER_READ_ONLY between frames, the state Forward
    // samples and the render graph imports. When the graph already moved
    // them to GENERAL for this frame's writes, they are cleared in place.
    const VkImageLayout clearLayout =
        graphOwnsAtlases ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkImageLayout atlasOld = graphOwnsAtlases ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    const std::array before{
        imageBarrier(irradiance_.image(), atlasOld, clearLayout, ComputeStage | RayStage, 0, TransferStage,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT),
        imageBarrier(distance_.image(), atlasOld, clearLayout, ComputeStage | RayStage, 0, TransferStage,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT),
        imageBarrier(rays_.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                     RayStage | ComputeStage, ShaderRead | ShaderWrite),
    };
    submitImageBarriers(cmd, before);
    const VkClearColorValue zero{{0, 0, 0, 0}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, irradiance_.image(), clearLayout, &zero, 1, &range);
    vkCmdClearColorImage(cmd, distance_.image(), clearLayout, &zero, 1, &range);
    vkCmdFillBuffer(cmd, probeData_.handle(), 0, VK_WHOLE_SIZE, 0);
    const VkImageLayout atlasNew = graphOwnsAtlases ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkPipelineStageFlags2 readers =
        ComputeStage | RayStage | (graphOwnsAtlases ? 0 : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    const std::array after{
        imageBarrier(irradiance_.image(), clearLayout, atlasNew, TransferStage, VK_ACCESS_2_TRANSFER_WRITE_BIT, readers,
                     ShaderRead | ShaderWrite),
        imageBarrier(distance_.image(), clearLayout, atlasNew, TransferStage, VK_ACCESS_2_TRANSFER_WRITE_BIT, readers,
                     ShaderRead | ShaderWrite),
    };
    submitImageBarriers(cmd, after);
    memoryBarrier(cmd, TransferStage, VK_ACCESS_2_TRANSFER_WRITE_BIT, ComputeStage, ShaderRead | ShaderWrite);
    initialized_ = true;
    traced_ = false;
}

void DdgiPass::prepareFrame(std::uint32_t frameSlot, Vec3 camera, const DdgiSettings& settings, bool traced) {
    if (!available())
        return;
    // The slot's previous frame has passed its fence, so its count is final.
    const std::uint32_t slot = frameSlot % FramesInFlight;
    if (tracedCountPending_[slot]) {
        tracedCounts_[slot].read(&tracedProbes_, sizeof(tracedProbes_));
        tracedCountPending_[slot] = false;
    }
    if (settings.enabled != lastSettings_.enabled || settings.quality != lastSettings_.quality ||
        settings.intensity != lastSettings_.intensity)
        wakeAll_ = true;
    lastSettings_ = settings;
    const DdgiQualitySettings quality = ddgiQualitySettings(settings.quality);
    raysPerProbe_ = std::min(quality.raysPerProbe, MaxRaysPerProbe);
    updateInterval_ = std::max(1U, quality.updateInterval);
    const float spacing = std::max(settings.probeSpacing, 0.1F);
    VolumeData& data = volumeData_[frameSlot % FramesInFlight];
    // Probe positions depend on the spacing, so changing it voids every probe.
    if (gridValid_ && data.spacingBias[0] != 0.0F && data.spacingBias[0] != spacing)
        reset();
    std::array<float, CascadeCount> cascadeSpacing{};
    for (std::uint32_t cascade = 0; cascade < CascadeCount; ++cascade)
        cascadeSpacing[cascade] = spacing * std::pow(CascadeSpacingScale, static_cast<float>(cascade));
    if (traced) {
        const std::array<std::int32_t, 3> counts{ProbesX, ProbesY, ProbesZ};
        const std::array<float, 3> position{camera.x(), camera.y(), camera.z()};
        for (std::uint32_t cascade = 0; cascade < CascadeCount; ++cascade)
            for (std::size_t axis = 0; axis < 3; ++axis)
                gridMin_[cascade][axis] =
                    static_cast<std::int32_t>(std::floor(position[axis] / cascadeSpacing[cascade])) - counts[axis] / 2;
        gridValid_ = true;
    }
    data = {};
    data.gridMin[0] = gridMin_[0][0];
    data.gridMin[1] = gridMin_[0][1];
    data.gridMin[2] = gridMin_[0][2];
    data.gridMin[3] = settings.enabled && traced_ && gridValid_ ? 1 : 0;
    data.probeCounts[0] = ProbesX;
    data.probeCounts[1] = ProbesY;
    data.probeCounts[2] = ProbesZ;
    data.probeCounts[3] = ProbesPerCascade;
    for (std::uint32_t cascade = 0; cascade < CascadeCount; ++cascade) {
        for (std::size_t axis = 0; axis < 3; ++axis)
            data.cascadeGrid[cascade][axis] = gridMin_[cascade][axis];
        std::memcpy(&data.cascadeGrid[cascade][3], &cascadeSpacing[cascade], sizeof(float));
    }
    data.cascades[0] = CascadeCount;
    // Biases are in units of the probe spacing.
    data.spacingBias[0] = spacing;
    data.spacingBias[1] = 0.25F;
    data.spacingBias[2] = 0.05F;
    data.spacingBias[3] = std::max(settings.intensity, 0.0F);
    data.rayParams[0] = spacing * 64.0F;
    // Hysteresis per update, scaled so the visible convergence time does
    // not depend on how often a probe is refreshed.
    data.rayParams[1] = std::pow(0.97F, static_cast<float>(updateInterval_));
    data.rayParams[2] = 50.0F;
    data.rayParams[3] = static_cast<float>(raysPerProbe_);
    data.atlasInfo[0] = ProbesX * ProbesY;
    data.atlasInfo[1] = IrradianceTexels;
    data.atlasInfo[2] = DistanceTexels;
    data.atlasInfo[3] = static_cast<std::uint32_t>(frameIndex_);
    data.atlasSize[0] = static_cast<float>(irradianceExtent_.width);
    data.atlasSize[1] = static_cast<float>(irradianceExtent_.height);
    data.atlasSize[2] = static_cast<float>(distanceExtent_.width);
    data.atlasSize[3] = static_cast<float>(distanceExtent_.height);
    randomRotation(frameIndex_, data.rayRotation);
    // An undisturbed probe sleeps once hysteresis has decayed its oldest
    // history below 1 %: further rays would only re-average the same answer.
    const float hysteresis = data.rayParams[1];
    data.wake[2] = static_cast<std::uint32_t>(
        std::clamp(std::ceil(std::log(0.01F) / std::log(std::max(hysteresis, 0.01F))), 1.0F, 255.0F));
    volumes_[frameSlot % FramesInFlight].update(&data, sizeof(data));
}

void DdgiPass::wakeProbes(std::uint32_t frameSlot, const WakeRequest& request) {
    if (!available())
        return;
    VolumeData& data = volumeData_[frameSlot % FramesInFlight];
    const bool all = request.all || wakeAll_ || request.boxes.size() > MaxWakeBoxes;
    data.wake[0] = all ? 1U : 0U;
    data.wake[1] = all ? 0U : static_cast<std::uint32_t>(request.boxes.size());
    for (std::uint32_t box = 0; box < data.wake[1]; ++box)
        for (int corner = 0; corner < 2; ++corner) {
            for (int axis = 0; axis < 3; ++axis)
                data.wakeBoxes[box * 2 + static_cast<std::uint32_t>(corner)][axis] =
                    request.boxes[box][static_cast<std::size_t>(corner * 3 + axis)];
            data.wakeBoxes[box * 2 + static_cast<std::uint32_t>(corner)][3] = 0.0F;
        }
    volumes_[frameSlot % FramesInFlight].update(&data.wake, sizeof(data.wake) + sizeof(data.wakeBoxes),
                                                offsetof(VolumeData, wake));
    // Consumed by this frame's probe selection.
    wakeAll_ = false;
}

void DdgiPass::writeDescriptors(std::uint32_t frame, const ClusterRayTracingScene& scene,
                                const GeometryBuffers& geometry) {
    const std::array<VkBuffer, 6> buffers{scene.instanceRecords(frame), scene.clusterMeshlets(), geometry.meshlets,
                                          geometry.meshletVertices,     geometry.meshletTriangles, geometry.vertices};
    if (buffers == traceBuffers_[frame])
        return;
    std::array<VkDescriptorBufferInfo, 6> infos{};
    std::array<VkWriteDescriptorSet, 6> writes{};
    for (std::uint32_t i = 0; i < buffers.size(); ++i) {
        infos[i] = {buffers[i], 0, VK_WHOLE_SIZE};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = traceSets_[frame];
        writes[i].dstBinding = 5 + i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    traceBuffers_[frame] = buffers;
}

void DdgiPass::record(VkCommandBuffer cmd, std::uint32_t frameSlot, VkDescriptorSet forwardSet,
                      const ClusterRayTracingScene& scene, const GeometryBuffers& geometry) {
    const std::uint32_t frame = frameSlot % FramesInFlight;
    const GpuLabel label{cmd, beginLabel_, endLabel_, "DDGI"};
    if (!initialized_)
        clearImages(cmd, true);
    if (!scene.ready())
        return;
    writeDescriptors(frame, scene, geometry);

    // The graph moved the atlases to GENERAL with compute as its destination
    // stage; chain that into the ray tracing stage, and order the argument
    // reset after the previous frame's indirect reads.
    memoryBarrier(cmd, ComputeStage | RayStage | IndirectStage | TransferStage,
                  ShaderWrite | ShaderRead | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
                  TransferStage | ComputeStage | RayStage, VK_ACCESS_2_TRANSFER_WRITE_BIT | ShaderRead | ShaderWrite);
    // VkTraceRaysIndirectCommandKHR {rays, probes, 1}, VkDispatchIndirectCommand {probes, 1, 1}.
    const std::array<std::uint32_t, 8> args{raysPerProbe_, 0, 1, 0, 0, 1, 1, 0};
    vkCmdUpdateBuffer(cmd, indirectArgs_.handle(), 0, sizeof(args), args.data());
    memoryBarrier(cmd, TransferStage, VK_ACCESS_2_TRANSFER_WRITE_BIT, ComputeStage, ShaderRead | ShaderWrite);
    {
        const GpuLabel selectLabel{cmd, beginLabel_, endLabel_, "DDGI.Select"};
        const SelectPush push{updateInterval_};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, select_.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, select_.pipelineLayout, 0, 1, &select_.sets[frame],
                                0, nullptr);
        vkCmdPushConstants(cmd, select_.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (ProbeCount + 63) / 64, 1, 1);
    }
    memoryBarrier(cmd, ComputeStage, ShaderWrite, IndirectStage | RayStage | ComputeStage | TransferStage,
                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | AnyShaderRead | VK_ACCESS_2_TRANSFER_READ_BIT);
    // The traced-probe count feeds the cache statistics two frames later.
    const VkBufferCopy countCopy{sizeof(std::uint32_t), 0, sizeof(std::uint32_t)};
    vkCmdCopyBuffer(cmd, indirectArgs_.handle(), tracedCounts_[frame].handle(), 1, &countCopy);
    tracedCountPending_[frame] = true;
    {
        const GpuLabel traceLabel{cmd, beginLabel_, endLabel_, "DDGI.Trace"};
        const std::array sets{forwardSet, traceSets_[frame]};
        const TracePush push{scene.tlasAddress()};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, tracePipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, tracePipelineLayout_, 0,
                                static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
        vkCmdPushConstants(cmd, tracePipelineLayout_, VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(push), &push);
        cmdTraceRaysIndirect_(cmd, &raygenRegion_, &missRegion_, &hitRegion_, &callableRegion_,
                              indirectArgs_.deviceAddress());
    }
    memoryBarrier(cmd, RayStage, ShaderWrite, ComputeStage | IndirectStage,
                  ShaderRead | ShaderWrite | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    {
        const GpuLabel blendLabel{cmd, beginLabel_, endLabel_, "DDGI.Blend"};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blend_.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blend_.pipelineLayout, 0, 1, &blend_.sets[frame],
                                0, nullptr);
        vkCmdDispatchIndirect(cmd, indirectArgs_.handle(), 16);
    }
    // The next frame's trace samples these atlases from the ray tracing stage.
    memoryBarrier(cmd, ComputeStage, ShaderWrite, ComputeStage | RayStage, ShaderRead | ShaderWrite);
    traced_ = true;
    ++frameIndex_;
}
} // namespace Engine
