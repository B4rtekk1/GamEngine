#pragma once

#include <vulkan/vulkan.h>
#include <array>
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
        static constexpr uint32_t FramesInFlight = 2;

        ParticleSystem(VkDevice device, VkPhysicalDevice physicalDevice,
                       VkQueue computeQueue, VkCommandPool commandPool,
                       uint32_t maxParticles);
        ~ParticleSystem();
        void update(float deltaTime);
        void setEmitter(const ParticleEmitter& emitter) { emitter_ = emitter; }
        // Kept in the frame recording API for renderer ordering; simulation is
        // CPU-authoritative so no device-wide wait is needed before uploads.
        void recordCompute(VkCommandBuffer commandBuffer, float deltaTime) const;
        void recordRender(VkCommandBuffer commandBuffer,
                          const ParticleFrameData& frameData,
                          VkPipeline pipeline, VkPipelineLayout pipelineLayout,
                          uint32_t frameIndex);
        [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const noexcept {
            return descriptorSetLayout_;
        }
    private:
        void createBuffers();
        void createDescriptorResources();
        void createQuadBuffer();
        void uploadInitialParticles();
        void spawnParticles(float deltaTime);
        void destroy();

        VkDevice device_{};
        VkPhysicalDevice physicalDevice_{};
        VkQueue computeQueue_{};
        VkCommandPool commandPool_{};
        std::array<VkBuffer, FramesInFlight> particleBuffers_{};
        std::array<VkDeviceMemory, FramesInFlight> particleMemories_{};
        std::array<VkBuffer, FramesInFlight> frameBuffers_{};
        std::array<VkDeviceMemory, FramesInFlight> frameMemories_{};
        std::array<VkBuffer, FramesInFlight> indirectBuffers_{};
        std::array<VkDeviceMemory, FramesInFlight> indirectMemories_{};
        VkBuffer quadBuffer_{};
        VkDeviceMemory quadMemory_{};
        VkDescriptorSetLayout descriptorSetLayout_{};
        VkDescriptorPool descriptorPool_{};
        std::array<VkDescriptorSet, FramesInFlight> descriptorSets_{};
        uint32_t maxParticles_ = 0;
        ParticleEmitter emitter_{};
        std::vector<Particle> particles_;
        std::vector<Particle> renderParticles_;
        std::array<void*, FramesInFlight> particleMapped_{};
        std::array<void*, FramesInFlight> frameMapped_{};
        std::array<void*, FramesInFlight> indirectMapped_{};
        void* quadMapped_ = nullptr;
        uint32_t nextSpawnIndex_ = 0;
        uint32_t activeParticles_ = 0;
    };
}
