#include "Engine/Renderer/RayTracing/AccelerationStructureManager.h"

#include "Engine/Renderer/Geometry/GpuVertex.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace Engine {
    namespace {
        constexpr VkBufferUsageFlags AsStorageUsage =
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        constexpr VkBufferUsageFlags AsScratchUsage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        constexpr VkBufferUsageFlags AsInputUsage =
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        VkDeviceSize alignedScratchSize(const VkDeviceSize size) {
            // The Vulkan minimum is exposed as an AS property; 256 is valid
            // for current desktop drivers and keeps this wrapper standalone.
            return std::max<VkDeviceSize>(256, (size + 255) & ~VkDeviceSize{255});
        }
    }

    AccelerationStructureManager::~AccelerationStructureManager() { destroy(); }

    void AccelerationStructureManager::create(const VkDevice device, const VmaAllocator allocator) {
        if (device == VK_NULL_HANDLE || allocator == VK_NULL_HANDLE)
            throw std::invalid_argument("Acceleration structures require a Vulkan device and allocator");
        destroy();
        device_ = device;
        allocator_ = allocator;
        createAccelerationStructure_ = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
            vkGetDeviceProcAddr(device_, "vkCreateAccelerationStructureKHR"));
        destroyAccelerationStructure_ = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
            vkGetDeviceProcAddr(device_, "vkDestroyAccelerationStructureKHR"));
        getBuildSizes_ = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
            vkGetDeviceProcAddr(device_, "vkGetAccelerationStructureBuildSizesKHR"));
        cmdBuild_ = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
            vkGetDeviceProcAddr(device_, "vkCmdBuildAccelerationStructuresKHR"));
        getAddress_ = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
            vkGetDeviceProcAddr(device_, "vkGetAccelerationStructureDeviceAddressKHR"));
        if (!createAccelerationStructure_ || !destroyAccelerationStructure_ || !getBuildSizes_ ||
            !cmdBuild_ || !getAddress_) {
            destroy();
            throw std::runtime_error("VK_KHR_acceleration_structure entry points are unavailable");
        }
    }

    void AccelerationStructureManager::destroyStructure(Structure& structure) noexcept {
        if (structure.handle != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE)
            destroyAccelerationStructure_(device_, structure.handle, nullptr);
        structure.storage.destroy();
        structure = {};
    }

    void AccelerationStructureManager::destroy() noexcept {
        for (auto& [key, blas] : blases_) {
            static_cast<void>(key);
            destroyStructure(blas);
        }
        blases_.clear();
        destroyStructure(tlas_);
        tlasInstances_.destroy();
        scratch_.destroy();
        tlasCapacity_ = 0;
        tlasBuilt_ = false;
        allocator_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        createAccelerationStructure_ = nullptr;
        destroyAccelerationStructure_ = nullptr;
        getBuildSizes_ = nullptr;
        cmdBuild_ = nullptr;
        getAddress_ = nullptr;
    }

    void AccelerationStructureManager::createStructure(Structure& structure,
                                                        const VkAccelerationStructureTypeKHR type,
                                                        const VkDeviceSize bytes) {
        structure.storage.createDeviceLocalEmpty(device_, std::max<VkDeviceSize>(bytes, 256),
                                                 AsStorageUsage, allocator_, true);
        const VkAccelerationStructureCreateInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = structure.storage.handle(), .size = std::max<VkDeviceSize>(bytes, 256), .type = type};
        if (createAccelerationStructure_(device_, &info, nullptr, &structure.handle) != VK_SUCCESS)
            throw std::runtime_error("Could not create acceleration structure");
        const VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
            .accelerationStructure = structure.handle};
        structure.address = getAddress_(device_, &addressInfo);
    }

    void AccelerationStructureManager::rebuildBlases(const VkCommandBuffer commandBuffer,
                                                      const std::span<const MeshBuildInput> meshes) {
        if (device_ == VK_NULL_HANDLE || commandBuffer == VK_NULL_HANDLE)
            throw std::logic_error("Acceleration structure manager is not initialized");
        for (auto& [key, blas] : blases_) { static_cast<void>(key); destroyStructure(blas); }
        blases_.clear();
        for (const MeshBuildInput& mesh : meshes) {
            if (mesh.key == nullptr || mesh.vertexAddress == 0 || mesh.indexAddress == 0 ||
                mesh.vertexCount == 0 || mesh.indexCount < 3) continue;
            VkAccelerationStructureGeometryTrianglesDataKHR triangles{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, .vertexData = {.deviceAddress = mesh.vertexAddress},
                .vertexStride = sizeof(GpuVertex), .maxVertex = mesh.vertexCount - 1,
                .indexType = VK_INDEX_TYPE_UINT32, .indexData = {.deviceAddress = mesh.indexAddress}};
            VkAccelerationStructureGeometryKHR geometry{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR, .geometry = {.triangles = triangles},
                .flags = VK_GEOMETRY_OPAQUE_BIT_KHR};
            const std::uint32_t primitiveCount = mesh.indexCount / 3;
            VkAccelerationStructureBuildGeometryInfoKHR build{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
                .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                .geometryCount = 1, .pGeometries = &geometry};
            VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            getBuildSizes_(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                           &build, &primitiveCount, &sizes);
            Structure& blas = blases_[mesh.key];
            createStructure(blas, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizes.accelerationStructureSize);
            if (scratch_.size() < alignedScratchSize(sizes.buildScratchSize)) {
                scratch_.destroy();
                scratch_.createDeviceLocalEmpty(device_, alignedScratchSize(sizes.buildScratchSize), AsScratchUsage, allocator_, true);
            }
            build.dstAccelerationStructure = blas.handle;
            build.scratchData.deviceAddress = scratch_.deviceAddress();
            const VkAccelerationStructureBuildRangeInfoKHR range{.primitiveCount = primitiveCount};
            const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = {&range};
            cmdBuild_(commandBuffer, 1, &build, ranges);
        }
        tlasBuilt_ = false;
    }

    VkDeviceAddress AccelerationStructureManager::meshAddress(const void* const key) const noexcept {
        const auto found = blases_.find(key);
        return found == blases_.end() ? 0 : found->second.address;
    }

    void AccelerationStructureManager::updateTlas(const VkCommandBuffer commandBuffer,
                                                   const std::span<const InstanceBuildInput> instances) {
        if (device_ == VK_NULL_HANDLE || commandBuffer == VK_NULL_HANDLE || instances.empty()) return;
        std::vector<VkAccelerationStructureInstanceKHR> records;
        records.reserve(instances.size());
        for (const InstanceBuildInput& input : instances) {
            const VkDeviceAddress address = meshAddress(input.meshKey);
            if (address == 0) continue;
            VkAccelerationStructureInstanceKHR record{};
            std::memcpy(record.transform.matrix, input.transform.data(), sizeof(record.transform.matrix));
            record.instanceCustomIndex = input.customIndex;
            record.mask = input.mask;
            record.instanceShaderBindingTableRecordOffset = 0;
            record.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
                           VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
            record.accelerationStructureReference = address;
            records.push_back(record);
        }
        if (records.empty()) return;
        const VkDeviceSize instanceBytes = sizeof(VkAccelerationStructureInstanceKHR) * records.size();
        if (tlasInstances_.size() < instanceBytes) {
            tlasInstances_.destroy();
            tlasInstances_.createHostVisible(VK_NULL_HANDLE, device_, instanceBytes, AsInputUsage, allocator_, true);
            tlasCapacity_ = static_cast<std::uint32_t>(records.size());
            tlasBuilt_ = false;
        }
        tlasInstances_.update(records.data(), instanceBytes);
        VkAccelerationStructureGeometryInstancesDataKHR instanceData{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
            .arrayOfPointers = VK_FALSE, .data = {.deviceAddress = tlasInstances_.deviceAddress()}};
        VkAccelerationStructureGeometryKHR geometry{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR, .geometry = {.instances = instanceData}};
        const std::uint32_t count = static_cast<std::uint32_t>(records.size());
        VkAccelerationStructureBuildGeometryInfoKHR build{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                     VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
            .mode = tlasBuilt_ && count <= tlasCapacity_ ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR :
                                                          VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .geometryCount = 1, .pGeometries = &geometry};
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        getBuildSizes_(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                       &build, &count, &sizes);
        if (tlas_.handle == VK_NULL_HANDLE) createStructure(tlas_, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, sizes.accelerationStructureSize);
        if (scratch_.size() < alignedScratchSize(std::max(sizes.buildScratchSize, sizes.updateScratchSize))) {
            scratch_.destroy();
            scratch_.createDeviceLocalEmpty(device_, alignedScratchSize(std::max(sizes.buildScratchSize, sizes.updateScratchSize)), AsScratchUsage, allocator_, true);
        }
        build.srcAccelerationStructure = build.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR ? tlas_.handle : VK_NULL_HANDLE;
        build.dstAccelerationStructure = tlas_.handle;
        build.scratchData.deviceAddress = scratch_.deviceAddress();
        const VkAccelerationStructureBuildRangeInfoKHR range{.primitiveCount = count};
        const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = {&range};
        cmdBuild_(commandBuffer, 1, &build, ranges);
        tlasBuilt_ = true;
    }
} // namespace Engine
