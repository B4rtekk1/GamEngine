#include "Engine/Renderer/RayTracing/AccelerationStructureManager.h"
#include "Engine/Renderer/Geometry/GpuVertex.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace Engine {
namespace {
constexpr VkBufferUsageFlags Storage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
constexpr VkBufferUsageFlags Scratch = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
constexpr VkBufferUsageFlags Input = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
void buildBarrier(VkCommandBuffer cmd, VkAccessFlags2 dst) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    b.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR; b.dstAccessMask = dst;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount = 1; d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}
}
AccelerationStructureManager::~AccelerationStructureManager() { destroy(); }
void AccelerationStructureManager::create(VkPhysicalDevice physical, VkDevice device, VmaAllocator allocator) {
    if (!physical || !device || !allocator) throw std::invalid_argument("Acceleration structures require Vulkan devices and allocator");
    destroy(); device_ = device; allocator_ = allocator;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; p2.pNext = &p; vkGetPhysicalDeviceProperties2(physical, &p2);
    scratchAlignment_ = std::max<VkDeviceSize>(p.minAccelerationStructureScratchOffsetAlignment, 1);
    createAccelerationStructure_ = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR"));
    destroyAccelerationStructure_ = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR"));
    getBuildSizes_ = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR"));
    cmdBuild_ = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR"));
    getAddress_ = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR"));
    if (!createAccelerationStructure_ || !destroyAccelerationStructure_ || !getBuildSizes_ || !cmdBuild_ || !getAddress_) { destroy(); throw std::runtime_error("VK_KHR_acceleration_structure entry points are unavailable"); }
}
void AccelerationStructureManager::destroyStructure(Structure& s) noexcept { if (s.handle && device_) destroyAccelerationStructure_(device_, s.handle, nullptr); s.storage.destroy(); s = {}; }
void AccelerationStructureManager::destroy() noexcept {
    for (auto& [key, s] : blases_) { static_cast<void>(key); destroyStructure(s); } blases_.clear();
    for (auto& f : frames_) { destroyStructure(f.tlas); f.instances.destroy(); f.scratch.destroy(); f = {}; } blasScratch_.destroy();
    device_ = VK_NULL_HANDLE; allocator_ = VK_NULL_HANDLE; scratchAlignment_ = 1; createAccelerationStructure_ = nullptr; destroyAccelerationStructure_ = nullptr; getBuildSizes_ = nullptr; cmdBuild_ = nullptr; getAddress_ = nullptr;
}
void AccelerationStructureManager::createStructure(Structure& s, VkAccelerationStructureTypeKHR type, VkDeviceSize bytes) {
    bytes = std::max<VkDeviceSize>(bytes, 256); s.storage.createDeviceLocalEmpty(device_, bytes, Storage, allocator_, true);
    VkAccelerationStructureCreateInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR}; info.buffer = s.storage.handle(); info.size = bytes; info.type = type;
    if (createAccelerationStructure_(device_, &info, nullptr, &s.handle) != VK_SUCCESS) throw std::runtime_error("Could not create acceleration structure");
    VkAccelerationStructureDeviceAddressInfoKHR address{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR}; address.accelerationStructure = s.handle; s.address = getAddress_(device_, &address);
}
VkDeviceAddress AccelerationStructureManager::meshAddress(const BlasKey& key) const noexcept { const auto i = blases_.find(key); return i == blases_.end() ? 0 : i->second.address; }
VkDeviceAddress AccelerationStructureManager::ensureScratch(Buffer& buffer, VkDeviceSize requiredSize) {
    if (requiredSize == 0) return 0;
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
    if (!device_ || !cmd) throw std::logic_error("Acceleration structure manager is not initialized");
    if (!blases_.empty() && vkDeviceWaitIdle(device_) != VK_SUCCESS) throw std::runtime_error("Could not idle device before rebuilding BLASes");
    for (auto& [key, s] : blases_) { static_cast<void>(key); destroyStructure(s); } blases_.clear(); for (auto& f : frames_) f.tlasBuilt = false;
    struct Pending { const MeshBuildInput* mesh; VkAccelerationStructureGeometryKHR geometry; VkAccelerationStructureBuildSizesInfoKHR sizes; uint32_t primitives; };
    std::vector<Pending> pending; VkDeviceSize maxScratch{};
    for (const auto& m : meshes) { if (!m.key.mesh || !m.vertexAddress || !m.indexAddress || !m.vertexCount || m.indexCount < 3) continue;
        VkAccelerationStructureGeometryTrianglesDataKHR t{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR}; t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT; t.vertexData.deviceAddress = m.vertexAddress; t.vertexStride = sizeof(GpuVertex); t.maxVertex = m.firstVertex + m.vertexCount - 1; t.indexType = VK_INDEX_TYPE_UINT32; t.indexData.deviceAddress = m.indexAddress;
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR}; g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR; g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR; g.geometry.triangles = t; uint32_t pc = m.indexCount / 3;
        VkAccelerationStructureBuildGeometryInfoKHR b{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR}; b.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR; b.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR; b.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; b.geometryCount = 1; b.pGeometries = &g;
        VkAccelerationStructureBuildSizesInfoKHR z{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR}; getBuildSizes_(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &b, &pc, &z); maxScratch = std::max(maxScratch, z.buildScratchSize); pending.push_back({&m, g, z, pc}); }
    const VkDeviceAddress scratchAddress = ensureScratch(blasScratch_, maxScratch);
    for (const auto& p : pending) { auto& s = blases_[p.mesh->key]; createStructure(s, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, p.sizes.accelerationStructureSize);
        VkAccelerationStructureBuildGeometryInfoKHR b{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR}; b.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR; b.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR; b.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; b.geometryCount = 1; b.pGeometries = &p.geometry; b.dstAccelerationStructure = s.handle; b.scratchData.deviceAddress = scratchAddress;
        VkAccelerationStructureBuildRangeInfoKHR r{.primitiveCount = p.primitives}; const VkAccelerationStructureBuildRangeInfoKHR* rs[] = {&r}; cmdBuild_(cmd, 1, &b, rs); buildBarrier(cmd, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR); }
}
void AccelerationStructureManager::updateTlas(VkCommandBuffer cmd, uint32_t index, std::span<const InstanceBuildInput> instances) {
    if (!device_ || !cmd) return;
    auto& f = frames_.at(index % FramesInFlight);
    if (instances.empty()) { f.tlasBuilt = false; return; }
    std::vector<VkAccelerationStructureInstanceKHR> records; records.reserve(instances.size());
    for (const auto& i : instances) { const auto address = meshAddress(i.meshKey); if (!address) continue; if (i.customIndex >= (1U << 24U)) throw std::out_of_range("TLAS instance custom index exceeds 24 bits"); VkAccelerationStructureInstanceKHR r{}; std::memcpy(r.transform.matrix, i.transform.data(), sizeof(r.transform.matrix)); r.instanceCustomIndex = i.customIndex; r.mask = i.mask; r.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR; r.accelerationStructureReference = address; records.push_back(r); } if (records.empty()) { f.tlasBuilt = false; return; }
    const VkDeviceSize bytes = sizeof(VkAccelerationStructureInstanceKHR) * records.size(); if (f.instances.size() < bytes) { f.instances.destroy(); f.instances.createHostVisible(VK_NULL_HANDLE, device_, bytes, Input, allocator_, true); f.instanceCapacity = static_cast<uint32_t>(records.size()); f.tlasBuilt = false; } f.instances.update(records.data(), bytes);
    VkAccelerationStructureGeometryInstancesDataKHR data{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR}; data.data.deviceAddress = f.instances.deviceAddress(); VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR}; g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR; g.geometry.instances = data; uint32_t count = static_cast<uint32_t>(records.size());
    VkAccelerationStructureBuildGeometryInfoKHR b{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR}; b.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; b.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR; b.mode = f.tlasBuilt && count == f.builtInstanceCount ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; b.geometryCount = 1; b.pGeometries = &g; VkAccelerationStructureBuildSizesInfoKHR z{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR}; getBuildSizes_(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &b, &count, &z);
    if (!f.tlas.handle || f.tlasAllocatedSize < z.accelerationStructureSize) { destroyStructure(f.tlas); createStructure(f.tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, z.accelerationStructureSize); f.tlasAllocatedSize = z.accelerationStructureSize; f.tlasBuilt = false; b.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; }
    const VkDeviceSize need = b.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR ? z.updateScratchSize : z.buildScratchSize;
    const VkDeviceAddress scratchAddress = ensureScratch(f.scratch, need);
    buildBarrier(cmd, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR); b.srcAccelerationStructure = b.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR ? f.tlas.handle : VK_NULL_HANDLE; b.dstAccelerationStructure = f.tlas.handle; b.scratchData.deviceAddress = scratchAddress; VkAccelerationStructureBuildRangeInfoKHR r{.primitiveCount = count}; const VkAccelerationStructureBuildRangeInfoKHR* rs[] = {&r}; cmdBuild_(cmd, 1, &b, rs); f.builtInstanceCount = count; f.tlasBuilt = true;
}
VkAccelerationStructureKHR AccelerationStructureManager::tlas(uint32_t index) const noexcept { return frames_[index % FramesInFlight].tlas.handle; }
bool AccelerationStructureManager::ready(uint32_t index) const noexcept { return tlas(index) != VK_NULL_HANDLE; }
bool AccelerationStructureManager::built(uint32_t index) const noexcept {
    return frames_[index % FramesInFlight].tlasBuilt;
}
} // namespace Engine
