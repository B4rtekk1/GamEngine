#include "Engine/Renderer/Lighting/Brixelizer/BrixelizerSystem.h"

#include "Engine/Renderer/Geometry/GpuVertex.h"
#include "Engine/Renderer/Lighting/Brixelizer/BrixelizerResources.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"
#include "Engine/Renderer/Textures/Texture2D.h"
#include "Engine/Renderer/shader_loader.h"

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_brixelizer.h>
#include <FidelityFX/host/ffx_brixelizergi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

float samplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_16spp(
    int pixel_i, int pixel_j, int sampleIndex, int sampleDimension);

namespace Engine {
namespace {
    constexpr float FinestCascadeVoxelSize = 0.2F;
    constexpr float SdfCenterSnap = 4.0F * FinestCascadeVoxelSize;
    constexpr std::uint32_t GiNoiseSize = 128;
    constexpr std::uint32_t GiNoiseFrames = 16;

    PFN_vkVoidFunction VKAPI_PTR getFfxDeviceProcAddr(const VkDevice device, const char* name) {
        if (const auto proc = vkGetDeviceProcAddr(device, name)) return proc;
        if (std::strcmp(name, "vkGetBufferMemoryRequirements2KHR") == 0)
            return vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements2");
        return nullptr;
    }

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
                              const wchar_t* name, const FfxResourceStates state,
                              const std::uint32_t mipLevels = 1,
                              const std::uint32_t arrayLayers = 1) {
        const VkImageCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .flags = arrayLayers == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0U,
            .imageType = type,
            .format = format,
            .extent = extent,
            .mipLevels = mipLevels,
            .arrayLayers = arrayLayers,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                     (state == FFX_RESOURCE_STATE_UNORDERED_ACCESS ? VK_IMAGE_USAGE_STORAGE_BIT : 0U),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        return ffxGetResourceVK(reinterpret_cast<void*>(image),
            ffxGetImageResourceDescriptionVK(image, info,
                state == FFX_RESOURCE_STATE_UNORDERED_ACCESS
                    ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY), name, state);
    }
}

struct BrixelizerSystem::Impl final {
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VmaAllocator allocator{VK_NULL_HANDLE};
    VkExtent2D size{};
    bool limitedGpuMemory{};
    BrixelizerResources resources;
    HdrBuffer debug;
    std::vector<std::byte> backendScratch;
    FfxInterface backend{};
    std::unique_ptr<FfxBrixelizerContext> context;
    std::unique_ptr<FfxBrixelizerGIContext> giContext;
    std::unique_ptr<FfxBrixelizerBakedUpdateDescription> baked;
    std::vector<FfxBrixelizerInstanceID> instanceIds;
    std::vector<BrixelizerSystem::StaticMesh> staticMeshes;
    std::array<std::uint32_t, 2> bufferIndices{};
    VkBuffer registeredVertices{VK_NULL_HANDLE};
    VkBuffer registeredIndices{VK_NULL_HANDLE};
    VkDeviceSize registeredVertexBytes{};
    VkDeviceSize registeredIndexBytes{};
    bool buffersRegistered{};
    bool atlasInitialized{};
    bool debugInitialized{};
    std::deque<HdrBuffer> giNormals;
    std::deque<HdrBuffer> giDepth;
    std::deque<HdrBuffer> giLitHistory;
    std::deque<HdrBuffer> giDiffuse;
    std::deque<HdrBuffer> giSpecular;
    std::vector<bool> giSlotInitialized;
    std::vector<bool> giOutputInitialized;
    std::uint32_t giLastOutputSlot{};
    bool giHistoryValid{};
    glm::mat4 giPreviousView{1.0F};
    glm::mat4 giPreviousProjection{1.0F};
    std::array<Texture2D, GiNoiseFrames> giNoise;
    std::uint32_t giNoiseFrame{};
    VkDescriptorSetLayout giPrepareLayout{VK_NULL_HANDLE};
    VkDescriptorPool giPreparePool{VK_NULL_HANDLE};
    VkPipelineLayout giPreparePipelineLayout{VK_NULL_HANDLE};
    VkPipeline giPreparePipeline{VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> giPrepareSets;

    void clearGeometry() {
        if (!context) return;
        if (!instanceIds.empty()) {
            check(ffxBrixelizerDeleteInstances(context.get(), instanceIds.data(),
                  static_cast<std::uint32_t>(instanceIds.size())), "delete static instances");
            instanceIds.clear();
        }
        staticMeshes.clear();
        if (buffersRegistered) {
            check(ffxBrixelizerUnregisterBuffers(context.get(), bufferIndices.data(),
                  static_cast<std::uint32_t>(bufferIndices.size())), "unregister geometry buffers");
            buffersRegistered = false;
        }
        registeredVertices = VK_NULL_HANDLE;
        registeredIndices = VK_NULL_HANDLE;
    }

    void destroy() noexcept {
        if (giContext) {
            ffxBrixelizerGIContextDestroy(giContext.get());
            giContext.reset();
        }
        if (device) {
            vkDestroyPipeline(device, giPreparePipeline, nullptr);
            vkDestroyPipelineLayout(device, giPreparePipelineLayout, nullptr);
            vkDestroyDescriptorPool(device, giPreparePool, nullptr);
            vkDestroyDescriptorSetLayout(device, giPrepareLayout, nullptr);
        }
        giPreparePipeline = VK_NULL_HANDLE;
        giPreparePipelineLayout = VK_NULL_HANDLE;
        giPreparePool = VK_NULL_HANDLE;
        giPrepareLayout = VK_NULL_HANDLE;
        giPrepareSets.clear();
        giNormals.clear();
        giDepth.clear();
        giLitHistory.clear();
        giDiffuse.clear();
        giSpecular.clear();
        giSlotInitialized.clear();
        giOutputInitialized.clear();
        giHistoryValid = false;
        for (auto& noise : giNoise) noise.destroy();
        giNoiseFrame = 0;
        if (context) {
            // Context teardown releases registrations and all SDK-owned resources.
            ffxBrixelizerContextDestroy(context.get());
            context.reset();
        }
        baked.reset();
        instanceIds.clear();
        staticMeshes.clear();
        buffersRegistered = false;
        debug.destroy();
        resources.scratch.clear();
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
        limitedGpuMemory = false;
        atlasInitialized = false;
        debugInitialized = false;
    }
};

BrixelizerSystem::BrixelizerSystem() : impl_(std::make_unique<Impl>()) {}
BrixelizerSystem::~BrixelizerSystem() { destroy(); }

void BrixelizerSystem::create(const VkPhysicalDevice physicalDevice, const VkDevice device,
                              const VmaAllocator allocator, const VkExtent2D extent,
                              const std::uint32_t framesInFlight,
                              const VkCommandPool commandPool, const VkQueue queue,
                              Assets::AssetManager& assets) {
    destroy();
    if (!physicalDevice || !device || !allocator || !extent.width || !extent.height || !framesInFlight)
        throw std::invalid_argument("Brixelizer needs a device, allocator, extent and frame slots");
    auto& state = *impl_;
    state.physicalDevice = physicalDevice;
    state.device = device;
    state.allocator = allocator;
    state.size = extent;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    constexpr VkDeviceSize GiB = VkDeviceSize{1} << 30U;
    for (std::uint32_t heap = 0; heap < memoryProperties.memoryHeapCount; ++heap) {
        const auto& memoryHeap = memoryProperties.memoryHeaps[heap];
        if ((memoryHeap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0U &&
            memoryHeap.size <= 6U * GiB) {
            state.limitedGpuMemory = true;
            break;
        }
    }
    state.resources.scratch.resize(framesInFlight);
    try {
        if (!getFfxDeviceProcAddr(device, "vkGetBufferMemoryRequirements2KHR"))
            throw std::runtime_error("Brixelizer requires vkGetBufferMemoryRequirements2");
        constexpr size_t maxContexts = 2;
        state.backendScratch.resize(ffxGetScratchMemorySizeVK(physicalDevice, maxContexts));
        VkDeviceContext deviceContext{device, physicalDevice, getFfxDeviceProcAddr};
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
        float voxelSize = FinestCascadeVoxelSize;
        for (auto& cascade : description.cascadeDescs) {
            cascade.flags = FFX_BRIXELIZER_CASCADE_STATIC;
            cascade.voxelSize = voxelSize;
            voxelSize *= 2.0F;
        }
        check(ffxBrixelizerContextCreate(&description, state.context.get()), "create context");

        state.giContext = std::make_unique<FfxBrixelizerGIContext>();
        FfxBrixelizerGIContextDescription giDescription{};
        giDescription.flags = FFX_BRIXELIZER_GI_FLAG_DISABLE_SPECULAR;
        giDescription.internalResolution = FFX_BRIXELIZER_GI_INTERNAL_RESOLUTION_50_PERCENT;
        giDescription.displaySize = {extent.width, extent.height};
        giDescription.backendInterface = state.backend;
        check(ffxBrixelizerGIContextCreate(state.giContext.get(), &giDescription), "create GI context");

        state.giNormals.resize(framesInFlight);
        state.giDepth.resize(framesInFlight);
        state.giLitHistory.resize(framesInFlight);
        state.giDiffuse.resize(framesInFlight);
        state.giSpecular.resize(framesInFlight);
        state.giSlotInitialized.resize(framesInFlight, false);
        state.giOutputInitialized.resize(framesInFlight, false);
        for (std::uint32_t i = 0; i < framesInFlight; ++i) {
            state.giNormals[i].create(physicalDevice, device, extent, allocator, VK_FILTER_NEAREST,
                                      VK_FORMAT_R16G16B16A16_SFLOAT, true);
            state.giDepth[i].create(physicalDevice, device, extent, allocator, VK_FILTER_NEAREST,
                                    VK_FORMAT_R32_SFLOAT, true);
            state.giLitHistory[i].create(physicalDevice, device, extent, allocator, VK_FILTER_NEAREST,
                                         HdrBuffer::Format);
            state.giDiffuse[i].create(physicalDevice, device, extent, allocator, VK_FILTER_LINEAR,
                                      HdrBuffer::Format, true);
            state.giSpecular[i].create(physicalDevice, device, extent, allocator, VK_FILTER_LINEAR,
                                       HdrBuffer::Format, true);
        }
        std::array<std::uint8_t, GiNoiseSize * GiNoiseSize * 4> noise{};
        for (std::uint32_t frame = 0; frame < GiNoiseFrames; ++frame) {
            for (std::uint32_t y = 0; y < GiNoiseSize; ++y) {
                for (std::uint32_t x = 0; x < GiNoiseSize; ++x) {
                    const auto offset = (y * GiNoiseSize + x) * 4;
                    for (int dimension = 0; dimension < 2; ++dimension) {
                        const float sample = samplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_16spp(
                            static_cast<int>(x), static_cast<int>(y), static_cast<int>(frame), dimension);
                        noise[offset + dimension] = static_cast<std::uint8_t>(sample * 256.0F);
                    }
                    noise[offset + 2] = 0;
                    noise[offset + 3] = 255;
                }
            }
            state.giNoise[frame].create(physicalDevice, device, commandPool, queue,
                                        GiNoiseSize, GiNoiseSize, noise,
                                        TextureColorSpace::Linear, false, allocator);
        }
        const std::array<VkDescriptorSetLayoutBinding, 4> giBindings{{
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        VkDescriptorSetLayoutCreateInfo giLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        giLayoutInfo.bindingCount = static_cast<std::uint32_t>(giBindings.size());
        giLayoutInfo.pBindings = giBindings.data();
        if (vkCreateDescriptorSetLayout(device, &giLayoutInfo, nullptr, &state.giPrepareLayout) != VK_SUCCESS)
            throw std::runtime_error("Brixelizer: create GI input layout");
        const std::array<VkDescriptorPoolSize, 2> giPoolSizes{{
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, framesInFlight * 2U},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, framesInFlight * 2U}}};
        VkDescriptorPoolCreateInfo giPoolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        giPoolInfo.maxSets = framesInFlight;
        giPoolInfo.poolSizeCount = static_cast<std::uint32_t>(giPoolSizes.size());
        giPoolInfo.pPoolSizes = giPoolSizes.data();
        if (vkCreateDescriptorPool(device, &giPoolInfo, nullptr, &state.giPreparePool) != VK_SUCCESS)
            throw std::runtime_error("Brixelizer: create GI input descriptor pool");
        std::vector<VkDescriptorSetLayout> layouts(framesInFlight, state.giPrepareLayout);
        state.giPrepareSets.resize(framesInFlight);
        VkDescriptorSetAllocateInfo giSetsInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        giSetsInfo.descriptorPool = state.giPreparePool;
        giSetsInfo.descriptorSetCount = framesInFlight;
        giSetsInfo.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(device, &giSetsInfo, state.giPrepareSets.data()) != VK_SUCCESS)
            throw std::runtime_error("Brixelizer: allocate GI input descriptors");
        const VkPushConstantRange giPush{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(glm::mat4)};
        VkPipelineLayoutCreateInfo giPipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        giPipelineLayoutInfo.setLayoutCount = 1;
        giPipelineLayoutInfo.pSetLayouts = &state.giPrepareLayout;
        giPipelineLayoutInfo.pushConstantRangeCount = 1;
        giPipelineLayoutInfo.pPushConstantRanges = &giPush;
        if (vkCreatePipelineLayout(device, &giPipelineLayoutInfo, nullptr,
                                   &state.giPreparePipelineLayout) != VK_SUCCESS)
            throw std::runtime_error("Brixelizer: create GI input pipeline layout");
        const auto giShader = Vkutil::loadShaderModule(device, assets, "shaders/brixelizer_gi_prepare.spv");
        VkComputePipelineCreateInfo giPipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        giPipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                VK_SHADER_STAGE_COMPUTE_BIT, giShader.get(), "main", nullptr};
        giPipelineInfo.layout = state.giPreparePipelineLayout;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &giPipelineInfo,
                                     nullptr, &state.giPreparePipeline) != VK_SUCCESS)
            throw std::runtime_error("Brixelizer: create GI input pipeline");
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
    if (impl_->giContext) {
        check(ffxBrixelizerGIContextDestroy(impl_->giContext.get()), "destroy resized GI context");
        FfxBrixelizerGIContextDescription giDescription{};
        giDescription.flags = FFX_BRIXELIZER_GI_FLAG_DISABLE_SPECULAR;
        giDescription.internalResolution = FFX_BRIXELIZER_GI_INTERNAL_RESOLUTION_50_PERCENT;
        giDescription.displaySize = {extent.width, extent.height};
        giDescription.backendInterface = impl_->backend;
        check(ffxBrixelizerGIContextCreate(impl_->giContext.get(), &giDescription),
              "recreate resized GI context");
    }
    for (std::size_t i = 0; i < impl_->giNormals.size(); ++i) {
        impl_->giNormals[i].create(impl_->physicalDevice, impl_->device, extent, impl_->allocator,
                                   VK_FILTER_NEAREST, VK_FORMAT_R16G16B16A16_SFLOAT, true);
        impl_->giDepth[i].create(impl_->physicalDevice, impl_->device, extent, impl_->allocator,
                                 VK_FILTER_NEAREST, VK_FORMAT_R32_SFLOAT, true);
        impl_->giLitHistory[i].create(impl_->physicalDevice, impl_->device, extent, impl_->allocator,
                                      VK_FILTER_NEAREST, HdrBuffer::Format);
        impl_->giDiffuse[i].create(impl_->physicalDevice, impl_->device, extent, impl_->allocator,
                                   VK_FILTER_LINEAR, HdrBuffer::Format, true);
        impl_->giSpecular[i].create(impl_->physicalDevice, impl_->device, extent, impl_->allocator,
                                    VK_FILTER_LINEAR, HdrBuffer::Format, true);
    }
    std::fill(impl_->giSlotInitialized.begin(), impl_->giSlotInitialized.end(), false);
    std::fill(impl_->giOutputInitialized.begin(), impl_->giOutputInitialized.end(), false);
    impl_->giHistoryValid = false;
    impl_->size = extent;
    impl_->debugInitialized = false;
}

void BrixelizerSystem::setStaticMeshes(const VkBuffer vertices, const VkDeviceSize vertexBytes,
                                       const VkBuffer indices, const VkDeviceSize indexBytes,
                                       const std::span<const StaticMesh> meshes) {
    if (!ready()) return;
    auto& state = *impl_;
    const auto makeInstance = [&](const StaticMesh& mesh) {
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
        return instance;
    };
    const auto sameGeometry = [&](const StaticMesh& left, const StaticMesh& right) {
        return left.firstVertex == right.firstVertex && left.vertexCount == right.vertexCount &&
               left.firstIndex == right.firstIndex && left.indexCount == right.indexCount;
    };
    const bool sameBuffers = state.buffersRegistered && state.registeredVertices == vertices &&
        state.registeredIndices == indices && state.registeredVertexBytes == vertexBytes &&
        state.registeredIndexBytes == indexBytes;
    const bool sameMeshLayout = sameBuffers && state.staticMeshes.size() == meshes.size() &&
        std::equal(meshes.begin(), meshes.end(), state.staticMeshes.begin(), sameGeometry);
    if (sameMeshLayout) {
        const auto changed = [](const StaticMesh& left, const StaticMesh& right) {
            return std::memcmp(&left.transform, &right.transform, sizeof(glm::mat4)) != 0 ||
                   left.worldBounds.min.x() != right.worldBounds.min.x() ||
                   left.worldBounds.min.y() != right.worldBounds.min.y() ||
                   left.worldBounds.min.z() != right.worldBounds.min.z() ||
                   left.worldBounds.max.x() != right.worldBounds.max.x() ||
                   left.worldBounds.max.y() != right.worldBounds.max.y() ||
                   left.worldBounds.max.z() != right.worldBounds.max.z();
        };
        for (std::size_t i = 0; i < meshes.size(); ++i) {
            if (!changed(meshes[i], state.staticMeshes[i])) continue;
            check(ffxBrixelizerDeleteInstances(state.context.get(), &state.instanceIds[i], 1),
                  "delete moved instance");
            FfxBrixelizerInstanceDescription instance = makeInstance(meshes[i]);
            instance.outInstanceID = &state.instanceIds[i];
            check(ffxBrixelizerCreateInstances(state.context.get(), &instance, 1),
                  "create moved instance");
            state.staticMeshes[i] = meshes[i];
        }
        return;
    }
    state.clearGeometry();
    state.giHistoryValid = false;
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

    std::vector<FfxBrixelizerInstanceDescription> instances;
    instances.reserve(meshes.size());
    for (const StaticMesh& mesh : meshes) {
        if (mesh.indexCount < 3 || mesh.vertexCount == 0 ||
            mesh.firstIndex >= indexBytes / sizeof(std::uint32_t) ||
            mesh.indexCount > indexBytes / sizeof(std::uint32_t) - mesh.firstIndex)
            continue;
        instances.push_back(makeInstance(mesh));
        state.staticMeshes.push_back(mesh);
    }
    if (!instances.empty()) {
        state.instanceIds.resize(instances.size(), FFX_BRIXELIZER_INVALID_ID);
        for (std::size_t i = 0; i < instances.size(); ++i)
            instances[i].outInstanceID = &state.instanceIds[i];
        check(ffxBrixelizerCreateInstances(state.context.get(), instances.data(),
              static_cast<std::uint32_t>(instances.size())), "create static instances");
    }
}

void BrixelizerSystem::update(const VkCommandBuffer commandBuffer, const float cameraPosition[3],
                              const std::uint32_t frameIndex, const std::uint32_t frameSlot,
                              const DebugView debugView,
                              const glm::mat4& inverseView, const glm::mat4& inverseProjection) {
    if (!ready() || !impl_->buffersRegistered || impl_->instanceIds.empty()) return;
    auto& state = *impl_;
    if (frameSlot >= state.resources.scratch.size())
        throw std::out_of_range("Brixelizer: invalid frame slot");
    FfxBrixelizerUpdateDescription update{};
    update.frameIndex = frameIndex;
    // The SDK snaps each cascade to voxels; this coarser grid reduces SDF scrolling.
    for (std::size_t axis = 0; axis < 3; ++axis)
        update.sdfCenter[axis] = std::floor(cameraPosition[axis] / SdfCenterSnap) * SdfCenterSnap;
    // The SDK sample's scratch settings need roughly 851 MiB per frame slot.
    // Keep a smaller working set on devices with limited local memory.
    update.maxReferences = state.limitedGpuMemory ? 4U << 20U : 32U << 20U;
    update.triangleSwapSize = state.limitedGpuMemory ? 64U << 20U : 300U << 20U;
    update.maxBricksPerBake = state.limitedGpuMemory ? 1U << 12U : 1U << 14U;
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
    Buffer& scratchBuffer = state.resources.scratch[frameSlot];
    if (requiredScratch > scratchBuffer.size()) {
        scratchBuffer.createDeviceLocalEmpty(state.device, requiredScratch,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, state.allocator);
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
    const FfxResource scratch = bufferResource(scratchBuffer.handle(),
        scratchBuffer.size(), L"Brixelizer scratch", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    check(ffxBrixelizerUpdate(state.context.get(), state.baked.get(), scratch,
          ffxGetCommandListVK(commandBuffer)), "record SDF update");
}

void BrixelizerSystem::dispatchGI(const VkCommandBuffer commandBuffer,
                                  const std::uint32_t frameSlot,
                                  const VkImageView depthView, const VkSampler depthSampler,
                                  const VkImageView viewNormalView, const VkSampler viewNormalSampler,
                                  const VkImage velocity, const VkImage litImage,
                                  const VkImage environmentImage, const std::uint32_t environmentSize,
                                  const std::uint32_t environmentMipLevels,
                                  const glm::mat4& view, const glm::mat4& projection,
                                  const glm::mat4& inverseView,
                                  const glm::vec3& cameraPosition) {
    if (!ready() || !hasStaticMeshes() || !impl_->giContext ||
        frameSlot >= impl_->giNormals.size()) return;
    auto& state = *impl_;
    const VkExtent3D extent{state.size.width, state.size.height, 1};
    const auto transition = [&](const VkImage image, const VkImageLayout oldLayout,
                                const VkImageLayout newLayout,
                                const VkPipelineStageFlags2 srcStage, const VkAccessFlags2 srcAccess,
                                const VkPipelineStageFlags2 dstStage, const VkAccessFlags2 dstAccess) {
        const VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = srcStage, .srcAccessMask = srcAccess,
            .dstStageMask = dstStage, .dstAccessMask = dstAccess,
            .oldLayout = oldLayout, .newLayout = newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    };
    const bool slotInitialized = state.giSlotInitialized[frameSlot];
    for (const VkImage image : {state.giNormals[frameSlot].image(), state.giDepth[frameSlot].image()})
        transition(image, slotInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_GENERAL,
                   slotInitialized ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                   slotInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    const std::array<VkDescriptorImageInfo, 4> images{{
        {depthSampler, depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        {viewNormalSampler, viewNormalView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, state.giNormals[frameSlot].imageView(), VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, state.giDepth[frameSlot].imageView(), VK_IMAGE_LAYOUT_GENERAL}}};
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (std::uint32_t binding = 0; binding < writes.size(); ++binding)
        writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            state.giPrepareSets[frameSlot], binding, 0, 1,
            binding < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            &images[binding], nullptr, nullptr};
    vkUpdateDescriptorSets(state.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.giPreparePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            state.giPreparePipelineLayout, 0, 1,
                            &state.giPrepareSets[frameSlot], 0, nullptr);
    vkCmdPushConstants(commandBuffer, state.giPreparePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                        0, sizeof(inverseView), &inverseView);
    vkCmdDispatch(commandBuffer, (extent.width + 7U) / 8U, (extent.height + 7U) / 8U, 1);
    for (const VkImage image : {state.giNormals[frameSlot].image(), state.giDepth[frameSlot].image()})
        transition(image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    transition(litImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    transition(state.giLitHistory[frameSlot].image(),
               slotInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               slotInitialized ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
               slotInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE,
               VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkImageCopy copy{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                           {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, extent};
    vkCmdCopyImage(commandBuffer, litImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   state.giLitHistory[frameSlot].image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    transition(litImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    transition(state.giLitHistory[frameSlot].image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    const bool outputInitialized = state.giOutputInitialized[frameSlot];
    for (const VkImage image : {state.giDiffuse[frameSlot].image(), state.giSpecular[frameSlot].image()})
        transition(image, outputInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_GENERAL,
                   outputInitialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                   outputInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const VkMemoryBarrier2 sdfBarrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    const VkDependencyInfo sdfDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &sdfBarrier};
    vkCmdPipelineBarrier2(commandBuffer, &sdfDependency);
    transition(state.resources.sdfAtlas, VK_IMAGE_LAYOUT_GENERAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    const std::uint32_t historySlot = state.giHistoryValid ? state.giLastOutputSlot : frameSlot;
    FfxBrixelizerGIDispatchDescription gi{};
    std::memcpy(&gi.view, &view, sizeof(gi.view));
    std::memcpy(&gi.projection, &projection, sizeof(gi.projection));
    std::memcpy(&gi.prevView, state.giHistoryValid ? &state.giPreviousView : &view, sizeof(gi.prevView));
    std::memcpy(&gi.prevProjection, state.giHistoryValid ? &state.giPreviousProjection : &projection,
                sizeof(gi.prevProjection));
    std::memcpy(&gi.cameraPosition, &cameraPosition, sizeof(gi.cameraPosition));
    gi.startCascade = 0;
    gi.endCascade = BrixelizerResources::CascadeCount - 1;
    gi.rayPushoff = 0.1F;
    gi.sdfSolveEps = 0.01F;
    gi.specularRayPushoff = 0.1F;
    gi.specularSDFSolveEps = 0.01F;
    gi.tMin = 0.01F;
    gi.tMax = 100.0F;
    gi.normalsUnpackMul = 2.0F;
    gi.normalsUnpackAdd = -1.0F;
    gi.isRoughnessPerceptual = true;
    gi.roughnessChannel = 3;
    gi.roughnessThreshold = 0.9F;
    gi.environmentMapIntensity = 1.0F;
    // Engine velocity is current UV minus previous UV; GI adds it to current UV.
    gi.motionVectorScale = {-1.0F, -1.0F};
    const auto sampled2D = [&](const VkImage image, const VkFormat format,
                               const wchar_t* name) {
        return imageResource(image, VK_IMAGE_TYPE_2D, format, extent, name, FFX_RESOURCE_STATE_COMPUTE_READ);
    };
    gi.environmentMap = imageResource(environmentImage, VK_IMAGE_TYPE_2D,
        VK_FORMAT_R16G16B16A16_SFLOAT, {environmentSize, environmentSize, 1},
        L"Environment", FFX_RESOURCE_STATE_COMPUTE_READ, environmentMipLevels, 6);
    gi.prevLitOutput = sampled2D(state.giLitHistory[historySlot].image(), HdrBuffer::Format,
                                 L"Previous lit output");
    gi.depth = sampled2D(state.giDepth[frameSlot].image(), VK_FORMAT_R32_SFLOAT, L"GI depth");
    gi.historyDepth = sampled2D(state.giDepth[historySlot].image(), VK_FORMAT_R32_SFLOAT,
                                L"GI history depth");
    gi.normal = sampled2D(state.giNormals[frameSlot].image(), VK_FORMAT_R16G16B16A16_SFLOAT,
                          L"GI world normals");
    gi.historyNormal = sampled2D(state.giNormals[historySlot].image(), VK_FORMAT_R16G16B16A16_SFLOAT,
                                 L"GI history normals");
    gi.roughness = gi.normal;
    gi.motionVectors = sampled2D(velocity, VK_FORMAT_R16G16_SFLOAT, L"GI motion vectors");
    gi.noiseTexture = imageResource(state.giNoise[state.giNoiseFrame].image(), VK_IMAGE_TYPE_2D,
        VK_FORMAT_R8G8B8A8_UNORM, {GiNoiseSize, GiNoiseSize, 1},
        L"GI noise", FFX_RESOURCE_STATE_COMPUTE_READ);
    gi.sdfAtlas = imageResource(state.resources.sdfAtlas, VK_IMAGE_TYPE_3D,
        VK_FORMAT_R8_UNORM, {512, 512, 512}, L"GI SDF atlas", FFX_RESOURCE_STATE_COMPUTE_READ);
    gi.bricksAABBs = bufferResource(state.resources.brickAabbs.handle(),
        state.resources.brickAabbs.size(), L"GI brick AABBs", FFX_RESOURCE_STATE_COMPUTE_READ);
    for (std::size_t i = 0; i < state.resources.cascades.size(); ++i) {
        gi.cascadeAABBTrees[i] = bufferResource(state.resources.cascades[i].aabbTree.handle(),
            state.resources.cascades[i].aabbTree.size(), L"GI cascade tree", FFX_RESOURCE_STATE_COMPUTE_READ);
        gi.cascadeBrickMaps[i] = bufferResource(state.resources.cascades[i].brickMap.handle(),
            state.resources.cascades[i].brickMap.size(), L"GI cascade map", FFX_RESOURCE_STATE_COMPUTE_READ);
    }
    gi.outputDiffuseGI = imageResource(state.giDiffuse[frameSlot].image(), VK_IMAGE_TYPE_2D,
        HdrBuffer::Format, extent, L"Diffuse GI", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    gi.outputSpecularGI = imageResource(state.giSpecular[frameSlot].image(), VK_IMAGE_TYPE_2D,
        HdrBuffer::Format, extent, L"Specular GI", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    check(ffxBrixelizerGetRawContext(state.context.get(), &gi.brixelizerContext),
          "get raw SDF context");
    check(ffxBrixelizerGIContextDispatch(state.giContext.get(), &gi,
          ffxGetCommandListVK(commandBuffer)), "dispatch GI");
    state.giNoiseFrame = (state.giNoiseFrame + 1) % GiNoiseFrames;
    transition(state.resources.sdfAtlas, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_IMAGE_LAYOUT_GENERAL,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    for (const VkImage image : {state.giDiffuse[frameSlot].image(), state.giSpecular[frameSlot].image()})
        transition(image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    state.giSlotInitialized[frameSlot] = true;
    state.giOutputInitialized[frameSlot] = true;
    state.giLastOutputSlot = frameSlot;
    state.giPreviousView = view;
    state.giPreviousProjection = projection;
    state.giHistoryValid = true;
}

VkDescriptorImageInfo BrixelizerSystem::giDiffuseDescriptor(const std::uint32_t frameSlot) const noexcept {
    if (!impl_ || frameSlot >= impl_->giDiffuse.size()) return {};
    return {impl_->giDiffuse[frameSlot].sampler(), impl_->giDiffuse[frameSlot].imageView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

bool BrixelizerSystem::giHasHistory() const noexcept { return impl_ && impl_->giHistoryValid; }
std::uint32_t BrixelizerSystem::giLatestOutputSlot() const noexcept {
    return impl_ ? impl_->giLastOutputSlot : 0;
}
void BrixelizerSystem::invalidateGI() noexcept {
    if (impl_) impl_->giHistoryValid = false;
}

bool BrixelizerSystem::ready() const noexcept { return impl_ && impl_->context != nullptr; }
bool BrixelizerSystem::hasStaticMeshes() const noexcept { return ready() && !impl_->instanceIds.empty(); }
VkImage BrixelizerSystem::debugImage() const noexcept { return impl_ ? impl_->debug.image() : VK_NULL_HANDLE; }
VkExtent2D BrixelizerSystem::extent() const noexcept { return impl_ ? impl_->size : VkExtent2D{}; }

} // namespace Engine
