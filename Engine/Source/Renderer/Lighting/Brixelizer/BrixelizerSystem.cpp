#include "Engine/Renderer/Lighting/Brixelizer/BrixelizerSystem.h"

#include "Engine/Renderer/Geometry/GpuVertex.h"
#include "Engine/Renderer/Lighting/Brixelizer/BrixelizerResources.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_brixelizer.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Engine {
namespace {
    void check(const FfxErrorCode result, const char* operation) {
        if (result != FFX_OK) throw std::runtime_error(std::string("Brixelizer: ") + operation);
    }

    FfxResource bufferResource(const VkBuffer buffer, const VkDeviceSize size,
                               const wchar_t* name, const FfxResourceStates state) {
        const VkBufferCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        return ffxGetResourceVK(reinterpret_cast<void*>(buffer),
            ffxGetBufferResourceDescriptionVK(buffer, info, FFX_RESOURCE_USAGE_UAV), name, state);
    }

    FfxResource imageResource(const VkImage image, const VkImageType type,
                              const VkFormat format, const VkExtent3D extent,
                              const wchar_t* name, const FfxResourceStates state) {
        const VkImageCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = type,
            .format = format,
            .extent = extent,
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        return ffxGetResourceVK(reinterpret_cast<void*>(image),
            ffxGetImageResourceDescriptionVK(image, info, FFX_RESOURCE_USAGE_UAV), name, state);
    }
}

struct BrixelizerSystem::Impl final {
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VmaAllocator allocator{VK_NULL_HANDLE};
    VkExtent2D size{};
    BrixelizerResources resources;
    HdrBuffer debug;
    std::vector<Buffer> retiredScratch;
    std::vector<std::byte> backendScratch;
    FfxInterface backend{};
    std::unique_ptr<FfxBrixelizerContext> context;
    std::unique_ptr<FfxBrixelizerBakedUpdateDescription> baked;
    std::vector<FfxBrixelizerInstanceID> instanceIds;
    std::array<std::uint32_t, 2> bufferIndices{};
    VkBuffer registeredVertices{VK_NULL_HANDLE};
    VkBuffer registeredIndices{VK_NULL_HANDLE};
    VkDeviceSize registeredVertexBytes{};
    VkDeviceSize registeredIndexBytes{};
    bool buffersRegistered{};
    bool atlasInitialized{};
    bool debugInitialized{};
    std::uint32_t scratchBytes{};

    void clearGeometry() {
        if (!context) return;
        if (!instanceIds.empty()) {
            check(ffxBrixelizerDeleteInstances(context.get(), instanceIds.data(),
                  static_cast<std::uint32_t>(instanceIds.size())), "delete static instances");
            instanceIds.clear();
        }
        if (buffersRegistered) {
            check(ffxBrixelizerUnregisterBuffers(context.get(), bufferIndices.data(),
                  static_cast<std::uint32_t>(bufferIndices.size())), "unregister geometry buffers");
            buffersRegistered = false;
        }
        registeredVertices = VK_NULL_HANDLE;
        registeredIndices = VK_NULL_HANDLE;
    }

    void destroy() noexcept {
        if (context) {
            // Context teardown releases registrations and all SDK-owned resources.
            ffxBrixelizerContextDestroy(context.get());
            context.reset();
        }
        baked.reset();
        instanceIds.clear();
        buffersRegistered = false;
        debug.destroy();
        resources.scratch.destroy();
        retiredScratch.clear();
        resources.brickAabbs.destroy();
        for (auto& cascade : resources.cascades) {
            cascade.aabbTree.destroy();
            cascade.brickMap.destroy();
        }
        if (resources.sdfAtlas != VK_NULL_HANDLE)
            vmaDestroyImage(allocator, resources.sdfAtlas, resources.sdfAtlasAllocation);
        resources.sdfAtlas = VK_NULL_HANDLE;
        resources.sdfAtlasAllocation = VK_NULL_HANDLE;
        backendScratch.clear();
        backend = {};
        device = VK_NULL_HANDLE;
        allocator = VK_NULL_HANDLE;
        size = {};
        scratchBytes = 0;
        atlasInitialized = false;
        debugInitialized = false;
    }
};

BrixelizerSystem::BrixelizerSystem() : impl_(std::make_unique<Impl>()) {}
BrixelizerSystem::~BrixelizerSystem() { destroy(); }

void BrixelizerSystem::create(const VkPhysicalDevice physicalDevice, const VkDevice device,
                              const VmaAllocator allocator, const VkExtent2D extent) {
    destroy();
    if (!physicalDevice || !device || !allocator || !extent.width || !extent.height)
        throw std::invalid_argument("Brixelizer needs a device, allocator and nonzero extent");
    auto& state = *impl_;
    state.physicalDevice = physicalDevice;
    state.device = device;
    state.allocator = allocator;
    state.size = extent;
    try {
        constexpr size_t maxContexts = 1;
        state.backendScratch.resize(ffxGetScratchMemorySizeVK(physicalDevice, maxContexts));
        VkDeviceContext deviceContext{device, physicalDevice, vkGetDeviceProcAddr};
        check(ffxGetInterfaceVK(&state.backend, ffxGetDeviceVK(&deviceContext),
              state.backendScratch.data(), state.backendScratch.size(), maxContexts),
              "initialize Vulkan backend");

        const VkImageCreateInfo atlasInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_3D,
            .format = VK_FORMAT_R8_UNORM,
            .extent = {512, 512, 512},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(allocator, &atlasInfo, &allocation, &state.resources.sdfAtlas,
                           &state.resources.sdfAtlasAllocation, nullptr) != VK_SUCCESS)
            throw std::runtime_error("Brixelizer: allocate 3D SDF atlas");
        const auto makeBuffer = [&](Buffer& buffer, const VkDeviceSize bytes) {
            buffer.createDeviceLocalEmpty(device, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, allocator);
        };
        makeBuffer(state.resources.brickAabbs, FFX_BRIXELIZER_BRICK_AABBS_SIZE);
        for (auto& cascade : state.resources.cascades) {
            makeBuffer(cascade.aabbTree, FFX_BRIXELIZER_CASCADE_AABB_TREE_SIZE);
            makeBuffer(cascade.brickMap, FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE);
        }
        state.debug.create(physicalDevice, device, extent, allocator, VK_FILTER_NEAREST,
                           HdrBuffer::Format, true);

        state.context = std::make_unique<FfxBrixelizerContext>();
        state.baked = std::make_unique<FfxBrixelizerBakedUpdateDescription>();
        FfxBrixelizerContextDescription description{};
        description.numCascades = BrixelizerResources::CascadeCount;
        description.backendInterface = state.backend;
        float voxelSize = 0.2F;
        for (auto& cascade : description.cascadeDescs) {
            cascade.flags = FFX_BRIXELIZER_CASCADE_STATIC;
            cascade.voxelSize = voxelSize;
            voxelSize *= 2.0F;
        }
        check(ffxBrixelizerContextCreate(&description, state.context.get()), "create context");
    } catch (...) {
        state.destroy();
        throw;
    }
}

void BrixelizerSystem::destroy() noexcept { if (impl_) impl_->destroy(); }

void BrixelizerSystem::resize(const VkExtent2D extent) {
    if (!ready() || (extent.width == impl_->size.width && extent.height == impl_->size.height)) return;
    if (!extent.width || !extent.height) throw std::invalid_argument("Brixelizer: zero extent");
    impl_->debug.create(impl_->physicalDevice, impl_->device, extent, impl_->allocator,
                         VK_FILTER_NEAREST, HdrBuffer::Format, true);
    impl_->size = extent;
    impl_->debugInitialized = false;
}

void BrixelizerSystem::setStaticMeshes(const VkBuffer vertices, const VkDeviceSize vertexBytes,
                                       const VkBuffer indices, const VkDeviceSize indexBytes,
                                       const std::span<const StaticMesh> meshes) {
    if (!ready()) return;
    auto& state = *impl_;
    state.clearGeometry();
    if (!vertices || !indices || meshes.empty()) return;
    state.registeredVertices = vertices;
    state.registeredIndices = indices;
    state.registeredVertexBytes = vertexBytes;
    state.registeredIndexBytes = indexBytes;
    std::array<FfxBrixelizerBufferDescription, 2> buffers{
        FfxBrixelizerBufferDescription{bufferResource(vertices, vertexBytes, L"Scene vertices",
                                                        FFX_RESOURCE_STATE_COMPUTE_READ), &state.bufferIndices[0]},
        FfxBrixelizerBufferDescription{bufferResource(indices, indexBytes, L"Scene indices",
                                                        FFX_RESOURCE_STATE_COMPUTE_READ), &state.bufferIndices[1]}};
    check(ffxBrixelizerRegisterBuffers(state.context.get(), buffers.data(),
          static_cast<std::uint32_t>(buffers.size())), "register geometry buffers");
    state.buffersRegistered = true;

    for (const StaticMesh& mesh : meshes) {
        if (mesh.indexCount < 3 || mesh.vertexCount == 0 ||
            mesh.firstIndex >= indexBytes / sizeof(std::uint32_t) ||
            mesh.indexCount > indexBytes / sizeof(std::uint32_t) - mesh.firstIndex)
            continue;
        FfxBrixelizerInstanceDescription instance{};
        instance.maxCascade = BrixelizerResources::CascadeCount - 1;
        instance.aabb = {{mesh.worldBounds.min.x(), mesh.worldBounds.min.y(), mesh.worldBounds.min.z()},
                         {mesh.worldBounds.max.x(), mesh.worldBounds.max.y(), mesh.worldBounds.max.z()}};
        for (std::uint32_t row = 0; row < 3; ++row)
            for (std::uint32_t column = 0; column < 4; ++column)
                instance.transform[row * 4 + column] = mesh.transform[column][row];
        instance.indexFormat = FFX_INDEX_TYPE_UINT32;
        instance.indexBuffer = state.bufferIndices[1];
        instance.indexBufferOffset = mesh.firstIndex * sizeof(std::uint32_t);
        instance.triangleCount = mesh.indexCount / 3;
        instance.vertexBuffer = state.bufferIndices[0];
        instance.vertexStride = sizeof(GpuVertex);
        // GamEngine stores global indices in the shared index heap.
        instance.vertexBufferOffset = 0;
        instance.vertexCount = static_cast<std::uint32_t>(vertexBytes / sizeof(GpuVertex));
        instance.vertexFormat = FFX_SURFACE_FORMAT_R32G32B32_FLOAT;
        FfxBrixelizerInstanceID id = FFX_BRIXELIZER_INVALID_ID;
        instance.outInstanceID = &id;
        check(ffxBrixelizerCreateInstances(state.context.get(), &instance, 1), "create static instance");
        state.instanceIds.push_back(id);
    }
}

void BrixelizerSystem::update(const VkCommandBuffer commandBuffer, const float cameraPosition[3],
                              const std::uint32_t frameIndex, const DebugView debugView,
                              const glm::mat4& inverseView, const glm::mat4& inverseProjection) {
    if (!ready() || !impl_->buffersRegistered || impl_->instanceIds.empty()) return;
    auto& state = *impl_;
    FfxBrixelizerUpdateDescription update{};
    update.frameIndex = frameIndex;
    std::copy_n(cameraPosition, 3, update.sdfCenter);
    update.maxReferences = 32U << 20U;
    update.triangleSwapSize = 300U << 20U;
    update.maxBricksPerBake = 1U << 14U;
    update.resources.sdfAtlas = imageResource(state.resources.sdfAtlas, VK_IMAGE_TYPE_3D,
        VK_FORMAT_R8_UNORM, {512, 512, 512}, L"Brixelizer SDF", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    update.resources.brickAABBs = bufferResource(state.resources.brickAabbs.handle(),
        state.resources.brickAabbs.size(), L"Brixelizer brick AABBs", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    for (std::size_t i = 0; i < state.resources.cascades.size(); ++i) {
        const auto& cascade = state.resources.cascades[i];
        update.resources.cascadeResources[i].aabbTree = bufferResource(cascade.aabbTree.handle(),
            cascade.aabbTree.size(), L"Brixelizer AABB tree", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        update.resources.cascadeResources[i].brickMap = bufferResource(cascade.brickMap.handle(),
            cascade.brickMap.size(), L"Brixelizer brick map", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    FfxBrixelizerDebugVisualizationDescription visualization{};
    if (debugView != DebugView::Off) {
        std::memcpy(visualization.inverseViewMatrix, &inverseView, sizeof(visualization.inverseViewMatrix));
        std::memcpy(visualization.inverseProjectionMatrix, &inverseProjection,
                    sizeof(visualization.inverseProjectionMatrix));
        switch (debugView) {
        case DebugView::Distance: visualization.debugState = FFX_BRIXELIZER_TRACE_DEBUG_MODE_DISTANCE; break;
        case DebugView::Gradient: visualization.debugState = FFX_BRIXELIZER_TRACE_DEBUG_MODE_GRAD; break;
        case DebugView::BrickId: visualization.debugState = FFX_BRIXELIZER_TRACE_DEBUG_MODE_BRICK_ID; break;
        case DebugView::CascadeId: visualization.debugState = FFX_BRIXELIZER_TRACE_DEBUG_MODE_CASCADE_ID; break;
        default: break;
        }
        visualization.endCascadeIndex = BrixelizerResources::CascadeCount - 1;
        visualization.sdfSolveEps = 0.01F;
        visualization.tMin = 0.01F;
        visualization.tMax = 100.0F;
        visualization.renderWidth = state.size.width;
        visualization.renderHeight = state.size.height;
        visualization.output = imageResource(state.debug.image(), VK_IMAGE_TYPE_2D,
            HdrBuffer::Format, {state.size.width, state.size.height, 1}, L"Brixelizer debug",
            FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        visualization.commandList = ffxGetCommandListVK(commandBuffer);
        update.debugVisualizationDesc = &visualization;
    }

    size_t requiredScratch = 0;
    update.outScratchBufferSize = &requiredScratch;
    check(ffxBrixelizerBakeUpdate(state.context.get(), &update, state.baked.get()), "bake update");
    requiredScratch = std::max<std::size_t>(requiredScratch, 4096);
    if (requiredScratch > state.scratchBytes) {
        if (state.resources.scratch.handle() != VK_NULL_HANDLE)
            state.retiredScratch.push_back(std::move(state.resources.scratch));
        state.resources.scratch.createDeviceLocalEmpty(state.device, requiredScratch,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, state.allocator);
        state.scratchBytes = static_cast<std::uint32_t>(requiredScratch);
    }
    VkImageMemoryBarrier2 atlasBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = state.atlasInitialized ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = state.atlasInitialized ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .oldLayout = state.atlasInitialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = state.resources.sdfAtlas,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageMemoryBarrier2 debugBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = state.debugInitialized ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = state.debugInitialized ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .oldLayout = state.debugInitialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = state.debug.image(),
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    const std::array barriers{atlasBarrier, debugBarrier};
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = debugView == DebugView::Off ? 1U : 2U,
        .pImageMemoryBarriers = barriers.data()};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    state.atlasInitialized = true;
    if (debugView != DebugView::Off) state.debugInitialized = true;
    const FfxResource scratch = bufferResource(state.resources.scratch.handle(),
        state.resources.scratch.size(), L"Brixelizer scratch", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    check(ffxBrixelizerUpdate(state.context.get(), state.baked.get(), scratch,
          ffxGetCommandListVK(commandBuffer)), "record SDF update");
}

bool BrixelizerSystem::ready() const noexcept { return impl_ && impl_->context != nullptr; }
bool BrixelizerSystem::hasStaticMeshes() const noexcept { return ready() && !impl_->instanceIds.empty(); }
VkImage BrixelizerSystem::debugImage() const noexcept { return impl_ ? impl_->debug.image() : VK_NULL_HANDLE; }
VkExtent2D BrixelizerSystem::extent() const noexcept { return impl_ ? impl_->size : VkExtent2D{}; }

} // namespace Engine
