#include "Engine/Renderer/RayTracing/ClusterRayTracingScene.h"

#include "Engine/Renderer/Vulkan/PipelineCache.h"
#include "Engine/Renderer/shader_loader.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace Engine {
namespace {
constexpr VkBufferUsageFlags Storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
constexpr VkBufferUsageFlags BuildInput = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
constexpr VkBufferUsageFlags AsStorage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
// Fixed per-cluster slots, matching cluster_prepare.slang and cluster_displace.slang.
constexpr std::uint32_t MaxClusterVertices = 64;
constexpr std::uint32_t MaxClusterTriangles = 126;
constexpr VkDeviceSize ClusterPositionBytes = MaxClusterVertices * 3 * sizeof(float);
constexpr VkDeviceSize ClusterIndexBytes = 384;
constexpr std::uint32_t MaxDispatchGroups = 65535;
constexpr VkDeviceSize PtlasAlignment = 256;
constexpr std::uint32_t MissingBlas = 0xffffffffU;
constexpr std::uint32_t NoTemplate = 0xffffffffU;
// Static instances are grouped into partitions of this edge length, so a
// rewritten instance rebuilds only its neighbourhood.
constexpr float PartitionCellSize = 32.0F;

using TriangleInfo = VkClusterAccelerationStructureBuildTriangleClusterInfoNV;
using TemplateInfo = VkClusterAccelerationStructureBuildTriangleClusterTemplateInfoNV;
using InstantiateInfo = VkClusterAccelerationStructureInstantiateClusterInfoNV;
using WriteData = VkPartitionedAccelerationStructureWriteInstanceDataNV;
using UpdateData = VkPartitionedAccelerationStructureUpdateInstanceDataNV;
using TranslationData = VkPartitionedAccelerationStructureWritePartitionTranslationDataNV;
using PtlasCommand = VkBuildPartitionedAccelerationStructureIndirectCommandNV;
static_assert(sizeof(TriangleInfo) == 64, "cluster_prepare.slang writes 16 words per triangle cluster");
static_assert(sizeof(TemplateInfo) == 72, "cluster_prepare.slang writes 18 words per cluster template");
static_assert(sizeof(InstantiateInfo) == 32, "cluster_displace.slang writes 8 words per instantiation");
static_assert(sizeof(WriteData) == 104 && offsetof(WriteData, accelerationStructure) == 96,
              "ptlas_patch_instances.slang patches word 24 of 26");
static_assert(sizeof(UpdateData) == 16 && offsetof(UpdateData, accelerationStructure) == 8,
              "ptlas_patch_instances.slang patches word 2 of 4");

struct PreparePush {
    VkDeviceAddress positionsAddress;
    VkDeviceAddress indicesAddress;
    std::uint32_t clusterCount;
    std::uint32_t groupsPerRow;
};
struct DisplacePush {
    VkDeviceAddress positionsAddress;
    std::uint32_t jobCount;
    std::uint32_t groupsPerRow;
};
struct PatchPush {
    std::uint32_t recordCount;
    std::uint32_t recordWords;
    std::uint32_t addressWord;
};

VkDeviceAddress alignUp(VkDeviceAddress value, VkDeviceAddress alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr VkPipelineStageFlags2 BuildStage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
constexpr VkPipelineStageFlags2 ComputeStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags2 RayStage = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
constexpr VkAccessFlags2 AsRead = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
constexpr VkAccessFlags2 AsWrite = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
constexpr VkAccessFlags2 ShaderWrite = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
constexpr VkAccessFlags2 ShaderRead = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
// Build inputs are read as indirect arguments (srcInfos) and as geometry.
constexpr VkAccessFlags2 BuildInputRead = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT | AsRead;

void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    const VkMemoryBarrier2 memory{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .srcStageMask = srcStage,
                                  .srcAccessMask = srcAccess,
                                  .dstStageMask = dstStage,
                                  .dstAccessMask = dstAccess};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &memory};
    vkCmdPipelineBarrier2(cmd, &dependency);
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
        throw std::runtime_error("Could not create cluster ray tracing compute pipeline");
    return pipeline;
}

VkDescriptorSetLayout storageSetLayout(VkDevice device, std::uint32_t bindings) {
    std::array<VkDescriptorSetLayoutBinding, 9> layoutBindings{};
    for (std::uint32_t b = 0; b < bindings; ++b)
        layoutBindings[b] = {b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = bindings;
    setInfo.pBindings = layoutBindings.data();
    VkDescriptorSetLayout layout{};
    if (vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &layout) != VK_SUCCESS)
        throw std::runtime_error("Could not create cluster ray tracing descriptor set layout");
    return layout;
}

VkPipelineLayout pipelineLayout(VkDevice device, std::span<const VkDescriptorSetLayout> sets, std::uint32_t pushBytes) {
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = static_cast<std::uint32_t>(sets.size());
    layoutInfo.pSetLayouts = sets.data();
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    VkPipelineLayout layout{};
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS)
        throw std::runtime_error("Could not create cluster ray tracing pipeline layout");
    return layout;
}

void writeStorageBuffers(VkDevice device, VkDescriptorSet set, std::span<const VkBuffer> buffers) {
    std::array<VkDescriptorBufferInfo, 9> infos{};
    std::array<VkWriteDescriptorSet, 9> writes{};
    for (std::uint32_t b = 0; b < buffers.size(); ++b) {
        infos[b] = {buffers[b], 0, VK_WHOLE_SIZE};
        writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[b].dstSet = set;
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &infos[b];
    }
    vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(buffers.size()), writes.data(), 0, nullptr);
}

void destroyKernel(VkDevice device, VkPipeline& pipeline, VkPipelineLayout& layout, VkDescriptorSetLayout& set) {
    if (pipeline)
        vkDestroyPipeline(device, pipeline, nullptr);
    if (layout)
        vkDestroyPipelineLayout(device, layout, nullptr);
    if (set)
        vkDestroyDescriptorSetLayout(device, set, nullptr);
    pipeline = VK_NULL_HANDLE;
    layout = VK_NULL_HANDLE;
    set = VK_NULL_HANDLE;
}
} // namespace

ClusterRayTracingScene::~ClusterRayTracingScene() {
    destroy();
}

void ClusterRayTracingScene::create(VkPhysicalDevice physical, VkDevice device, VmaAllocator allocator,
                                    Assets::AssetManager& assets, VkDescriptorSetLayout forwardLayout) {
    destroy();
    physicalDevice_ = physical;
    device_ = device;
    allocator_ = allocator;
    getClusterSizes_ = reinterpret_cast<PFN_vkGetClusterAccelerationStructureBuildSizesNV>(
        vkGetDeviceProcAddr(device, "vkGetClusterAccelerationStructureBuildSizesNV"));
    cmdBuildClusters_ = reinterpret_cast<PFN_vkCmdBuildClusterAccelerationStructureIndirectNV>(
        vkGetDeviceProcAddr(device, "vkCmdBuildClusterAccelerationStructureIndirectNV"));
    getPartitionedSizes_ = reinterpret_cast<PFN_vkGetPartitionedAccelerationStructuresBuildSizesNV>(
        vkGetDeviceProcAddr(device, "vkGetPartitionedAccelerationStructuresBuildSizesNV"));
    cmdBuildPartitioned_ = reinterpret_cast<PFN_vkCmdBuildPartitionedAccelerationStructuresNV>(
        vkGetDeviceProcAddr(device, "vkCmdBuildPartitionedAccelerationStructuresNV"));
    if (!getClusterSizes_ || !cmdBuildClusters_ || !getPartitionedSizes_ || !cmdBuildPartitioned_) {
        destroy();
        throw std::runtime_error("Cluster and partitioned acceleration structure entry points are unavailable");
    }
    clusterProperties_ = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_PROPERTIES_NV};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &clusterProperties_;
    vkGetPhysicalDeviceProperties2(physical, &properties);
    clusterProperties_.pNext = nullptr;
    if (clusterProperties_.maxVerticesPerCluster < MaxClusterVertices ||
        clusterProperties_.maxTrianglesPerCluster < MaxClusterTriangles) {
        destroy();
        throw std::runtime_error("Device clusters are smaller than engine meshlets");
    }
    try {
        prepare_.setLayout = storageSetLayout(device_, 9);
        prepare_.pipelineLayout = pipelineLayout(device_, std::array{prepare_.setLayout}, sizeof(PreparePush));
        prepare_.pipeline = makeCompute(device_, assets, "shaders/cluster_prepare.spv", prepare_.pipelineLayout);
        displace_.setLayout = storageSetLayout(device_, 8);
        displace_.pipelineLayout =
            pipelineLayout(device_, std::array{forwardLayout, displace_.setLayout}, sizeof(DisplacePush));
        displace_.pipeline = makeCompute(device_, assets, "shaders/cluster_displace.spv", displace_.pipelineLayout);
        patch_.setLayout = storageSetLayout(device_, 2);
        patch_.pipelineLayout = pipelineLayout(device_, std::array{patch_.setLayout}, sizeof(PatchPush));
        patch_.pipeline = makeCompute(device_, assets, "shaders/ptlas_patch_instances.spv", patch_.pipelineLayout);
        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 9 + 8 + 2 * FramesInFlight};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 2 + FramesInFlight;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS)
            throw std::runtime_error("Could not create cluster ray tracing descriptor pool");
        const auto allocate = [&](VkDescriptorSetLayout layout) {
            VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            info.descriptorPool = descriptorPool_;
            info.descriptorSetCount = 1;
            info.pSetLayouts = &layout;
            VkDescriptorSet set{};
            if (vkAllocateDescriptorSets(device_, &info, &set) != VK_SUCCESS)
                throw std::runtime_error("Could not allocate cluster ray tracing descriptors");
            return set;
        };
        prepareSet_ = allocate(prepare_.setLayout);
        displaceSet_ = allocate(displace_.setLayout);
        for (VkDescriptorSet& set : patchSets_)
            set = allocate(patch_.setLayout);
    } catch (...) {
        destroy();
        throw;
    }
}

void ClusterRayTracingScene::retire(Buffer& buffer) {
    if (buffer.handle())
        retired_.push_back({std::move(buffer), updateSerial_});
}

void ClusterRayTracingScene::releaseGeometry() noexcept {
    for (Buffer* buffer :
         {&clusterPositions_, &clusterIndices_, &clusterBuildInfos_, &templateInfos_, &templateAddresses_,
          &templateSizes_, &templateData_, &variantJobs_, &variantPositions_, &instantiateInfos_, &variantData_,
          &scratch_, &clusterMeshlets_, &clusterAddresses_, &clusterSizes_, &clusterData_, &blasBuildInfos_,
          &blasAddresses_, &blasSizes_, &blasData_})
        buffer->destroy();
    retired_.clear();
    blasIndices_.clear();
    variantIndices_.clear();
    blasFirstCluster_.clear();
    blasClusterCount_.clear();
    clusterCount_ = variantClusterCount_ = templateCount_ = maxClustersPerBlas_ = 0;
    compactionPending_ = false;
    stats_ = {};
    // Every PTLAS instance references a BLAS that no longer exists.
    tlasBuilt_ = false;
    instanceCount_ = 0;
}

void ClusterRayTracingScene::releaseScene() {
    if (!device_ || (!hasGeometry() && !tlasBuilt_ && !ptlasData_.handle()))
        return;
    if (vkDeviceWaitIdle(device_) != VK_SUCCESS)
        throw std::runtime_error("Could not idle device before releasing the cluster scene");
    releaseGeometry();
    ptlasData_.destroy();
    ptlasScratch_.destroy();
    ptlasInput_ = {};
    tlasAddress_ = 0;
    writtenTransforms_.clear();
    partitions_.clear();
    partitionOrigins_.clear();
    records_.clear();
    sources_ = {};
}

void ClusterRayTracingScene::destroy() noexcept {
    releaseGeometry();
    ptlasData_.destroy();
    ptlasScratch_.destroy();
    for (FrameResources& frame : frames_) {
        frame.records.destroy();
        frame.operations.destroy();
        frame.commands.destroy();
        frame.recordsRevision = 0;
    }
    if (device_) {
        if (descriptorPool_)
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        for (Kernel* kernel : {&prepare_, &displace_, &patch_})
            destroyKernel(device_, kernel->pipeline, kernel->pipelineLayout, kernel->setLayout);
    }
    descriptorPool_ = VK_NULL_HANDLE;
    prepareSet_ = displaceSet_ = VK_NULL_HANDLE;
    patchSets_ = {};
    ptlasInput_ = {};
    tlasAddress_ = 0;
    writtenTransforms_.clear();
    partitions_.clear();
    partitionOrigins_.clear();
    records_.clear();
    recordsRevision_ = 0;
    sources_ = {};
    device_ = VK_NULL_HANDLE;
    allocator_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
}

bool ClusterRayTracingScene::hasVariants(std::span<const VariantInput> variants) const {
    return std::ranges::all_of(variants, [&](const VariantInput& v) { return variantIndices_.contains(v); }) &&
           variants.size() >= variantIndices_.size();
}

std::uint32_t ClusterRayTracingScene::blasFor(const InstanceInput& instance) const noexcept {
    if (instance.displaced) {
        const auto variant = variantIndices_.find({instance.key, instance.materialBase});
        return variant == variantIndices_.end() ? MissingBlas : variant->second;
    }
    const auto blas = blasIndices_.find(instance.key);
    return blas == blasIndices_.end() ? MissingBlas : blas->second;
}

VkDeviceSize ClusterRayTracingScene::allocatedBytes() const noexcept {
    VkDeviceSize bytes = 0;
    for (const Buffer* buffer :
         {&clusterPositions_, &clusterIndices_, &clusterBuildInfos_, &templateInfos_, &templateAddresses_,
          &templateSizes_, &templateData_, &variantJobs_, &variantPositions_, &instantiateInfos_, &variantData_,
          &scratch_, &clusterMeshlets_, &clusterAddresses_, &clusterSizes_, &clusterData_, &blasBuildInfos_,
          &blasAddresses_, &blasSizes_, &blasData_, &ptlasData_, &ptlasScratch_})
        bytes += buffer->size();
    return bytes;
}

void ClusterRayTracingScene::buildGeometry(VkCommandBuffer cmd, VkPipeline tracingPipeline, VkDescriptorSet forwardSet,
                                           std::span<const BlasInput> blases, std::span<const VariantInput> variants,
                                           const GeometrySources& sources) {
    if (!device_)
        throw std::logic_error("Cluster ray tracing scene is not initialized");
    if ((hasGeometry() || tlasBuilt_) && vkDeviceWaitIdle(device_) != VK_SUCCESS)
        throw std::runtime_error("Could not idle device before rebuilding cluster acceleration structures");
    releaseGeometry();
    sources_ = sources;
    if (!sources.meshlets || !sources.meshletVertices || !sources.meshletTriangles || !sources.vertices)
        return;

    // Cluster table indexed by ClusterID: base clusters first, then the
    // clusters instantiated for displaced variants.
    std::vector<std::array<std::uint32_t, 4>> clusters;
    // Inputs sharing a key are meshlet ranges of one cluster BLAS (e.g. every
    // section of a mesh): one TLAS instance then covers the whole object.
    std::vector<BlasKey> order;
    std::unordered_map<BlasKey, std::vector<const BlasInput*>, AccelerationStructureManager::BlasKeyHash> ranges;
    for (const BlasInput& blas : blases) {
        if (blas.meshletCount == 0)
            continue;
        auto [it, inserted] = ranges.try_emplace(blas.key);
        if (inserted)
            order.push_back(blas.key);
        if (std::ranges::none_of(it->second, [&](const BlasInput* known) {
                return known->firstMeshlet == blas.firstMeshlet && known->meshletCount == blas.meshletCount;
            }))
            it->second.push_back(&blas);
    }
    for (const BlasKey& key : order) {
        blasIndices_.emplace(key, static_cast<std::uint32_t>(blasFirstCluster_.size()));
        blasFirstCluster_.push_back(static_cast<std::uint32_t>(clusters.size()));
        std::uint32_t local = 0;
        for (const BlasInput* blas : ranges.at(key))
            for (std::uint32_t meshlet = 0; meshlet < blas->meshletCount; ++meshlet)
                clusters.push_back({blas->firstMeshlet + meshlet, local++, NoTemplate, 0});
        blasClusterCount_.push_back(local);
        maxClustersPerBlas_ = std::max(maxClustersPerBlas_, local);
    }
    clusterCount_ = static_cast<std::uint32_t>(clusters.size());
    if (clusterCount_ == 0) {
        releaseGeometry();
        sources_ = sources;
        return;
    }
    // x: variant cluster, y: base cluster, z: material base, w: position slot.
    std::vector<std::array<std::uint32_t, 4>> jobs;
    for (const VariantInput& variant : variants) {
        const auto base = blasIndices_.find(variant.key);
        if (base == blasIndices_.end() || variantIndices_.contains(variant))
            continue;
        const std::uint32_t baseFirst = blasFirstCluster_[base->second];
        const std::uint32_t count = blasClusterCount_[base->second];
        // One template per meshlet, shared by every material that displaces it.
        for (std::uint32_t local = 0; local < count; ++local)
            if (clusters[baseFirst + local][2] == NoTemplate)
                clusters[baseFirst + local][2] = templateCount_++;
        variantIndices_.emplace(variant, static_cast<std::uint32_t>(blasFirstCluster_.size()));
        blasFirstCluster_.push_back(static_cast<std::uint32_t>(clusters.size()));
        blasClusterCount_.push_back(count);
        for (std::uint32_t local = 0; local < count; ++local) {
            jobs.push_back({static_cast<std::uint32_t>(clusters.size()), baseFirst + local, variant.materialBase,
                            static_cast<std::uint32_t>(jobs.size())});
            clusters.push_back({clusters[baseFirst + local][0], local, NoTemplate, 0});
        }
    }
    variantClusterCount_ = static_cast<std::uint32_t>(jobs.size());
    const auto totalClusters = static_cast<std::uint32_t>(clusters.size());
    const auto blasCount = static_cast<std::uint32_t>(blasFirstCluster_.size());

    VkClusterAccelerationStructureTriangleClusterInputNV triangles{
        VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_TRIANGLE_CLUSTER_INPUT_NV};
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.maxGeometryIndexValue = maxClustersPerBlas_ - 1;
    triangles.maxClusterUniqueGeometryCount = 1;
    triangles.maxClusterTriangleCount = MaxClusterTriangles;
    triangles.maxClusterVertexCount = MaxClusterVertices;
    const auto clusterInput = [&](VkClusterAccelerationStructureOpTypeNV op, std::uint32_t count,
                                  VkClusterAccelerationStructureTriangleClusterInputNV& input) {
        input = triangles;
        input.maxTotalTriangleCount = count * MaxClusterTriangles;
        input.maxTotalVertexCount = count * MaxClusterVertices;
        VkClusterAccelerationStructureInputInfoNV info{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV};
        info.maxAccelerationStructureCount = count;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        info.opType = op;
        info.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_IMPLICIT_DESTINATIONS_NV;
        info.opInput.pTriangleClusters = &input;
        return info;
    };
    VkClusterAccelerationStructureTriangleClusterInputNV baseTriangles{}, templateTriangles{}, variantTriangles{};
    const VkClusterAccelerationStructureInputInfoNV baseInput =
        clusterInput(VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_NV, clusterCount_, baseTriangles);
    const VkClusterAccelerationStructureInputInfoNV templateInput = clusterInput(
        VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_TEMPLATE_NV, templateCount_, templateTriangles);
    const VkClusterAccelerationStructureInputInfoNV variantInput = clusterInput(
        VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_INSTANTIATE_TRIANGLE_CLUSTER_NV, variantClusterCount_,
        variantTriangles);
    const auto sizesOf = [&](const VkClusterAccelerationStructureInputInfoNV& input) {
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        if (input.maxAccelerationStructureCount > 0)
            getClusterSizes_(device_, &input, &sizes);
        return sizes;
    };
    const auto baseSizes = sizesOf(baseInput);
    const auto templateSizes = sizesOf(templateInput);
    const auto variantSizes = sizesOf(variantInput);
    VkClusterAccelerationStructureClustersBottomLevelInputNV bottomLevel{
        VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_CLUSTERS_BOTTOM_LEVEL_INPUT_NV};
    bottomLevel.maxTotalClusterCount = totalClusters;
    bottomLevel.maxClusterCountPerAccelerationStructure = maxClustersPerBlas_;
    VkClusterAccelerationStructureInputInfoNV blasInput{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV};
    blasInput.maxAccelerationStructureCount = blasCount;
    blasInput.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    blasInput.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_CLUSTERS_BOTTOM_LEVEL_NV;
    blasInput.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_IMPLICIT_DESTINATIONS_NV;
    blasInput.opInput.pClustersBottomLevel = &bottomLevel;
    const auto blasSizes = sizesOf(blasInput);

    const VkDeviceSize scratchAlignment = clusterProperties_.clusterScratchByteAlignment;
    const auto deviceBuffer = [&](Buffer& buffer, VkDeviceSize bytes, VkBufferUsageFlags usage) {
        buffer.createDeviceLocalEmpty(device_, std::max<VkDeviceSize>(bytes, 16), usage, allocator_, true);
    };
    const auto hostBuffer = [&](Buffer& buffer, VkDeviceSize bytes, VkBufferUsageFlags usage, const void* data) {
        buffer.createHostVisible(physicalDevice_, device_, std::max<VkDeviceSize>(bytes, 16), usage, allocator_,
                                 true);
        if (data && bytes)
            buffer.update(data, bytes);
    };
    hostBuffer(clusterMeshlets_, sizeof(clusters[0]) * clusters.size(), Storage, clusters.data());
    deviceBuffer(clusterPositions_, ClusterPositionBytes * clusterCount_, Storage | BuildInput);
    deviceBuffer(clusterIndices_, ClusterIndexBytes * clusterCount_, Storage | BuildInput);
    deviceBuffer(clusterBuildInfos_, sizeof(TriangleInfo) * clusterCount_, Storage | BuildInput);
    deviceBuffer(templateInfos_, sizeof(TemplateInfo) * templateCount_, Storage | BuildInput);
    deviceBuffer(templateAddresses_, sizeof(VkDeviceAddress) * templateCount_, Storage | AsStorage);
    deviceBuffer(templateSizes_, sizeof(std::uint32_t) * templateCount_, Storage);
    deviceBuffer(templateData_, templateSizes.accelerationStructureSize + clusterProperties_.clusterTemplateByteAlignment,
                 AsStorage);
    hostBuffer(variantJobs_, sizeof(jobs.empty() ? std::array<std::uint32_t, 4>{} : jobs[0]) * jobs.size(), Storage,
               jobs.data());
    deviceBuffer(variantPositions_, ClusterPositionBytes * variantClusterCount_, Storage | BuildInput);
    deviceBuffer(instantiateInfos_, sizeof(InstantiateInfo) * variantClusterCount_, Storage | BuildInput);
    deviceBuffer(variantData_, variantSizes.accelerationStructureSize + clusterProperties_.clusterByteAlignment,
                 AsStorage);
    deviceBuffer(clusterAddresses_, sizeof(VkDeviceAddress) * totalClusters, Storage | BuildInput | AsStorage);
    // Read back by compaction, which moves exactly the bytes the build used.
    hostBuffer(clusterSizes_, sizeof(std::uint32_t) * totalClusters, Storage, nullptr);
    deviceBuffer(clusterData_, baseSizes.accelerationStructureSize + clusterProperties_.clusterByteAlignment,
                 AsStorage);
    deviceBuffer(scratch_,
                 std::max({baseSizes.buildScratchSize, templateSizes.buildScratchSize, variantSizes.buildScratchSize,
                           blasSizes.buildScratchSize}) +
                     scratchAlignment,
                 Storage);
    std::vector<VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV> blasInfos;
    blasInfos.reserve(blasCount);
    for (std::uint32_t blas = 0; blas < blasCount; ++blas)
        blasInfos.push_back(
            {blasClusterCount_[blas], sizeof(VkDeviceAddress),
             clusterAddresses_.deviceAddress() + VkDeviceAddress{blasFirstCluster_[blas]} * sizeof(VkDeviceAddress)});
    hostBuffer(blasBuildInfos_, sizeof(blasInfos[0]) * blasInfos.size(), BuildInput, blasInfos.data());
    deviceBuffer(blasAddresses_, sizeof(VkDeviceAddress) * blasCount, Storage | BuildInput | AsStorage);
    hostBuffer(blasSizes_, sizeof(std::uint32_t) * blasCount, Storage, nullptr);
    deviceBuffer(blasData_, blasSizes.accelerationStructureSize + clusterProperties_.clusterBottomLevelByteAlignment,
                 AsStorage);

    // 1. Meshlets to cluster inputs and template inputs.
    const std::array prepareBuffers{sources.meshlets,          sources.meshletVertices,   sources.meshletTriangles,
                                    sources.vertices,          clusterMeshlets_.handle(), clusterPositions_.handle(),
                                    clusterIndices_.handle(),  clusterBuildInfos_.handle(), templateInfos_.handle()};
    writeStorageBuffers(device_, prepareSet_, prepareBuffers);
    const std::uint32_t prepareGroups = std::min(clusterCount_, MaxDispatchGroups);
    const PreparePush preparePush{clusterPositions_.deviceAddress(), clusterIndices_.deviceAddress(), clusterCount_,
                                  prepareGroups};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prepare_.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prepare_.pipelineLayout, 0, 1, &prepareSet_, 0,
                            nullptr);
    vkCmdPushConstants(cmd, prepare_.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(preparePush), &preparePush);
    vkCmdDispatch(cmd, prepareGroups, (clusterCount_ + prepareGroups - 1) / prepareGroups, 1);
    barrier(cmd, ComputeStage, ShaderWrite, BuildStage, BuildInputRead);

    // 2. Base clusters and templates; independent, so one barrier follows both.
    // VUID-vkCmdBuildClusterAccelerationStructureIndirectNV-pNext-10444: the
    // bound ray tracing pipeline must allow cluster acceleration structures.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, tracingPipeline);
    const auto clusterCommand = [&](const VkClusterAccelerationStructureInputInfoNV& input, const Buffer& data,
                                    VkDeviceAddress alignment, VkDeviceAddress addresses, VkDeviceAddress sizes,
                                    const Buffer& sourceInfos, VkDeviceSize infoStride) {
        const std::uint32_t count = input.maxAccelerationStructureCount;
        VkClusterAccelerationStructureCommandsInfoNV command{
            VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV};
        command.input = input;
        command.dstImplicitData = alignUp(data.deviceAddress(), alignment);
        command.scratchData = alignUp(scratch_.deviceAddress(), scratchAlignment);
        command.dstAddressesArray = {addresses, sizeof(VkDeviceAddress), sizeof(VkDeviceAddress) * count};
        command.dstSizesArray = {sizes, sizeof(std::uint32_t), sizeof(std::uint32_t) * count};
        command.srcInfosArray = {sourceInfos.deviceAddress(), infoStride, infoStride * count};
        cmdBuildClusters_(cmd, &command);
    };
    clusterCommand(baseInput, clusterData_, clusterProperties_.clusterByteAlignment, clusterAddresses_.deviceAddress(),
                   clusterSizes_.deviceAddress(), clusterBuildInfos_, sizeof(TriangleInfo));
    if (templateCount_ > 0) {
        // Scratch is shared: order the template build after the cluster build.
        barrier(cmd, BuildStage, AsWrite | AsRead, BuildStage, AsWrite | AsRead);
        clusterCommand(templateInput, templateData_, clusterProperties_.clusterTemplateByteAlignment,
                       templateAddresses_.deviceAddress(), templateSizes_.deviceAddress(), templateInfos_,
                       sizeof(TemplateInfo));
    }
    // 3. Displaced positions and instantiation of the templates.
    if (variantClusterCount_ > 0) {
        barrier(cmd, BuildStage, AsWrite, ComputeStage | BuildStage, ShaderRead | AsRead | AsWrite);
        const std::array displaceBuffers{sources.meshlets,          sources.meshletVertices,      sources.vertices,
                                         clusterMeshlets_.handle(), variantJobs_.handle(),        templateAddresses_.handle(),
                                         variantPositions_.handle(), instantiateInfos_.handle()};
        writeStorageBuffers(device_, displaceSet_, displaceBuffers);
        const std::uint32_t groups = std::min(variantClusterCount_, MaxDispatchGroups);
        const DisplacePush push{variantPositions_.deviceAddress(), variantClusterCount_, groups};
        const std::array sets{forwardSet, displaceSet_};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, displace_.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, displace_.pipelineLayout, 0,
                                static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
        vkCmdPushConstants(cmd, displace_.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, groups, (variantClusterCount_ + groups - 1) / groups, 1);
        barrier(cmd, ComputeStage, ShaderWrite, BuildStage, BuildInputRead);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, tracingPipeline);
        clusterCommand(variantInput, variantData_, clusterProperties_.clusterByteAlignment,
                       clusterAddresses_.deviceAddress() + VkDeviceAddress{clusterCount_} * sizeof(VkDeviceAddress),
                       clusterSizes_.deviceAddress() + VkDeviceAddress{clusterCount_} * sizeof(std::uint32_t),
                       instantiateInfos_, sizeof(InstantiateInfo));
    }
    // 4. Cluster BLASes link the CLAS addresses the builds reported.
    barrier(cmd, BuildStage, AsWrite | AsRead, BuildStage, BuildInputRead | AsWrite);
    buildBlases(cmd);
    builtSerial_ = updateSerial_;
    compactionPending_ = true;
    stats_ = {.builtBytes = 0,
              .compactedBytes = 0,
              .clusters = clusterCount_,
              .templates = templateCount_,
              .displacedClusters = variantClusterCount_,
              .blases = blasCount,
              .compacted = false};
}

void ClusterRayTracingScene::buildBlases(VkCommandBuffer cmd) {
    const auto blasCount = static_cast<std::uint32_t>(blasFirstCluster_.size());
    VkClusterAccelerationStructureClustersBottomLevelInputNV bottomLevel{
        VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_CLUSTERS_BOTTOM_LEVEL_INPUT_NV};
    bottomLevel.maxTotalClusterCount = clusterCount_ + variantClusterCount_;
    bottomLevel.maxClusterCountPerAccelerationStructure = maxClustersPerBlas_;
    VkClusterAccelerationStructureCommandsInfoNV command{
        VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV};
    command.input = {VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV};
    command.input.maxAccelerationStructureCount = blasCount;
    command.input.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    command.input.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_CLUSTERS_BOTTOM_LEVEL_NV;
    command.input.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_IMPLICIT_DESTINATIONS_NV;
    command.input.opInput.pClustersBottomLevel = &bottomLevel;
    command.dstImplicitData = alignUp(blasData_.deviceAddress(), clusterProperties_.clusterBottomLevelByteAlignment);
    command.scratchData = alignUp(scratch_.deviceAddress(), clusterProperties_.clusterScratchByteAlignment);
    command.dstAddressesArray = {blasAddresses_.deviceAddress(), sizeof(VkDeviceAddress),
                                 sizeof(VkDeviceAddress) * blasCount};
    command.dstSizesArray = {blasSizes_.deviceAddress(), sizeof(std::uint32_t), sizeof(std::uint32_t) * blasCount};
    command.srcInfosArray = {blasBuildInfos_.deviceAddress(),
                             sizeof(VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV),
                             sizeof(VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV) * blasCount};
    cmdBuildClusters_(cmd, &command);
    // BLAS addresses are patched into PTLAS records by compute; the BLASes are
    // read by the PTLAS build and by traversal.
    barrier(cmd, BuildStage, AsWrite, ComputeStage | BuildStage | RayStage,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | AsRead);
}

void ClusterRayTracingScene::ensurePtlas(std::uint32_t instanceCount, std::uint32_t partitionCount,
                                         std::uint32_t maxPerPartition) {
    if (ptlasData_.handle() && ptlasInput_.instanceCount >= instanceCount &&
        ptlasInput_.partitionCount >= partitionCount && ptlasInput_.maxInstancePerPartitionCount >= maxPerPartition)
        return;
    if (ptlasData_.handle() && vkDeviceWaitIdle(device_) != VK_SUCCESS)
        throw std::runtime_error("Could not idle device before resizing the partitioned TLAS");
    // Headroom so a few added instances do not reallocate every frame.
    const auto grow = [](std::uint32_t value) { return value + value / 4 + 8; };
    // Partition translation: instances are stored relative to their
    // partition, so world-space magnitude never reaches instance transforms.
    ptlasFlags_ = {VK_STRUCTURE_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_FLAGS_NV};
    ptlasFlags_.enablePartitionTranslation = VK_TRUE;
    ptlasInput_ = {VK_STRUCTURE_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_INSTANCES_INPUT_NV};
    ptlasInput_.pNext = &ptlasFlags_;
    ptlasInput_.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    ptlasInput_.instanceCount = grow(instanceCount);
    ptlasInput_.partitionCount = grow(partitionCount);
    ptlasInput_.maxInstancePerPartitionCount = grow(maxPerPartition);
    // Every instance may end up in the global partition after it moves.
    ptlasInput_.maxInstanceInGlobalPartitionCount = ptlasInput_.instanceCount;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    getPartitionedSizes_(device_, &ptlasInput_, &sizes);
    ptlasData_.destroy();
    ptlasScratch_.destroy();
    ptlasData_.createDeviceLocalEmpty(device_, sizes.accelerationStructureSize + PtlasAlignment, AsStorage, allocator_,
                                      true);
    ptlasScratch_.createDeviceLocalEmpty(device_, sizes.buildScratchSize + PtlasAlignment, Storage, allocator_, true);
    tlasAddress_ = alignUp(ptlasData_.deviceAddress(), PtlasAlignment);
    tlasBuilt_ = false;
}

void ClusterRayTracingScene::patchAddresses(VkCommandBuffer cmd, std::uint32_t frameSlot, const Buffer& records,
                                            std::uint32_t count, std::uint32_t recordWords,
                                            std::uint32_t addressWord) {
    const std::array buffers{records.handle(), blasAddresses_.handle()};
    const VkDescriptorSet set = patchSets_[frameSlot % FramesInFlight];
    writeStorageBuffers(device_, set, buffers);
    const PatchPush push{count, recordWords, addressWord};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, patch_.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, patch_.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, patch_.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);
}

void ClusterRayTracingScene::buildPtlas(VkCommandBuffer cmd, const FrameResources& frame, std::uint32_t commandCount,
                                        bool fromScratch) {
    // The previous frame's traversal must finish before an in-place rewrite.
    barrier(cmd, ComputeStage | RayStage | BuildStage, ShaderWrite | AsRead | AsWrite, BuildStage,
            BuildInputRead | AsWrite);
    VkBuildPartitionedAccelerationStructureInfoNV build{VK_STRUCTURE_TYPE_BUILD_PARTITIONED_ACCELERATION_STRUCTURE_INFO_NV};
    build.input = ptlasInput_;
    build.srcAccelerationStructureData = fromScratch ? 0 : tlasAddress_;
    build.dstAccelerationStructureData = tlasAddress_;
    build.scratchData = alignUp(ptlasScratch_.deviceAddress(), PtlasAlignment);
    build.srcInfos = frame.commands.deviceAddress();
    build.srcInfosCount = frame.commands.deviceAddress() + sizeof(PtlasCommand) * 2;
    frame.commands.update(&commandCount, sizeof(commandCount), sizeof(PtlasCommand) * 2);
    cmdBuildPartitioned_(cmd, &build);
    barrier(cmd, BuildStage, AsWrite, RayStage | BuildStage, AsRead | AsWrite);
}

void ClusterRayTracingScene::compact(VkCommandBuffer cmd, VkPipeline tracingPipeline, std::uint32_t frameSlot,
                                     std::span<const InstanceInput> instances) {
    const std::uint32_t totalClusters = clusterCount_ + variantClusterCount_;
    std::vector<std::uint32_t> sizes(totalClusters);
    clusterSizes_.read(sizes.data(), sizeof(std::uint32_t) * sizes.size());
    VkDeviceSize usedBytes = 0;
    for (const std::uint32_t size : sizes)
        usedBytes += alignUp(size, clusterProperties_.clusterByteAlignment);
    stats_.builtBytes = allocatedBytes();

    // MOVE_OBJECTS with implicit destinations packs the clusters tightly by
    // their actual sizes; the conservative build allocation is then freed.
    VkClusterAccelerationStructureMoveObjectsInputNV move{
        VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_MOVE_OBJECTS_INPUT_NV};
    move.type = VK_CLUSTER_ACCELERATION_STRUCTURE_TYPE_TRIANGLE_CLUSTER_NV;
    move.noMoveOverlap = VK_TRUE;
    move.maxMovedBytes = usedBytes;
    VkClusterAccelerationStructureInputInfoNV moveInput{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV};
    moveInput.maxAccelerationStructureCount = totalClusters;
    moveInput.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_MOVE_OBJECTS_NV;
    moveInput.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_IMPLICIT_DESTINATIONS_NV;
    moveInput.opInput.pMoveObjects = &move;
    VkAccelerationStructureBuildSizesInfoKHR moveSizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    getClusterSizes_(device_, &moveInput, &moveSizes);
    Buffer compactData;
    compactData.createDeviceLocalEmpty(device_, moveSizes.accelerationStructureSize + clusterProperties_.clusterByteAlignment,
                                       AsStorage, allocator_, true);
    Buffer compactAddresses;
    compactAddresses.createDeviceLocalEmpty(device_, sizeof(VkDeviceAddress) * totalClusters,
                                            Storage | BuildInput | AsStorage, allocator_, true);
    if (scratch_.size() < moveSizes.buildScratchSize + clusterProperties_.clusterScratchByteAlignment) {
        retire(scratch_);
        scratch_.createDeviceLocalEmpty(device_, moveSizes.buildScratchSize + clusterProperties_.clusterScratchByteAlignment,
                                        Storage, allocator_, true);
    }

    barrier(cmd, RayStage | BuildStage | ComputeStage, AsRead | AsWrite | ShaderWrite, BuildStage,
            BuildInputRead | AsWrite);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, tracingPipeline);
    VkClusterAccelerationStructureCommandsInfoNV moveCommand{
        VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV};
    moveCommand.input = moveInput;
    moveCommand.dstImplicitData = alignUp(compactData.deviceAddress(), clusterProperties_.clusterByteAlignment);
    moveCommand.scratchData = alignUp(scratch_.deviceAddress(), clusterProperties_.clusterScratchByteAlignment);
    moveCommand.dstAddressesArray = {compactAddresses.deviceAddress(), sizeof(VkDeviceAddress),
                                     sizeof(VkDeviceAddress) * totalClusters};
    // VkClusterAccelerationStructureMoveObjectsInfoNV is the source address alone.
    moveCommand.srcInfosArray = {clusterAddresses_.deviceAddress(), sizeof(VkDeviceAddress),
                                 sizeof(VkDeviceAddress) * totalClusters};
    cmdBuildClusters_(cmd, &moveCommand);
    barrier(cmd, BuildStage, AsWrite, BuildStage, BuildInputRead | AsWrite);

    // Cluster BLASes reference CLASes by address: relink them in place.
    std::vector<VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV> blasInfos;
    for (std::size_t blas = 0; blas < blasFirstCluster_.size(); ++blas)
        blasInfos.push_back({blasClusterCount_[blas], sizeof(VkDeviceAddress),
                             compactAddresses.deviceAddress() +
                                 VkDeviceAddress{blasFirstCluster_[blas]} * sizeof(VkDeviceAddress)});
    blasBuildInfos_.update(blasInfos.data(), sizeof(blasInfos[0]) * blasInfos.size());
    retire(clusterData_);
    retire(clusterAddresses_);
    clusterData_ = std::move(compactData);
    clusterAddresses_ = std::move(compactAddresses);
    buildBlases(cmd);
    for (Buffer* input : {&clusterPositions_, &clusterIndices_, &clusterBuildInfos_, &templateInfos_,
                          &templateAddresses_, &templateSizes_, &templateData_, &variantJobs_, &variantPositions_,
                          &instantiateInfos_, &variantData_, &scratch_})
        retire(*input);

    // UPDATE_INSTANCE swaps only the BLAS address of an instance; its
    // transform and partition stay as written.
    FrameResources& frame = frames_[frameSlot % FramesInFlight];
    const auto count = static_cast<std::uint32_t>(instances.size());
    std::vector<UpdateData> updates(count);
    for (std::uint32_t i = 0; i < count; ++i)
        updates[i] = {i, 0, blasFor(instances[i])};
    frame.operations.update(updates.data(), sizeof(UpdateData) * updates.size());
    const PtlasCommand command{VK_PARTITIONED_ACCELERATION_STRUCTURE_OP_TYPE_UPDATE_INSTANCE_NV, count,
                               {frame.operations.deviceAddress(), sizeof(UpdateData)}};
    frame.commands.update(&command, sizeof(command));
    patchAddresses(cmd, frameSlot, frame.operations, count, 4, 2);
    buildPtlas(cmd, frame, 1, false);
    compactionPending_ = false;
    stats_.compacted = true;
    stats_.compactedBytes = allocatedBytes();
}

void ClusterRayTracingScene::updateInstances(VkCommandBuffer cmd, VkPipeline tracingPipeline, std::uint32_t frameSlot,
                                             std::span<const InstanceInput> instances, bool topologyChanged) {
    ++updateSerial_;
    // Buffers retired by an earlier call are free once every frame that could
    // still read them has passed its fence.
    std::erase_if(retired_, [&](const RetiredBuffer& retired) { return updateSerial_ > retired.serial + FramesInFlight; });
    if (!device_ || !hasGeometry() || instances.empty()) {
        instanceCount_ = 0;
        return;
    }
    const auto count = static_cast<std::uint32_t>(instances.size());
    bool full = topologyChanged || !tlasBuilt_ || count != instanceCount_;
    if (full) {
        std::unordered_map<std::int64_t, std::uint32_t> cells;
        std::vector<std::uint32_t> population;
        partitions_.assign(count, 0);
        partitionOrigins_.clear();
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto& t = instances[i].transform;
            const std::array<float, 3> cell{std::floor(t[3] / PartitionCellSize), std::floor(t[7] / PartitionCellSize),
                                            std::floor(t[11] / PartitionCellSize)};
            const auto bits = [](float value) { return static_cast<std::int64_t>(value) & 0x1fffff; };
            const std::int64_t key = bits(cell[0]) | (bits(cell[1]) << 21) | (bits(cell[2]) << 42);
            const auto [it, inserted] = cells.try_emplace(key, static_cast<std::uint32_t>(cells.size()));
            if (inserted) {
                population.push_back(0);
                partitionOrigins_.push_back({(cell[0] + 0.5F) * PartitionCellSize, (cell[1] + 0.5F) * PartitionCellSize,
                                             (cell[2] + 0.5F) * PartitionCellSize});
            }
            partitions_[i] = it->second;
            ++population[it->second];
        }
        ensurePtlas(count, static_cast<std::uint32_t>(cells.size()),
                    *std::max_element(population.begin(), population.end()));
        full = full || !tlasBuilt_;
        writtenTransforms_.assign(count, {});
    }
    FrameResources& frame = frames_[frameSlot % FramesInFlight];
    const VkDeviceSize operationBytes =
        sizeof(WriteData) * ptlasInput_.instanceCount + sizeof(TranslationData) * ptlasInput_.partitionCount;
    if (frame.operations.size() < operationBytes) {
        frame.operations.destroy();
        frame.operations.createHostVisible(physicalDevice_, device_, operationBytes, Storage | BuildInput, allocator_,
                                           true);
    }
    if (!frame.commands.handle())
        frame.commands.createHostVisible(physicalDevice_, device_, 64, BuildInput, allocator_, true);
    const VkDeviceSize recordBytes = sizeof(InstanceRecord) * count;
    if (frame.records.size() < recordBytes) {
        frame.records.destroy();
        frame.records.createHostVisible(physicalDevice_, device_, sizeof(InstanceRecord) * ptlasInput_.instanceCount,
                                        Storage, allocator_);
        frame.recordsRevision = 0;
    }
    // Compaction waits until the build's sizes are readable; that frame only
    // repoints instances, and transform changes follow in the next one.
    if (compactionPending_ && !full && updateSerial_ > builtSerial_ + FramesInFlight) {
        compact(cmd, tracingPipeline, frameSlot, instances);
        if (frame.recordsRevision != recordsRevision_) {
            frame.records.update(records_.data(), recordBytes);
            frame.recordsRevision = recordsRevision_;
        }
        return;
    }

    std::vector<std::uint32_t> writes;
    writes.reserve(full ? count : 16);
    records_.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const InstanceInput& input = instances[i];
        if (!full && input.transform == writtenTransforms_[i] && records_[i].materialBase == input.materialBase)
            continue;
        if (!full) {
            // A moving instance joins the global partition, which the
            // implementation rebuilds cheaply without touching static ones.
            partitions_[i] = VK_PARTITIONED_ACCELERATION_STRUCTURE_PARTITION_INDEX_GLOBAL_NV;
        }
        records_[i] = {input.transform, 0, input.materialBase, {}};
        writtenTransforms_[i] = input.transform;
        writes.push_back(i);
    }
    if (!writes.empty())
        ++recordsRevision_;
    if (frame.recordsRevision != recordsRevision_) {
        frame.records.update(records_.data(), recordBytes);
        frame.recordsRevision = recordsRevision_;
    }
    if (writes.empty())
        return;

    std::vector<WriteData> data(writes.size());
    for (std::size_t w = 0; w < writes.size(); ++w) {
        const std::uint32_t i = writes[w];
        WriteData& write = data[w];
        std::memcpy(write.transform.matrix, instances[i].transform.data(), sizeof(write.transform.matrix));
        if (partitions_[i] != VK_PARTITIONED_ACCELERATION_STRUCTURE_PARTITION_INDEX_GLOBAL_NV)
            for (int row = 0; row < 3; ++row)
                write.transform.matrix[row][3] -= partitionOrigins_[partitions_[i]][static_cast<std::size_t>(row)];
        write.instanceID = i;
        write.instanceMask = 0xff;
        write.instanceFlags = VK_PARTITIONED_ACCELERATION_STRUCTURE_INSTANCE_FLAG_TRIANGLE_FACING_CULL_DISABLE_BIT_NV;
        write.instanceIndex = i;
        write.partitionIndex = partitions_[i];
        // Replaced on the GPU by the cluster BLAS address the build reported.
        write.accelerationStructure = blasFor(instances[i]);
    }
    frame.operations.update(data.data(), sizeof(WriteData) * data.size());
    std::array<PtlasCommand, 2> commands{};
    commands[0] = {VK_PARTITIONED_ACCELERATION_STRUCTURE_OP_TYPE_WRITE_INSTANCE_NV,
                   static_cast<std::uint32_t>(writes.size()), {frame.operations.deviceAddress(), sizeof(WriteData)}};
    std::uint32_t commandCount = 1;
    if (full) {
        std::vector<TranslationData> translations(partitionOrigins_.size());
        for (std::uint32_t p = 0; p < translations.size(); ++p)
            translations[p] = {p, {partitionOrigins_[p][0], partitionOrigins_[p][1], partitionOrigins_[p][2]}};
        const VkDeviceSize translationOffset = sizeof(WriteData) * ptlasInput_.instanceCount;
        frame.operations.update(translations.data(), sizeof(TranslationData) * translations.size(), translationOffset);
        commands[1] = {VK_PARTITIONED_ACCELERATION_STRUCTURE_OP_TYPE_WRITE_PARTITION_TRANSLATION_NV,
                       static_cast<std::uint32_t>(translations.size()),
                       {frame.operations.deviceAddress() + translationOffset, sizeof(TranslationData)}};
        commandCount = 2;
    }
    frame.commands.update(commands.data(), sizeof(commands));
    patchAddresses(cmd, frameSlot, frame.operations, static_cast<std::uint32_t>(writes.size()), 26, 24);
    buildPtlas(cmd, frame, commandCount, full);
    instanceCount_ = count;
    tlasBuilt_ = true;
}
} // namespace Engine
