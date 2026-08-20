#pragma once

#include <vulkan/vulkan.h>
#include <array>
#include <cstddef>
#include <cstdint>

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

    /** Parameters consumed by particle_update.comp.  Keep this layout in sync
     * with the push-constant block in that shader. */
    struct alignas(16) ParticleSimulationData {
        float deltaTime = 0.0f;
        uint32_t spawnStart = 0;
        uint32_t spawnCount = 0;
        uint32_t maxParticles = 0;
        uint32_t spawnSeed = 0;
        float padding[3]{};
        float emitterPositionMinLife[4]{};
        float minVelocityMinSize[4]{};
        float maxVelocity[4]{};
        float color[4]{};
        float maxLifeMaxSize[4]{};
    };
    static_assert(sizeof(ParticleSimulationData) == 112);
    static_assert(offsetof(ParticleSimulationData, emitterPositionMinLife) == 32);

    class ParticleSystem {
    public:
        static constexpr uint32_t FramesInFlight = 2;

        ParticleSystem(VkDevice device, VkPhysicalDevice physicalDevice,
                       VkQueue computeQueue, VkCommandPool commandPool,
                       uint32_t maxParticles);
        ~ParticleSystem();
        void update(float deltaTime);
        void setEmitter(const ParticleEmitter& emitter) { emitter_ = emitter; }
        /** Records the GPU simulation and makes its writes visible to rendering. */
        void recordCompute(VkCommandBuffer commandBuffer, VkPipeline pipeline,
                           VkPipelineLayout pipelineLayout, uint32_t frameIndex) const;
        void recordRender(VkCommandBuffer commandBuffer,
                          const ParticleFrameData& frameData,
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
        VkBuffer quadBuffer_{};
        VkDeviceMemory quadMemory_{};
        VkDescriptorSetLayout descriptorSetLayout_{};
        VkDescriptorPool descriptorPool_{};
        std::array<std::array<VkDescriptorSet, FramesInFlight>, RenderTargets> descriptorSets_{};
        uint32_t maxParticles_ = 0;
        ParticleEmitter emitter_{};
        ParticleSimulationData simulation_{};
        std::array<std::array<void*, FramesInFlight>, RenderTargets> frameMapped_{};
        void* quadMapped_ = nullptr;
        uint32_t nextSpawnIndex_ = 0;
        uint32_t spawnSeed_ = 0;
    };
}
