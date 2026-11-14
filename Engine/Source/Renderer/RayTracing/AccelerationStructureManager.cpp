#include "Engine/Renderer/RayTracing/AccelerationStructureManager.h"

#include "Engine/Core/Diagnostics.h"
#include "Engine/Renderer/Geometry/GpuVertex.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace Engine {
namespace {
constexpr VkBufferUsageFlags Storage =
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
constexpr VkBufferUsageFlags Scratch = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
constexpr VkBufferUsageFlags Input =
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
void buildBarrier(VkCommandBuffer cmd, VkAccessFlags2 dst) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    b.srcAccessMask =
        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    b.dstAccessMask = dst;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}
} // namespace
AccelerationStructureManager::~AccelerationStructureManager() {
    destroy();
}
void AccelerationStructureManager::create(VkPhysicalDevice physical, VkDevice device, VmaAllocator allocator) {
    if (!physical || !device || !allocator)
        throw std::invalid_argument("Acceleration structures require Vulkan devices and allocator");
    destroy();
    device_ = device;
    allocator_ = allocator;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR p{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &p;
    vkGetPhysicalDeviceProperties2(physical, &p2);
    scratchAlignment_ = std::max<VkDeviceSize>(p.minAccelerationStructureScratchOffsetAlignment, 1);
    createAccelerationStructure_ = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR"));
    destroyAccelerationStructure_ = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR"));
    getBuildSizes_ = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR"));
    cmdBuild_ = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR"));
    getAddress_ = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
        vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR"));
    // Optional: without them BLASes simply stay uncompacted.
    writeProperties_ = reinterpret_cast<PFN_vkCmdWriteAccelerationStructuresPropertiesKHR>(
        vkGetDeviceProcAddr(device, "vkCmdWriteAccelerationStructuresPropertiesKHR"));
    copyStructure_ = reinterpret_cast<PFN_vkCmdCopyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device, "vkCmdCopyAccelerationStructureKHR"));
    if (!createAccelerationStructure_ || !destroyAccelerationStructure_ || !getBuildSizes_ || !cmdBuild_ ||
        !getAddress_) {
        destroy();
        throw std::runtime_error("VK_KHR_acceleration_structure entry points are unavailable");
    }
}
void AccelerationStructureManager::destroyStructure(Structure& s) noexcept {
    if (s.handle && device_)
        destroyAccelerationStructure_(device_, s.handle, nullptr);
    s.storage.destroy();
    s = {};
}
void AccelerationStructureManager::releaseStructures() {
    if (!device_ || (blases_.empty() && std::ranges::none_of(frames_, [](const auto& f) { return f.tlas.handle; })))
        return;
    if (vkDeviceWaitIdle(device_) != VK_SUCCESS)
        throw std::runtime_error("Could not idle device before releasing acceleration structures");
    for (auto& [key, s] : blases_) {
        static_cast<void>(key);
        destroyStructure(s);
    }
    blases_.clear();
    for (auto& f : frames_) {
        destroyStructure(f.tlas);
        f.instances.destroy();
        f.scratch.destroy();
        f = {};
    }
    blasScratch_.destroy();
    resetCompaction();
}

void AccelerationStructureManager::destroy() noexcept {
    for (auto& [key, s] : blases_) {
        static_cast<void>(key);
        destroyStructure(s);
    }
    blases_.clear();
    for (auto& f : frames_) {
        destroyStructure(f.tlas);
        f.instances.destroy();
        f.scratch.destroy();
        f = {};
    }
    blasScratch_.destroy();
    resetCompaction();
    if (compactionQueries_ != VK_NULL_HANDLE && device_)
        vkDestroyQueryPool(device_, compactionQueries_, nullptr);
    compactionQueries_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    allocator_ = VK_NULL_HANDLE;
    scratchAlignment_ = 1;
    createAccelerationStructure_ = nullptr;
    destroyAccelerationStructure_ = nullptr;
    getBuildSizes_ = nullptr;
    cmdBuild_ = nullptr;
    getAddress_ = nullptr;
    writeProperties_ = nullptr;
    copyStructure_ = nullptr;
}

// Callers have idled the device (rebuild, release, destroy).
void AccelerationStructureManager::resetCompaction() noexcept {
    for (RetiredStructure& old : retired_)
        destroyStructure(old.structure);
    retired_.clear();
    compactionKeys_.clear();
    compactionAge_ = 0;
}

void AccelerationStructureManager::advanceCompaction(const VkCommandBuffer cmd) {
    ++compactionClock_;
    // Originals stay alive until every frame recorded with them has retired.
    std::erase_if(retired_, [&](RetiredStructure& old) {
        if (old.releaseAt > compactionClock_)
            return false;
        destroyStructure(old.structure);
        return true;
    });
    if (compactionKeys_.empty())
        return;
    // The build's frame must have completed; the results are then read
    // without waiting, and a later frame retries if they are not yet there.
    if (++compactionAge_ <= FramesInFlight + 1)
        return;
    std::vector<VkDeviceSize> sizes(compactionKeys_.size());
    const VkResult result = vkGetQueryPoolResults(
        device_, compactionQueries_, 0, static_cast<std::uint32_t>(sizes.size()), sizeof(VkDeviceSize) * sizes.size(),
        sizes.data(), sizeof(VkDeviceSize), VK_QUERY_RESULT_64_BIT);
    if (result == VK_NOT_READY)
        return;
    const std::vector<BlasKey> keys = std::move(compactionKeys_);
    compactionKeys_.clear();
    if (result != VK_SUCCESS)
        return;
    VkDeviceSize before = 0;
    VkDeviceSize after = 0;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        const auto found = blases_.find(keys[index]);
        if (found == blases_.end() || sizes[index] == 0)
            continue;
        Structure& original = found->second;
        before += original.storage.size();
        if (sizes[index] >= original.storage.size()) {
            after += original.storage.size();
            continue;
        }
        Structure compacted;
        createStructure(compacted, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizes[index]);
        VkCopyAccelerationStructureInfoKHR copy{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
        copy.src = original.handle;
        copy.dst = compacted.handle;
        copy.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
        copyStructure_(cmd, &copy);
        after += compacted.storage.size();
        retired_.push_back({std::move(original), compactionClock_ + FramesInFlight + 1});
        original = std::move(compacted);
    }
    // Copies -> the TLAS builds that now reference the compacted BLASes.
    buildBarrier(cmd, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    // An update may not change instance references: rebuild every TLAS.
    for (auto& f : frames_)
        f.tlasBuilt = false;
    builtBlasBytes_ = before;
    compactedBlasBytes_ = after;
    Diagnostics::instance().report(DiagnosticSeverity::Info,
                                   "[RayTracing] Compacted " + std::to_string(keys.size()) + " BLASes: " +
                                       std::to_string(before >> 20U) + " MiB -> " + std::to_string(after >> 20U) +
                                       " MiB",
                                   {.subsystem = "Renderer"});
}
void AccelerationStructureManager::createStructure(Structure& s, VkAccelerationStructureTypeKHR type,
                                                   VkDeviceSize bytes) {
    bytes = std::max<VkDeviceSize>(bytes, 256);
    s.storage.createDeviceLocalEmpty(device_, bytes, Storage, allocator_, true);
    VkAccelerationStructureCreateInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    info.buffer = s.storage.handle();
    info.size = bytes;
    info.type = type;
    if (createAccelerationStructure_(device_, &info, nullptr, &s.handle) != VK_SUCCESS)
        throw std::runtime_error("Could not create acceleration structure");
    VkAccelerationStructureDeviceAddressInfoKHR address{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    address.accelerationStructure = s.handle;
    s.address = getAddress_(device_, &address);
}
VkDeviceAddress AccelerationStructureManager::meshAddress(const BlasKey& key) const noexcept {
    const auto i = blases_.find(key);
    return i == blases_.end() ? 0 : i->second.address;
}
VkDeviceAddress AccelerationStructureManager::ensureScratch(Buffer& buffer, VkDeviceSize requiredSize) {
    if (requiredSize == 0)
        return 0;
    const VkDeviceSize padding = scratchAlignment_ - 1;
    if (requiredSize > std::numeric_limits<VkDeviceSize>::max() - padding)
        throw std::overflow_error("Acceleration structure scratch size overflow");
    const VkDeviceSize allocationSize = requiredSize + padding;
    if (buffer.size() < allocationSize) {
        buffer.destroy();
        buffer.createDeviceLocalEmpty(device_, allocationSize, Scratch, allocator_, true);
    }
    const VkDeviceAddress base = buffer.deviceAddress();
    const VkDeviceAddress alignment = static_cast<VkDeviceAddress>(scratchAlignment_);
    return (base + alignment - 1) & ~(alignment - 1);
}
void AccelerationStructureManager::rebuildBlases(VkCommandBuffer cmd, std::span<const MeshBuildInput> meshes) {
    if (!device_ || !cmd)
        throw std::logic_error("Acceleration structure manager is not initialized");
    if (!blases_.empty() && vkDeviceWaitIdle(device_) != VK_SUCCESS)
        throw std::runtime_error("Could not idle device before rebuilding BLASes");
    for (auto& [key, s] : blases_) {
        static_cast<void>(key);
        destroyStructure(s);
    }
    blases_.clear();
    resetCompaction();
    const bool compaction = writeProperties_ != nullptr && copyStructure_ != nullptr;
    const VkBuildAccelerationStructureFlagsKHR compactionFlag =
        compaction ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR : 0;
    for (auto& f : frames_)
        f.tlasBuilt = false;
    // Inputs sharing a key become geometries of one BLAS. A mesh split into
    // thousands of sections then forms one structure: one TLAS instance per
    // object instead of thousands of overlapping per-section boxes that every
    // ray had to enter.
    std::vector<BlasKey> order;
    std::unordered_map<BlasKey, std::vector<const MeshBuildInput*>, BlasKeyHash> groups;
    for (const auto& m : meshes) {
        if (!m.key.mesh || !m.vertexAddress || !m.indexAddress || !m.vertexCount || m.indexCount < 3)
            continue;
        auto [it, inserted] = groups.try_emplace(m.key);
        if (inserted)
            order.push_back(m.key);
        it->second.push_back(&m);
    }
    struct Pending {
        BlasKey key;
        std::vector<VkAccelerationStructureGeometryKHR> geometries;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        VkAccelerationStructureBuildSizesInfoKHR sizes;
    };
    std::vector<Pending> pending;
    pending.reserve(order.size());
    VkDeviceSize maxScratch{};
    for (const BlasKey& key : order) {
        Pending entry{key, {}, {}, {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR}};
        std::vector<uint32_t> primitiveCounts;
        for (const MeshBuildInput* m : groups.at(key)) {
            VkAccelerationStructureGeometryTrianglesDataKHR t{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
            t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            t.vertexData.deviceAddress = m->vertexAddress;
            t.vertexStride = sizeof(GpuVertex);
            t.maxVertex = m->firstVertex + m->vertexCount - 1;
            t.indexType = VK_INDEX_TYPE_UINT32;
            t.indexData.deviceAddress = m->indexAddress;
            VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            g.geometry.triangles = t;
            entry.geometries.push_back(g);
            entry.ranges.push_back({.primitiveCount = m->indexCount / 3});
            primitiveCounts.push_back(m->indexCount / 3);
        }
        VkAccelerationStructureBuildGeometryInfoKHR b{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        b.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        b.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | compactionFlag;
        b.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        b.geometryCount = static_cast<uint32_t>(entry.geometries.size());
        b.pGeometries = entry.geometries.data();
        getBuildSizes_(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &b, primitiveCounts.data(),
                       &entry.sizes);
        maxScratch = std::max(maxScratch, entry.sizes.buildScratchSize);
        pending.push_back(std::move(entry));
    }
    const VkDeviceAddress scratchAddress = ensureScratch(blasScratch_, maxScratch);
    for (const auto& p : pending) {
        auto& s = blases_[p.key];
        createStructure(s, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, p.sizes.accelerationStructureSize);
        VkAccelerationStructureBuildGeometryInfoKHR b{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        b.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        b.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | compactionFlag;
        b.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        b.geometryCount = static_cast<uint32_t>(p.geometries.size());
        b.pGeometries = p.geometries.data();
        b.dstAccelerationStructure = s.handle;
        b.scratchData.deviceAddress = scratchAddress;
        const VkAccelerationStructureBuildRangeInfoKHR* rs[] = {p.ranges.data()};
        cmdBuild_(cmd, 1, &b, rs);
        buildBarrier(cmd, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                              VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    }
    if (!compaction || pending.empty())
        return;
    // Ask for every compacted size in one query batch after the builds.
    if (compactionQueries_ != VK_NULL_HANDLE)
        vkDestroyQueryPool(device_, compactionQueries_, nullptr);
    compactionQueries_ = VK_NULL_HANDLE;
    VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    queryInfo.queryCount = static_cast<std::uint32_t>(pending.size());
    if (vkCreateQueryPool(device_, &queryInfo, nullptr, &compactionQueries_) != VK_SUCCESS) {
        compactionQueries_ = VK_NULL_HANDLE;
        return;
    }
    std::vector<VkAccelerationStructureKHR> handles;
    handles.reserve(pending.size());
    for (const auto& p : pending) {
        handles.push_back(blases_.at(p.key).handle);
        compactionKeys_.push_back(p.key);
    }
    vkCmdResetQueryPool(cmd, compactionQueries_, 0, queryInfo.queryCount);
    writeProperties_(cmd, queryInfo.queryCount, handles.data(),
                     VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, compactionQueries_, 0);
}
void AccelerationStructureManager::updateTlas(VkCommandBuffer cmd, uint32_t index,
                                              std::span<const InstanceBuildInput> instances) {
    if (!device_ || !cmd)
        return;
    auto& f = frames_.at(index % FramesInFlight);
    if (instances.empty()) {
        f.tlasBuilt = false;
        return;
    }
    std::vector<VkAccelerationStructureInstanceKHR> records;
    records.reserve(instances.size());
    for (const auto& i : instances) {
        const auto address = meshAddress(i.meshKey);
        if (!address)
            continue;
        if (i.customIndex >= (1U << 24U))
            throw std::out_of_range("TLAS instance custom index exceeds 24 bits");
        VkAccelerationStructureInstanceKHR r{};
        std::memcpy(r.transform.matrix, i.transform.data(), sizeof(r.transform.matrix));
        r.instanceCustomIndex = i.customIndex;
        r.mask = i.mask;
        r.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        r.accelerationStructureReference = address;
        records.push_back(r);
    }
    if (records.empty()) {
        f.tlasBuilt = false;
        return;
    }
    const VkDeviceSize bytes = sizeof(VkAccelerationStructureInstanceKHR) * records.size();
    if (f.instances.size() < bytes) {
        f.instances.destroy();
        f.instances.createHostVisible(VK_NULL_HANDLE, device_, bytes, Input, allocator_, true);
        f.instanceCapacity = static_cast<uint32_t>(records.size());
        f.tlasBuilt = false;
    }
    f.instances.update(records.data(), bytes);
    VkAccelerationStructureGeometryInstancesDataKHR data{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    data.data.deviceAddress = f.instances.deviceAddress();
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.geometry.instances = data;
    uint32_t count = static_cast<uint32_t>(records.size());
    VkAccelerationStructureBuildGeometryInfoKHR b{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    b.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    b.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
              VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    b.mode = f.tlasBuilt && count == f.builtInstanceCount ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                                          : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    b.geometryCount = 1;
    b.pGeometries = &g;
    VkAccelerationStructureBuildSizesInfoKHR z{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    getBuildSizes_(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &b, &count, &z);
    if (!f.tlas.handle || f.tlasAllocatedSize < z.accelerationStructureSize) {
        destroyStructure(f.tlas);
        createStructure(f.tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, z.accelerationStructureSize);
        f.tlasAllocatedSize = z.accelerationStructureSize;
        f.tlasBuilt = false;
        b.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    }
    const VkDeviceSize need =
        b.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR ? z.updateScratchSize : z.buildScratchSize;
    const VkDeviceAddress scratchAddress = ensureScratch(f.scratch, need);
    buildBarrier(cmd,
                 VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    b.srcAccelerationStructure =
        b.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR ? f.tlas.handle : VK_NULL_HANDLE;
    b.dstAccelerationStructure = f.tlas.handle;
    b.scratchData.deviceAddress = scratchAddress;
    VkAccelerationStructureBuildRangeInfoKHR r{.primitiveCount = count};
    const VkAccelerationStructureBuildRangeInfoKHR* rs[] = {&r};
    cmdBuild_(cmd, 1, &b, rs);
    f.builtInstanceCount = count;
    f.tlasBuilt = true;
}
VkAccelerationStructureKHR AccelerationStructureManager::tlas(uint32_t index) const noexcept {
    return frames_[index % FramesInFlight].tlas.handle;
}
bool AccelerationStructureManager::ready(uint32_t index) const noexcept {
    return tlas(index) != VK_NULL_HANDLE;
}
bool AccelerationStructureManager::built(uint32_t index) const noexcept {
    return frames_[index % FramesInFlight].tlasBuilt;
}
} // namespace Engine
