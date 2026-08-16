#include "Engine/Renderer/Particles/ParticleSystem.h"
#include "Engine/Renderer/shader_loader.h"

#include <algorithm>
#include <cstring>
#include <random>
#include <stdexcept>

namespace Engine::Particles {
namespace {
uint32_t memoryType(VkPhysicalDevice gpu, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("ParticleSystem: no compatible memory type");
}

void makeBuffer(VkDevice device, VkPhysicalDevice gpu, VkDeviceSize size,
                VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory,
                void** mapped) {
    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create.size = size; create.usage = usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &create, nullptr, &buffer) != VK_SUCCESS)
        throw std::runtime_error("ParticleSystem: buffer creation failed");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType(gpu, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS)
        throw std::runtime_error("ParticleSystem: buffer memory allocation failed");
    if (mapped && vkMapMemory(device, memory, 0, size, 0, mapped) != VK_SUCCESS)
        throw std::runtime_error("ParticleSystem: buffer mapping failed");
}
}

ParticleSystem::ParticleSystem(VkDevice device, VkPhysicalDevice physicalDevice,
                               VkQueue computeQueue, VkCommandPool commandPool,
                               uint32_t maxParticles)
    : device_(device), physicalDevice_(physicalDevice), computeQueue_(computeQueue),
      commandPool_(commandPool), maxParticles_(std::max(256u, (maxParticles + 255u) / 256u * 256u)) {
    if (!device_ || !physicalDevice_ || !computeQueue_ || !commandPool_)
        throw std::invalid_argument("ParticleSystem requires valid Vulkan handles");
    try {
        createBuffers(); createDescriptorResources(); createComputePipeline();
        createQuadBuffer(); uploadInitialParticles();
    } catch (...) { destroy(); throw; }
}

ParticleSystem::~ParticleSystem() { destroy(); }

void ParticleSystem::update(float deltaTime) { spawnParticles(deltaTime); }

void ParticleSystem::spawnParticles(float deltaTime) {
    emitter_.accumulator += std::max(0.0f, deltaTime) * emitter_.spawnRate;
    const uint32_t count = static_cast<uint32_t>(emitter_.accumulator);
    emitter_.accumulator -= static_cast<float>(count);
    if (!particleMapped_) return;
    static thread_local std::mt19937 rng{std::random_device{}()};
    const auto random = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); };
    auto* particles = static_cast<Particle*>(particleMapped_);
    for (uint32_t i = 0; i < std::min(count, maxParticles_); ++i) {
        Particle& p = particles[nextSpawnIndex_++ % maxParticles_];
        p.positionLife = {emitter_.position.x(), emitter_.position.y(), emitter_.position.z(),
                          random(emitter_.minLifeTime, emitter_.maxLifeTime)};
        p.velocitySize = {random(emitter_.minVelocity.x(), emitter_.maxVelocity.x()),
                          random(emitter_.minVelocity.y(), emitter_.maxVelocity.y()),
                          random(emitter_.minVelocity.z(), emitter_.maxVelocity.z()),
                          random(emitter_.minSize, emitter_.maxSize)};
        p.color = emitter_.color;
    }
}

void ParticleSystem::recordCompute(VkCommandBuffer commandBuffer, float deltaTime) {
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelineLayout_, 0, 1,
                            &descriptorSet_, 0, nullptr);
    vkCmdPushConstants(commandBuffer, computePipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(float), &deltaTime);
    vkCmdDispatch(commandBuffer, maxParticles_ / 256u, 1, 1);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void ParticleSystem::recordRender(VkCommandBuffer commandBuffer, const ParticleFrameData& frameData,
                                  VkPipeline pipeline, VkPipelineLayout pipelineLayout) {
    std::memcpy(frameMapped_, &frameData, sizeof(frameData));
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1,
                            &descriptorSet_, 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &quadBuffer_, &offset);
    vkCmdDraw(commandBuffer, 6, maxParticles_, 0, 0);
}

void ParticleSystem::createBuffers() {
    makeBuffer(device_, physicalDevice_, sizeof(Particle) * maxParticles_,
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, particleBuffer_, particleMemory_, &particleMapped_);
    makeBuffer(device_, physicalDevice_, sizeof(ParticleFrameData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
               frameBuffer_, frameMemory_, &frameMapped_);
}

void ParticleSystem::createDescriptorResources() {
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = 2; layout.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &layout, nullptr, &descriptorSetLayout_) != VK_SUCCESS)
        throw std::runtime_error("ParticleSystem: descriptor layout creation failed");
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1; pool.poolSizeCount = 2; pool.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device_, &pool, nullptr, &descriptorPool_) != VK_SUCCESS)
        throw std::runtime_error("ParticleSystem: descriptor pool creation failed");
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptorPool_; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &descriptorSetLayout_;
    if (vkAllocateDescriptorSets(device_, &allocate, &descriptorSet_) != VK_SUCCESS)
        throw std::runtime_error("ParticleSystem: descriptor allocation failed");
    VkDescriptorBufferInfo particle{particleBuffer_, 0, sizeof(Particle) * maxParticles_};
    VkDescriptorBufferInfo frame{frameBuffer_, 0, sizeof(ParticleFrameData)};
    VkWriteDescriptorSet writes[] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet_, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &particle, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet_, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &frame, nullptr}};
    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
}

void ParticleSystem::createComputePipeline() {
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1; layout.pSetLayouts = &descriptorSetLayout_; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device_, &layout, nullptr, &computePipelineLayout_) != VK_SUCCESS)
        throw std::runtime_error("ParticleSystem: pipeline layout creation failed");
    const auto shader = vkutil::loadShaderModule(device_, "shaders/particle_update.comp.spv");
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; stage.module = shader.get(); stage.pName = "main";
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage = stage; pipeline.layout = computePipelineLayout_;
    if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &computePipeline_) != VK_SUCCESS)
        throw std::runtime_error("ParticleSystem: compute pipeline creation failed");
}

void ParticleSystem::createQuadBuffer() {
    constexpr float quad[] = {-1,-1,0,0, 1,-1,1,0, 1,1,1,1, -1,-1,0,0, 1,1,1,1, -1,1,0,1};
    makeBuffer(device_, physicalDevice_, sizeof(quad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
               quadBuffer_, quadMemory_, &quadMapped_);
    std::memcpy(quadMapped_, quad, sizeof(quad));
}

void ParticleSystem::uploadInitialParticles() {
    std::memset(particleMapped_, 0, sizeof(Particle) * maxParticles_);
    std::memset(frameMapped_, 0, sizeof(ParticleFrameData));
}

void ParticleSystem::destroy() {
    if (!device_) return;
    if (particleMapped_) vkUnmapMemory(device_, particleMemory_);
    if (frameMapped_) vkUnmapMemory(device_, frameMemory_);
    if (quadMapped_) vkUnmapMemory(device_, quadMemory_);
    vkDestroyPipeline(device_, computePipeline_, nullptr);
    vkDestroyPipelineLayout(device_, computePipelineLayout_, nullptr);
    vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
    vkDestroyBuffer(device_, particleBuffer_, nullptr); vkDestroyBuffer(device_, frameBuffer_, nullptr); vkDestroyBuffer(device_, quadBuffer_, nullptr);
    vkFreeMemory(device_, particleMemory_, nullptr); vkFreeMemory(device_, frameMemory_, nullptr); vkFreeMemory(device_, quadMemory_, nullptr);
    device_ = VK_NULL_HANDLE;
}
}
