#include "Engine/Renderer/Particles/ParticleSystem.h"

#include <algorithm>
#include <cstring>
#include <random>
#include <stdexcept>

namespace Engine::Particles {
namespace {
uint32_t memoryType(VkPhysicalDevice gpu, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    throw std::runtime_error("ParticleSystem: no compatible memory type");
}

void makeBuffer(VkDevice device, VkPhysicalDevice gpu, VkDeviceSize size,
                VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory,
                void** mapped) {
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
    allocation.memoryTypeIndex = memoryType(
        gpu, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
        throw std::runtime_error("ParticleSystem: buffer memory allocation failed");
    }
    if (mapped && vkMapMemory(device, memory, 0, size, 0, mapped) != VK_SUCCESS) {
        throw std::runtime_error("ParticleSystem: buffer mapping failed");
    }
}
}

ParticleSystem::ParticleSystem(VkDevice device, VkPhysicalDevice physicalDevice,
                               VkQueue computeQueue, VkCommandPool commandPool,
                               uint32_t maxParticles)
    : device_(device), physicalDevice_(physicalDevice), computeQueue_(computeQueue),
      commandPool_(commandPool),
      maxParticles_(std::max(256u, (maxParticles + 255u) / 256u * 256u)),
      particles_(maxParticles_), renderParticles_() {
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

void ParticleSystem::update(float deltaTime) { spawnParticles(deltaTime); }

void ParticleSystem::spawnParticles(float deltaTime) {
    const float dt = std::min(std::max(0.0f, deltaTime), 0.1f);
    static thread_local std::mt19937 rng{std::random_device{}()};
    const auto random = [&](float lo, float hi) {
        return std::uniform_real_distribution<float>(lo, hi)(rng);
    };

    activeParticles_ = 0;
    for (Particle& p : particles_) {
        if (p.positionLife.w() > 0.0f) {
            p.velocitySize.setY(p.velocitySize.y() - 9.81f * dt);
            p.positionLife.setX(p.positionLife.x() + p.velocitySize.x() * dt);
            p.positionLife.setY(p.positionLife.y() + p.velocitySize.y() * dt);
            p.positionLife.setZ(p.positionLife.z() + p.velocitySize.z() * dt);
            p.positionLife.setW(p.positionLife.w() - dt);
            p.color.set_a(std::clamp(p.positionLife.w(), 0.0f, 1.0f));
        }
        if (p.positionLife.w() > 0.0f) {
            ++activeParticles_;
        }
    }

    emitter_.accumulator += dt * std::max(0.0f, emitter_.spawnRate);
    const auto count = static_cast<uint32_t>(emitter_.accumulator);
    emitter_.accumulator -= static_cast<float>(count);
    for (uint32_t i = 0; i < std::min(count, maxParticles_); ++i) {
        Particle& p = particles_[nextSpawnIndex_++ % maxParticles_];
        if (p.positionLife.w() <= 0.0f) {
            ++activeParticles_;
        }
        p.positionLife = {emitter_.position.x(), emitter_.position.y(), emitter_.position.z(),
                          random(emitter_.minLifeTime, emitter_.maxLifeTime)};
        p.velocitySize = {random(emitter_.minVelocity.x(), emitter_.maxVelocity.x()),
                          random(emitter_.minVelocity.y(), emitter_.maxVelocity.y()),
                          random(emitter_.minVelocity.z(), emitter_.maxVelocity.z()),
                          random(emitter_.minSize, emitter_.maxSize)};
        p.color = emitter_.color;
    }

    renderParticles_.clear();
    renderParticles_.reserve(activeParticles_);
    for (const Particle& p : particles_) {
        if (p.positionLife.w() > 0.0f) {
            renderParticles_.push_back(p);
        }
    }
    activeParticles_ = static_cast<uint32_t>(renderParticles_.size());
}

void ParticleSystem::recordCompute(VkCommandBuffer, float) {
    // Simulation is CPU-authoritative and uploads into the current frame's
    // buffer during recordRender. This intentionally emits no GPU work.
}

void ParticleSystem::recordRender(VkCommandBuffer commandBuffer,
                                  const ParticleFrameData& frameData,
                                  VkPipeline pipeline, VkPipelineLayout pipelineLayout,
                                  uint32_t frameIndex) const {
    const uint32_t frame = frameIndex % FramesInFlight;
    if (!renderParticles_.empty()) {
        std::memcpy(particleMapped_[frame], renderParticles_.data(),
                    sizeof(Particle) * renderParticles_.size());
    }
    std::memcpy(frameMapped_[frame], &frameData, sizeof(frameData));

    const VkDrawIndirectCommand draw{6, activeParticles_, 0, 0};
    std::memcpy(indirectMapped_[frame], &draw, sizeof(draw));

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                            0, 1, &descriptorSets_[frame], 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &quadBuffer_, &offset);
    vkCmdDrawIndirect(commandBuffer, indirectBuffers_[frame], 0, 1, sizeof(VkDrawIndirectCommand));
}

void ParticleSystem::createBuffers() {
    for (uint32_t frame = 0; frame < FramesInFlight; ++frame) {
        makeBuffer(device_, physicalDevice_, sizeof(Particle) * maxParticles_,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, particleBuffers_[frame],
                   particleMemories_[frame], &particleMapped_[frame]);
        makeBuffer(device_, physicalDevice_, sizeof(ParticleFrameData),
                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, frameBuffers_[frame],
                   frameMemories_[frame], &frameMapped_[frame]);
        makeBuffer(device_, physicalDevice_, sizeof(VkDrawIndirectCommand),
                   VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, indirectBuffers_[frame],
                   indirectMemories_[frame], &indirectMapped_[frame]);
    }
}

void ParticleSystem::createDescriptorResources() {
    constexpr VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = 2;
    layout.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &layout, nullptr, &descriptorSetLayout_) != VK_SUCCESS) {
        throw std::runtime_error("ParticleSystem: descriptor layout creation failed");
    }

    constexpr VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, FramesInFlight},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FramesInFlight}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = FramesInFlight;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device_, &pool, nullptr, &descriptorPool_) != VK_SUCCESS) {
        throw std::runtime_error("ParticleSystem: descriptor pool creation failed");
    }

    std::array<VkDescriptorSetLayout, FramesInFlight> layouts{};
    layouts.fill(descriptorSetLayout_);
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptorPool_;
    allocate.descriptorSetCount = FramesInFlight;
    allocate.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device_, &allocate, descriptorSets_.data()) != VK_SUCCESS) {
        throw std::runtime_error("ParticleSystem: descriptor allocation failed");
    }

    for (uint32_t frame = 0; frame < FramesInFlight; ++frame) {
        VkDescriptorBufferInfo particle{particleBuffers_[frame], 0,
                                        sizeof(Particle) * maxParticles_};
        VkDescriptorBufferInfo uniform{frameBuffers_[frame], 0, sizeof(ParticleFrameData)};
        VkWriteDescriptorSet writes[] = {
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSets_[frame], 0, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &particle, nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSets_[frame], 1, 0, 1,
             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uniform, nullptr}};
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    }
}

void ParticleSystem::createQuadBuffer() {
    constexpr float quad[] = {-1,-1,0,0, 1,-1,1,0, 1,1,1,1,
                              -1,-1,0,0, 1,1,1,1, -1,1,0,1};
    makeBuffer(device_, physicalDevice_, sizeof(quad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
               quadBuffer_, quadMemory_, &quadMapped_);
    std::memcpy(quadMapped_, quad, sizeof(quad));
}

void ParticleSystem::uploadInitialParticles() {
    std::memset(particles_.data(), 0, sizeof(Particle) * particles_.size());
    for (uint32_t frame = 0; frame < FramesInFlight; ++frame) {
        std::memset(particleMapped_[frame], 0, sizeof(Particle) * particles_.size());
        std::memset(frameMapped_[frame], 0, sizeof(ParticleFrameData));
        constexpr VkDrawIndirectCommand draw{6, 0, 0, 0};
        std::memcpy(indirectMapped_[frame], &draw, sizeof(draw));
    }
}

void ParticleSystem::destroy() {
    if (!device_) return;
    for (uint32_t frame = 0; frame < FramesInFlight; ++frame) {
        if (particleMapped_[frame]) vkUnmapMemory(device_, particleMemories_[frame]);
        if (frameMapped_[frame]) vkUnmapMemory(device_, frameMemories_[frame]);
        if (indirectMapped_[frame]) vkUnmapMemory(device_, indirectMemories_[frame]);
        vkDestroyBuffer(device_, particleBuffers_[frame], nullptr);
        vkDestroyBuffer(device_, frameBuffers_[frame], nullptr);
        vkDestroyBuffer(device_, indirectBuffers_[frame], nullptr);
        vkFreeMemory(device_, particleMemories_[frame], nullptr);
        vkFreeMemory(device_, frameMemories_[frame], nullptr);
        vkFreeMemory(device_, indirectMemories_[frame], nullptr);
        particleMapped_[frame] = frameMapped_[frame] = indirectMapped_[frame] = nullptr;
    }
    if (quadMapped_) vkUnmapMemory(device_, quadMemory_);
    vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
    vkDestroyBuffer(device_, quadBuffer_, nullptr);
    vkFreeMemory(device_, quadMemory_, nullptr);
    device_ = VK_NULL_HANDLE;
}
}
