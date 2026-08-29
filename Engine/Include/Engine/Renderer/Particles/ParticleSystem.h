#pragma once

#include <vulkan/vulkan.h>
#include <array>
#include <cstddef>
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
        // initial size, final size, total lifetime, authored opacity
        Vec4 spawnData;
    };

    struct ParticleEmitter {
        Vec3 position;
        Vec3 minVelocity{-1.0F, 1.0F, -1.0F};
        Vec3 maxVelocity{1.0F, 4.0F, 1.0F};
        Color color{1.0F, 1.0F, 1.0F, 1.0F};
        float minLifeTime = 1.0F;
        float maxLifeTime = 2.0F;
        float minSize = 0.04F;
        float maxSize = 0.12F;
        float spawnRate = 200.0F;
        float accumulator = 0.0F;
    };

    inline constexpr float DefaultSmokeBuoyancy = 2.25F;

    // NOLINTBEGIN(readability-magic-numbers)
    /**
     * A particle emitter tuned for smoke.  It deliberately extends the
     * generic emitter so code accepting ParticleEmitter can still use it,
     * while a smoke simulation also receives its fluid-like parameters.
     */
    struct SmokeEmitter final : ParticleEmitter {
        float buoyancy = DefaultSmokeBuoyancy;
        float drag = 0.68F;
        float turbulence = 0.30F;
        float collisionRadius = 0.10F;

        SmokeEmitter() {
            minVelocity = {-0.24F, 0.45F, -0.24F};
            maxVelocity = {0.24F, 1.05F, 0.24F};
            color = {0.18F, 0.20F, 0.23F, 0.19F};
            minLifeTime = 5.5F;
            maxLifeTime = 9.0F;
            // A substantial initial puff, followed by a slow expansion,
            // reads as smoke leaving a hot source rather than point sprites.
            minSize = 0.28F;
            maxSize = 0.88F;
            spawnRate = 260.0F;
        }
    };

    struct ParticleFrameData {
        Mat4 viewProjection;
        Vec3 cameraRight{1.0F, 0.0F, 0.0F};
        float _pad0 = 0.0F;
        Vec3 cameraUp{0.0F, 1.0F, 0.0F};
        float _pad1 = 0.0F;
    };

    /** World-space axis-aligned obstacle used by the smoke simulation. */
    struct ParticleCollider {
        Vec4 center;
        Vec4 halfExtents;
    };

    /** Parameters consumed by particle_simulation::main. Keep this layout in sync
     * with the push-constant block in that shader. */
    struct alignas(16) ParticleSimulationData {
        float deltaTime = 0.0F;
        uint32_t spawnStart = 0;
        uint32_t spawnCount = 0;
        uint32_t maxParticles = 0;
        uint32_t spawnSeed = 0;
        uint32_t colliderCount = 0;
        float padding[2]{};
        float emitterPositionMinLife[4]{};
        float minVelocityMinSize[4]{};
        float maxVelocity[4]{};
        float color[4]{};
        float maxLifeMaxSize[4]{};
        float smokeDynamics[4]{}; // buoyancy, drag, turbulence, collision radius
    };

    static_assert(sizeof(ParticleSimulationData) == 128);
    static_assert(offsetof(ParticleSimulationData, emitterPositionMinLife) == 32);

    class ParticleSystem {
    public:
        static constexpr uint32_t FramesInFlight = 2;

        ParticleSystem(VkDevice device, VkPhysicalDevice physicalDevice,
                       VkQueue computeQueue, VkCommandPool commandPool,
                       uint32_t maxParticles);

        ~ParticleSystem();

        void update(float deltaTime);

        void setEmitter(const ParticleEmitter &emitter) { emitter_ = emitter; }

        void setEmitter(const SmokeEmitter &emitter) {
            emitter_ = static_cast<ParticleEmitter>(emitter);
            smoke_ = emitter;
        }

        void setColliders(const std::vector<ParticleCollider> &colliders) { colliders_ = colliders; }

        /** Records the GPU simulation and makes its writes visible to rendering. */
        void recordCompute(VkCommandBuffer commandBuffer, VkPipeline pipeline,
                           VkPipelineLayout pipelineLayout, uint32_t frameIndex) const;

        void recordRender(VkCommandBuffer commandBuffer,
                          const ParticleFrameData &frameData,
                          VkPipeline pipeline, VkPipelineLayout pipelineLayout,
                          uint32_t frameIndex, bool sceneView = false) const;

        [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const noexcept {
            return descriptorSetLayout_;
        }

    private:
        void createBuffers();

        void createDescriptorResources();

        void createQuadBuffer();

        void uploadInitialParticles();

        void uploadColliders();

        void destroy();

        VkDevice device_{};
        VkPhysicalDevice physicalDevice_{};
        VkQueue computeQueue_{};
        VkCommandPool commandPool_{};
        static constexpr uint32_t RenderTargets = 2;
        VkBuffer particleBuffer_{};
        VkDeviceMemory particleMemory_{};
        std::array<std::array<VkBuffer, FramesInFlight>, RenderTargets> frameBuffers_{};
        std::array<std::array<VkDeviceMemory, FramesInFlight>, RenderTargets> frameMemories_{};
        std::array<VkBuffer, FramesInFlight> activeIndexBuffers_{};
        std::array<VkDeviceMemory, FramesInFlight> activeIndexMemories_{};
        std::array<VkBuffer, FramesInFlight> drawBuffers_{};
        std::array<VkDeviceMemory, FramesInFlight> drawMemories_{};
        VkBuffer quadBuffer_{};
        VkDeviceMemory quadMemory_{};
        VkDescriptorSetLayout descriptorSetLayout_{};
        VkDescriptorPool descriptorPool_{};
        std::array<std::array<VkDescriptorSet, FramesInFlight>, RenderTargets> descriptorSets_{};
        uint32_t maxParticles_ = 0;
        ParticleEmitter emitter_{};
        SmokeEmitter smoke_;
        ParticleSimulationData simulation_{};
        std::array<std::array<void *, FramesInFlight>, RenderTargets> frameMapped_{};
        void *quadMapped_ = nullptr;
        uint32_t nextSpawnIndex_ = 0;
        uint32_t spawnSeed_ = 0;
        static constexpr uint32_t MaxColliders = 64;
        VkBuffer colliderBuffer_{};
        VkDeviceMemory colliderMemory_{};
        void *colliderMapped_ = nullptr;
        std::vector<ParticleCollider> colliders_;
        std::vector<ParticleCollider> activeColliders_;
        std::vector<ParticleCollider> uploadedColliders_;
    };
    // NOLINTEND(readability-magic-numbers)

}
