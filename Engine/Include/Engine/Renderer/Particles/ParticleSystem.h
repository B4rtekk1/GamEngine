#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

#include "Engine/Math/Color.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Math/Vec4.h"
#include "Engine/Math/Mat4.h"

namespace Engine::Particles {
    struct Particle {
        Vec4 positionLife;
        Vec4 velocitySize;
        Color color;
    };

    struct ParticleEmitter {
        Vec3 position{};
        Vec3 minVelocity{-1.0f, 1.0f, -1.0f};
        Vec3 maxVelocity{1.0f, 4.0f, 1.0f};
        Color color{1.0f, 1.0f, 1.0f, 1.0f};
        float minLifeTime = 1.0f;
        float maxLifeTime = 2.0f;
        float minSize = 0.04f;
        float maxSize = 0.12f;
        float spawnRate = 200.0f;
        float accumulator = 0.0f;
    };

    struct ParticleFrameData {
        Mat4 viewProjection;
        Vec3 cameraRight{1.0f, 0.0f, 0.0f};
        float _pad0 = 0.0f;
        Vec3 cameraUp{0.0f, 1.0f, 0.0f};
        float _pad1 = 0.0f;
    };

    class ParticleSystem {
    public:
        ParticleSystem(VkDevice device, VkPhysicalDevice physicalDevice,
                       VkQueue computeQueue, VkCommandPool commandPool,
                       uint32_t maxParticles);
        ~ParticleSystem();
        void update(float deltaTime);
        void setEmitter(const ParticleEmitter& emitter) { emitter_ = emitter; }
        void recordCompute(VkCommandBuffer commandBuffer, float deltaTime);
        void recordRender(VkCommandBuffer commandBuffer,
                          const ParticleFrameData& frameData,
                          VkPipeline pipeline, VkPipelineLayout pipelineLayout);
        [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const noexcept {
            return descriptorSetLayout_;
        }
    private:
        void createBuffers();
        void createDescriptorResources();
        void createComputePipeline();
        void createQuadBuffer();
        void uploadInitialParticles();
        void spawnParticles(float deltaTime);
        void destroy();

        VkDevice device_{};
        VkPhysicalDevice physicalDevice_{};
        VkQueue computeQueue_{};
        VkCommandPool commandPool_{};
        VkBuffer particleBuffer_{};
        VkDeviceMemory particleMemory_{};
        VkBuffer frameBuffer_{};
        VkDeviceMemory frameMemory_{};
        VkBuffer quadBuffer_{};
        VkDeviceMemory quadMemory_{};
        VkDescriptorSetLayout descriptorSetLayout_{};
        VkDescriptorPool descriptorPool_{};
        VkDescriptorSet descriptorSet_{};
        VkPipeline computePipeline_{};
        VkPipelineLayout computePipelineLayout_{};
        uint32_t maxParticles_ = 0;
        ParticleEmitter emitter_{};
        void* particleMapped_ = nullptr;
        void* frameMapped_ = nullptr;
        void* quadMapped_ = nullptr;
        uint32_t nextSpawnIndex_ = 0;
    };
}
