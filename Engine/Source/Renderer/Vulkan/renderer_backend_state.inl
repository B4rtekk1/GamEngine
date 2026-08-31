    private:
        friend class Renderer;
        SDL_Window* window = nullptr;

        VkInstance instance{};
        VkDebugUtilsMessengerEXT debugMessenger{};
        VkSurfaceKHR surface{};

        VulkanDevice vulkanDevice;
        VkDevice device = VK_NULL_HANDLE;

        Swapchain swapchain;
        VkFramebuffer hdrFramebuffer = VK_NULL_HANDLE;
        // The editor's Scene View uses this actual render output rather than a
        // UI-only placeholder. It has the same attachment formats as the game
        // path, so both views share the forward/sky/particle pipelines.
        ViewportRenderTarget sceneViewportTarget;
        VkFramebuffer sceneViewportFramebuffer = VK_NULL_HANDLE;
        VkRenderPass editorUiRenderPass = VK_NULL_HANDLE;
        std::vector<VkFramebuffer> editorUiFramebuffers;
        VkDescriptorSet gameViewportDescriptor = VK_NULL_HANDLE;
        VkDescriptorSet sceneViewportDescriptor = VK_NULL_HANDLE;
        bool editorUiActive = false;

        MsaaResources msaa;
        HdrBuffer hdrBuffer;
        // A single-sample depth target used exclusively to generate Hi-Z when
        // the visible geometry is rendered with MSAA.
        DepthBuffer hiZDepthBuffer;
        ForwardPass hiZDepthPrepass;
        VkFramebuffer hiZDepthPrepassFramebuffer = VK_NULL_HANDLE;

        ForwardPass& forwardPass;
        GraphicsPipeline& particlePipeline;
        std::unique_ptr<Particles::ParticleSystem> particleSystem;
        VkPipelineLayout particleComputePipelineLayout = VK_NULL_HANDLE;
        VkPipeline particleComputePipeline = VK_NULL_HANDLE;
        SkyPass& skyPass;
        TonemapPass& tonemapPass;
        TemporalAaPass& temporalAaPass;
        UI::CanvasRenderer& canvasRenderer;
        Texture2D fpsFontTexture;
        Texture2D fallbackMaterialTexture;
        std::vector<Texture2D> materialTextures;
        std::vector<VkDescriptorImageInfo> materialTextureDescriptors;
        std::unordered_map<const Mesh*, std::uint32_t> meshTextureOffsets;
        DepthBuffer depthBuffer;
        ShadowPass shadowPass;
        // A descriptor-compatible pass for Scene View. It owns an independent
        // per-frame camera UBO while reusing the exact forward material layout.
        ShadowPass sceneDescriptorPass;
        std::array<Mat4, ShadowMap::ClipLevelCount> shadowClipMatrices{};
        std::array<Mat4, ShadowMap::ClipLevelCount> sceneShadowClipMatrices{};
        Vec3 lastShadowCameraPosition{};
        Vec3 lastSceneShadowCameraPosition{};
        Vec3 lastShadowLightDirection{};
        Vec3 lastSceneShadowLightDirection{};
        std::uint32_t shadowClipUpdateMask{0xFu};
        std::uint32_t sceneShadowClipUpdateMask{0xFu};
        std::uint64_t shadowClipFrameIndex{0};
        bool shadowClipmapsValid{false};
        bool sceneShadowClipmapsValid{false};
        bool fallbackCameraWarningReported{false};
        SkyPass sceneSkyPass;
        Scene& scene;
        Registry& registry;
        const RenderOptimizationFeatures& optimizationFeatures;
        AntialiasingLevel antialiasingLevel;
        Assets::AssetManager& assetManager;
        SceneGpuResources sceneGpu;
        CameraController cameraController;
        using RenderableRecord = SceneGpuResources::RenderableRecord;
        using InstanceBatch = SceneGpuResources::InstanceBatch;
        std::vector<RenderableRecord>& renderables;
        std::vector<InstanceBatch>& instanceBatches;
        std::vector<RendererInstanceData>& instanceModels;
        std::vector<GPUMaterialData>& materials;
        std::uint32_t& materialSlots;
        std::uint64_t& lastTransformRevision;
        std::uint64_t& lastMeshRendererRevision;
        std::uint64_t& lastTerrainGrassRevision;
        std::array<std::vector<std::size_t>, MAX_FRAMES_IN_FLIGHT>& dirtyTransforms;
        std::array<std::vector<std::size_t>, MAX_FRAMES_IN_FLIGHT>& dirtyMaterials;
        std::array<std::vector<std::size_t>, MAX_FRAMES_IN_FLIGHT>& dirtyCullingObjects;
        Vec3& sceneCenter;
        float& sceneRadius;
        bool& hasShadowCasters;
        Buffer vertexBuffer;
        Buffer indexBuffer;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> instanceBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> materialBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> uniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> sceneUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> cullingObjectBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> cullingUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> foliageCullingUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> sceneCullingUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> sceneFoliageCullingUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowCullingUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> indirectBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> foliageIndirectBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> sceneIndirectBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> sceneFoliageIndirectBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowIndirectBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> drawCountBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> foliageDrawCountBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> sceneDrawCountBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> sceneFoliageDrawCountBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowDrawCountBuffers;
        std::array<Culling::GPUCullingPass, MAX_FRAMES_IN_FLIGHT> gpuCullingPasses;
        std::array<Culling::GPUCullingPass, MAX_FRAMES_IN_FLIGHT> foliageGpuCullingPasses;
        std::array<Culling::GPUCullingPass, MAX_FRAMES_IN_FLIGHT> sceneGpuCullingPasses;
        std::array<Culling::GPUCullingPass, MAX_FRAMES_IN_FLIGHT> sceneFoliageGpuCullingPasses;
        std::array<Culling::GPUCullingPass, MAX_FRAMES_IN_FLIGHT> shadowCullingPasses;
        std::array<Culling::IndexedIndirectDrawCount, MAX_FRAMES_IN_FLIGHT> indirectDraws;
        std::array<Culling::IndexedIndirectDrawCount, MAX_FRAMES_IN_FLIGHT> foliageIndirectDraws;
        std::array<Culling::IndexedIndirectDrawCount, MAX_FRAMES_IN_FLIGHT> sceneIndirectDraws;
        std::array<Culling::IndexedIndirectDrawCount, MAX_FRAMES_IN_FLIGHT> sceneFoliageIndirectDraws;
        std::array<Culling::IndexedIndirectDrawCount, MAX_FRAMES_IN_FLIGHT> shadowIndirectDraws;
        Culling::HiZBuffer hiZBuffer;
        Culling::HiZPass hiZPass;
        VkDescriptorPool cullingDescriptorPool = VK_NULL_HANDLE;
        VkDescriptorSetLayout hiZCopyDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout hiZReduceDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout cullingDescriptorSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout hiZCopyPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout hiZReducePipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout cullingPipelineLayout = VK_NULL_HANDLE;
        VkPipeline hiZCopyPipeline = VK_NULL_HANDLE;
        VkPipeline hiZReducePipeline = VK_NULL_HANDLE;
        VkPipeline cullingPipeline = VK_NULL_HANDLE;
        std::vector<Culling::GPUObjectData> gpuObjects;
        // Old and new bounds of renderables whose shadow contribution changed
        // in this frame. ShadowPass evicts only overlapping virtual pages
        // instead of rebuilding the complete atlas for one moving body/blade.
        std::vector<Culling::GPUObjectData> dirtyShadowObjects;
        // CPU-side cache for particle obstacles.  ParticleSystem receives
        // this vector only when collider-related ECS data actually changed.
        std::vector<Particles::ParticleCollider> cachedParticleColliders;
        std::vector<Entity> particleColliderEntities;
        std::unordered_map<Entity, std::size_t> particleColliderIndices;
        const Registry* particleColliderRegistry = nullptr;
        std::uint64_t particleColliderStructuralRevision = 0;
        std::uint64_t particleColliderComponentRevision = 0;
        std::uint64_t particleColliderTransformRevision = 0;
        // Reused frame-stamped deduplication storage for changed renderables.
        std::vector<std::uint32_t> renderableChangeMarks;
        std::uint32_t renderableChangeEpoch = 0;
        std::uint64_t renderableTopologySignature = 0;
        bool hiZValid = false;
        bool sceneViewportActive = false;
        Entity editorSelectedEntity = NullEntity;
        std::uint32_t editorSelectedRenderable = std::numeric_limits<std::uint32_t>::max();

        VkCommandPool commandPool{};
        std::vector<VkCommandBuffer> commandBuffers;

        std::vector<VkSemaphore> imageAvailableSemaphores;
        std::vector<VkSemaphore> renderFinishedSemaphores;
        std::vector<VkFence> inFlightFences;
        uint32_t currentFrame = 0;
        std::uint64_t taaSampleIndex = 0;
        float taaJitterX = 0.0F;
        float taaJitterY = 0.0F;

        bool framebufferResized = false;
        bool cleanedUp = false;

        uint32_t fpsFrameCount = 0;
        double fpsElapsedTime = 0.0;

        static constexpr uint8_t allFrameBits =
            static_cast<uint8_t>((1u << MAX_FRAMES_IN_FLIGHT) - 1u);

        [[nodiscard]] static uint8_t frameBit(const uint32_t frame) noexcept {
            return static_cast<uint8_t>(1U << frame);
        }

        [[nodiscard]] static bool sameTransform(const Transform& lhs,
                                                const Transform& rhs) noexcept {
            // NOLINTNEXTLINE(readability-identifier-length)
            const auto sameVector = [](const Vec3& a, const Vec3& b) {
                return a.x() == b.x() && a.y() == b.y() && a.z() == b.z();
            };
            return sameVector(lhs.position, rhs.position) &&
                   sameVector(lhs.rotation, rhs.rotation) &&
                   sameVector(lhs.scale, rhs.scale);
        }

        [[nodiscard]] static bool sameMaterial(const GPUMaterialData& lhs,
                                               const GPUMaterialData& rhs) noexcept {
            return lhs.baseColorMetallic.x == rhs.baseColorMetallic.x &&
                   lhs.baseColorMetallic.y == rhs.baseColorMetallic.y &&
                   lhs.baseColorMetallic.z == rhs.baseColorMetallic.z &&
                   lhs.baseColorMetallic.w == rhs.baseColorMetallic.w &&
                   lhs.roughnessAmbientOcclusion.x == rhs.roughnessAmbientOcclusion.x &&
                   lhs.roughnessAmbientOcclusion.y == rhs.roughnessAmbientOcclusion.y &&
                   lhs.roughnessAmbientOcclusion.z == rhs.roughnessAmbientOcclusion.z &&
                   lhs.roughnessAmbientOcclusion.w == rhs.roughnessAmbientOcclusion.w &&
                   lhs.textureIndices.x == rhs.textureIndices.x &&
                   lhs.textureIndices.y == rhs.textureIndices.y &&
                   lhs.textureIndices.z == rhs.textureIndices.z &&
                   lhs.textureIndices.w == rhs.textureIndices.w;
        }

        void markDirty(const std::size_t index,
                       uint8_t RenderableRecord::* dirtyFrames,
                       std::array<std::vector<std::size_t>, MAX_FRAMES_IN_FLIGHT>& dirtyIndices) { // NOLINT(readability-named-parameter)
            RenderableRecord& record = renderables[index];
            for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
                const uint8_t bit = frameBit(frame);
                if ((record.*dirtyFrames & bit) == 0) {
                    dirtyIndices[frame].push_back(index);
                }
            }
            record.*dirtyFrames |= allFrameBits;
        }
