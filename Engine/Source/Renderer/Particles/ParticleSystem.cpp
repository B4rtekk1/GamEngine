#include "Engine/Renderer/Particles/ParticleSystem.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace Engine::Particles {
namespace {
uint32_t memoryType(VkPhysicalDevice gpu, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    throw std::runtime_error("ParticleSystem: no compatible memory type");
}

void makeBuffer(VkDevice device, VkPhysicalDevice gpu, VkDeviceSize size, VkBufferUsageFlags usage,
                VkBuffer& buffer, VkDeviceMemory& memory, void** mapped = nullptr,
                VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create.size = size;
    create.usage = usage;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &create, nullptr, &buffer) != VK_SUCCESS) {
        throw std::runtime_error("ParticleSystem: buffer creation failed");
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType(gpu, requirements.memoryTypeBits, flags);
    if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
        throw std::runtime_error("ParticleSystem: buffer memory allocation failed");
    }
    if (mapped && vkMapMemory(device, memory, 0, size, 0, mapped) != VK_SUCCESS) {
        throw std::runtime_error("ParticleSystem: buffer mapping failed");
    }
}
}

ParticleSystem::ParticleSystem(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue computeQueue,
                               VkCommandPool commandPool, uint32_t maxParticles)
    : device_(device), physicalDevice_(physicalDevice), computeQueue_(computeQueue),
      commandPool_(commandPool), maxParticles_(std::max(256u, (maxParticles + 255u) / 256u * 256u)) {
    if (!device_ || !physicalDevice_ || !computeQueue_ || !commandPool_) {
        throw std::invalid_argument("ParticleSystem requires valid Vulkan handles");
    }
    try {
        createBuffers();
        createDescriptorResources();
        createQuadBuffer();
        uploadInitialParticles();
    } catch (...) {
        destroy();
        throw;
    }
}

ParticleSystem::~ParticleSystem() { destroy(); }

void ParticleSystem::update(float deltaTime) {
    const float dt = std::clamp(deltaTime, 0.0f, 0.1f);
    emitter_.accumulator += dt * std::max(0.0f, emitter_.spawnRate);
    const uint32_t requested = static_cast<uint32_t>(emitter_.accumulator);
    emitter_.accumulator -= static_cast<float>(requested);
    const uint32_t spawnCount = std::min(requested, maxParticles_);

    simulation_ = {};
    simulation_.deltaTime = dt;
    simulation_.spawnStart = nextSpawnIndex_;
    simulation_.spawnCount = spawnCount;
    simulation_.maxParticles = maxParticles_;
    simulation_.spawnSeed = spawnSeed_;
    simulation_.emitterPositionMinLife[0] = emitter_.position.x();
    simulation_.emitterPositionMinLife[1] = emitter_.position.y();
    simulation_.emitterPositionMinLife[2] = emitter_.position.z();
    simulation_.emitterPositionMinLife[3] = emitter_.minLifeTime;
    simulation_.minVelocityMinSize[0] = emitter_.minVelocity.x();
    simulation_.minVelocityMinSize[1] = emitter_.minVelocity.y();
    simulation_.minVelocityMinSize[2] = emitter_.minVelocity.z();
    simulation_.minVelocityMinSize[3] = emitter_.minSize;
    simulation_.maxVelocity[0] = emitter_.maxVelocity.x();
    simulation_.maxVelocity[1] = emitter_.maxVelocity.y();
    simulation_.maxVelocity[2] = emitter_.maxVelocity.z();
    simulation_.color[0] = emitter_.color.r();
    simulation_.color[1] = emitter_.color.g();
    simulation_.color[2] = emitter_.color.b();
    simulation_.color[3] = emitter_.color.a();
    simulation_.maxLifeMaxSize[0] = emitter_.maxLifeTime;
    simulation_.maxLifeMaxSize[1] = emitter_.maxSize;
    nextSpawnIndex_ = (nextSpawnIndex_ + spawnCount) % maxParticles_;
    spawnSeed_ += spawnCount;
}

void ParticleSystem::recordCompute(VkCommandBuffer commandBuffer, VkPipeline pipeline,
                                   VkPipelineLayout pipelineLayout, uint32_t frameIndex) const {
    const uint32_t frame = frameIndex % FramesInFlight;
    // The previous frame may still be reading this shared buffer in its vertex
    // stage. Serialize that read before this frame overwrites particle state.
    VkBufferMemoryBarrier2 previousRender{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    previousRender.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    previousRender.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    previousRender.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    previousRender.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    previousRender.buffer = particleBuffer_;
    previousRender.size = VK_WHOLE_SIZE;
    VkDependencyInfo previousRenderDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    previousRenderDependency.bufferMemoryBarrierCount = 1;
    previousRenderDependency.pBufferMemoryBarriers = &previousRender;
    vkCmdPipelineBarrier2(commandBuffer, &previousRenderDependency);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
                            0, 1, &descriptorSets_[0][frame], 0, nullptr);
    vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(ParticleSimulationData), &simulation_);
    vkCmdDispatch(commandBuffer, (maxParticles_ + 255u) / 256u, 1, 1);

    VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    barrier.buffer = particleBuffer_;
    barrier.size = VK_WHOLE_SIZE;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

void ParticleSystem::recordRender(VkCommandBuffer commandBuffer, const ParticleFrameData& frameData,
                                  VkPipeline pipeline, VkPipelineLayout pipelineLayout,
                                  uint32_t frameIndex, bool sceneView) const {
    const uint32_t frame = frameIndex % FramesInFlight;
    const uint32_t target = sceneView ? 1u : 0u;
    std::memcpy(frameMapped_[target][frame], &frameData, sizeof(frameData));
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                            0, 1, &descriptorSets_[target][frame], 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &quadBuffer_, &offset);
    // Dead particles are rejected by the vertex shader; this avoids CPU readback and compaction.
    vkCmdDraw(commandBuffer, 6, maxParticles_, 0, 0);
}

void ParticleSystem::createBuffers() {
    makeBuffer(device_, physicalDevice_, sizeof(Particle) * maxParticles_,
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               particleBuffer_, particleMemory_, nullptr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    for (uint32_t target = 0; target < RenderTargets; ++target) {
        for (uint32_t frame = 0; frame < FramesInFlight; ++frame) {
            makeBuffer(device_, physicalDevice_, sizeof(ParticleFrameData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                       frameBuffers_[target][frame], frameMemories_[target][frame], &frameMapped_[target][frame]);
        }
    }
}

void ParticleSystem::createDescriptorResources() {
    constexpr VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}
    };
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = std::size(bindings);
    layout.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &layout, nullptr, &descriptorSetLayout_) != VK_SUCCESS) {
        throw std::runtime_error("ParticleSystem: descriptor layout creation failed");
    }
    constexpr VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, RenderTargets * FramesInFlight},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, RenderTargets * FramesInFlight}
    };
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = RenderTargets * FramesInFlight;
    pool.poolSizeCount = std::size(sizes);
    pool.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device_, &pool, nullptr, &descriptorPool_) != VK_SUCCESS) {
        throw std::runtime_error("ParticleSystem: descriptor pool creation failed");
    }
    std::array<VkDescriptorSetLayout, RenderTargets * FramesInFlight> layouts{};
    layouts.fill(descriptorSetLayout_);
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptorPool_;
    allocate.descriptorSetCount = RenderTargets * FramesInFlight;
    allocate.pSetLayouts = layouts.data();
    std::array<VkDescriptorSet, RenderTargets * FramesInFlight> flatSets{};
    if (vkAllocateDescriptorSets(device_, &allocate, flatSets.data()) != VK_SUCCESS) {
        throw std::runtime_error("ParticleSystem: descriptor allocation failed");
    }
    for (uint32_t target = 0; target < RenderTargets; ++target) {
        for (uint32_t frame = 0; frame < FramesInFlight; ++frame) {
            descriptorSets_[target][frame] = flatSets[target * FramesInFlight + frame];
            VkDescriptorBufferInfo particles{particleBuffer_, 0, sizeof(Particle) * maxParticles_};
            VkDescriptorBufferInfo uniform{frameBuffers_[target][frame], 0, sizeof(ParticleFrameData)};
            VkWriteDescriptorSet writes[] = {
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSets_[target][frame], 0, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &particles, nullptr},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSets_[target][frame], 1, 0, 1,
                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uniform, nullptr}
            };
            vkUpdateDescriptorSets(device_, std::size(writes), writes, 0, nullptr);
        }
    }
}

void ParticleSystem::createQuadBuffer() {
    constexpr float quad[] = {
        -1, -1, 0, 0, 1, -1, 1, 0, 1, 1, 1, 1,
        -1, -1, 0, 0, 1, 1, 1, 1, -1, 1, 0, 1
    };
    makeBuffer(device_, physicalDevice_, sizeof(quad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
               quadBuffer_, quadMemory_, &quadMapped_);
    std::memcpy(quadMapped_, quad, sizeof(quad));
}

void ParticleSystem::uploadInitialParticles() {
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    const VkDeviceSize size = sizeof(Particle) * maxParticles_;
    try {
        makeBuffer(device_, physicalDevice_, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   stagingBuffer, stagingMemory, &stagingMapped);
        std::memset(stagingMapped, 0, static_cast<size_t>(size));
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = commandPool_;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &allocation, &commandBuffer) != VK_SUCCESS) {
            throw std::runtime_error("ParticleSystem: initial upload command allocation failed");
        }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(commandBuffer, &begin) != VK_SUCCESS) {
            throw std::runtime_error("ParticleSystem: initial upload command begin failed");
        }
        const VkBufferCopy copy{0, 0, size};
        vkCmdCopyBuffer(commandBuffer, stagingBuffer, particleBuffer_, 1, &copy);
        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            throw std::runtime_error("ParticleSystem: initial upload command end failed");
        }
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer;
        if (vkQueueSubmit(computeQueue_, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS ||
            vkQueueWaitIdle(computeQueue_) != VK_SUCCESS) {
            throw std::runtime_error("ParticleSystem: initial particle upload failed");
        }
    } catch (...) {
        if (commandBuffer) vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
        if (stagingMapped) vkUnmapMemory(device_, stagingMemory);
        vkDestroyBuffer(device_, stagingBuffer, nullptr);
        vkFreeMemory(device_, stagingMemory, nullptr);
        throw;
    }
    vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
    vkUnmapMemory(device_, stagingMemory);
    vkDestroyBuffer(device_, stagingBuffer, nullptr);
    vkFreeMemory(device_, stagingMemory, nullptr);
}

void ParticleSystem::destroy() {
    if (!device_) return;
    for (uint32_t target = 0; target < RenderTargets; ++target) {
        for (uint32_t frame = 0; frame < FramesInFlight; ++frame) {
            if (frameMapped_[target][frame]) vkUnmapMemory(device_, frameMemories_[target][frame]);
            vkDestroyBuffer(device_, frameBuffers_[target][frame], nullptr);
            vkFreeMemory(device_, frameMemories_[target][frame], nullptr);
            frameMapped_[target][frame] = nullptr;
        }
    }
    if (quadMapped_) vkUnmapMemory(device_, quadMemory_);
    vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
    vkDestroyBuffer(device_, particleBuffer_, nullptr);
    vkFreeMemory(device_, particleMemory_, nullptr);
    vkDestroyBuffer(device_, quadBuffer_, nullptr);
    vkFreeMemory(device_, quadMemory_, nullptr);
    device_ = VK_NULL_HANDLE;
}
} // namespace Engine::Particles
