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
        // With AA off, Scene View preserves its color image between redraws.
        // This pass starts from the descriptor's sampled layout instead of
        // discarding the cached image through an UNDEFINED transition.
        ForwardPass sceneViewportForwardPass;
        VkRenderPass editorUiRenderPass = VK_NULL_HANDLE;
        std::vector<VkFramebuffer> editorUiFramebuffers;
        VkDescriptorSet gameViewportDescriptor = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, 2> gameViewportTemporalDescriptors{};
        VkDescriptorSet sceneViewportDescriptor = VK_NULL_HANDLE;
        bool editorUiActive = false;
        bool sceneResourcesInitialized = false;

        MsaaResources msaa;
        HdrBuffer hdrBuffer;
        HdrBuffer velocityBuffer;
        GpuTimestampProfiler gpuTimestampProfiler;
        // Retained across frames: reset() clears declarations, while the graph
        // keeps its compiled topology cache.
        RenderGraph::RenderGraph frameGraph;
        GpuProfileFrame lastGpuProfile{};
        // Single-sample depth target populated by the forward pass's MSAA depth resolve.
        DepthBuffer hiZDepthBuffer;

        ForwardPass& forwardPass;
        GraphicsPipeline& particlePipeline;
        std::unique_ptr<Particles::ParticleSystem> particleSystem;
        VkPipelineLayout particleComputePipelineLayout = VK_NULL_HANDLE;
        VkPipeline particleComputePipeline = VK_NULL_HANDLE;
        SkyPass& skyPass;
        TonemapPass& tonemapPass;
        TemporalAaPass& temporalAaPass;
        BloomPass& bloomPass;
        UI::CanvasRenderer& canvasRenderer;
        Texture2D fpsFontTexture;
        Texture2D fallbackMaterialTexture;
        // Terrain height input for procedural grass generation. It is kept
        // separate from material textures so compute can sample it directly.
        Texture2D grassHeightTexture;
        Texture2D grassDensityTexture;
        std::vector<VkDescriptorImageInfo> materialTextureDescriptors;
        std::unordered_map<const Mesh*, std::uint32_t> meshTextureOffsets;
        DepthBuffer depthBuffer;
        std::vector<Texture2D> materialTextures;
        // Shared physical 4096x4096 VSM atlas. Game and Scene contexts own
        // their descriptors/page tables, never concurrent atlas writes.
        ShadowMap physicalShadowPagePool;
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
        std::uint32_t shadowClipUpdateMask{(1u << ShadowMap::ClipLevelCount) - 1u};
        std::uint32_t sceneShadowClipUpdateMask{(1u << ShadowMap::ClipLevelCount) - 1u};
        std::uint64_t shadowClipFrameIndex{0};
        bool shadowClipmapsValid{false};
        bool sceneShadowClipmapsValid{false};
        // A physical page can hold data for only one virtual-shadow context.
        // Switching views invalidates both logical caches before the next use.
        bool gameShadowContextActive{true};
        bool fallbackCameraWarningReported{false};
        SceneFrameDataCache sceneFrameDataCache;
        SkyPass sceneSkyPass;
        Scene& scene;
        Registry& registry;
        const RenderOptimizationFeatures& optimizationFeatures;
        AntialiasingLevel antialiasingLevel;
        const ShadowQuality& shadowQuality;
        const ShadowDebugView& shadowDebugView;
        const GrassRenderSettings& grassSettings;
        Assets::AssetManager& assetManager;
        SceneGpuResources sceneGpu;
        CameraController cameraController;
        using RenderableRecord = SceneGpuResources::RenderableRecord;
        using InstanceBatch = SceneGpuResources::InstanceBatch;
        std::vector<RenderableRecord>& renderables;
        std::vector<InstanceBatch>& instanceBatches;
        std::vector<RendererInstanceData>& instanceModels;
        std::vector<RendererPreviousTransformData>& previousInstanceTransforms;
        std::vector<GPUMaterialData>& materials;
        std::uint32_t& materialSlots;
        std::uint64_t& lastTransformRevision;
        std::uint64_t& lastMeshRendererRevision;
        std::uint64_t& lastTerrainGrassRevision;
        std::uint64_t& lastParentRevision;
        std::array<std::vector<std::size_t>, MAX_FRAMES_IN_FLIGHT>& dirtyTransforms;
        std::array<std::vector<std::size_t>, MAX_FRAMES_IN_FLIGHT>& dirtyMaterials;
        std::array<std::vector<std::size_t>, MAX_FRAMES_IN_FLIGHT>& dirtyCullingObjects;
        Vec3& sceneCenter;
        float& sceneRadius;
        bool& hasShadowCasters;
        Buffer vertexBuffer;
        Buffer indexBuffer;
        // Mesh-shader payload mirrors the append-only indexed geometry heap.
        // It remains separate so the conventional indexed fallback does not
        // pay a descriptor or vertex-input cost for meshlet data.
        Buffer meshletBuffer;
        Buffer meshletVertexBuffer;
        Buffer meshletTriangleBuffer;
        std::uint32_t globalMeshletCount{};
        std::uint32_t meshletVisibleCapacity{};
        // The current forward pipelines use vertex/fragment entry points and
        // indexed draws.  Keep meshlet culling off until a mesh-shader forward
        // pipeline consumes visibleMeshletBuffers.
        bool meshShaderPathActive = false;
        // Geometry Heap. Mesh ranges are never derived from dense ECS order:
        // a new proxy receives an append-only sub-allocation and removing a
        // proxy leaves the old range untouched until a future heap compaction.
        struct GeometryHeapAllocation final {
            std::uint32_t firstVertex{};
            std::uint32_t vertexCount{};
            std::uint32_t firstIndex{};
            std::uint32_t indexCount{};
        };
        std::unordered_map<const Mesh*, GeometryHeapAllocation> geometryHeapAllocations;
        std::uint32_t geometryHeapVertexHighWater{};
        std::uint32_t geometryHeapIndexHighWater{};
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> instanceBuffers;
        // Allocated only with TAA. Descriptor binding 8 falls back to the
        // current transform buffer when this array is empty.
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> previousTransformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> materialBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> gpuSceneInstanceBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> gpuSceneMeshBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> gpuSceneMaterialBuffers;
        // CPU-side roots for the frame-local BDA scene tables.  The current
        // descriptor path remains active until shaders consume this root via
        // push constants.
        std::array<GpuScene, MAX_FRAMES_IN_FLIGHT> gpuSceneRoots;
        // Retained across scene reloads so the next scene starts at the
        // largest GPU-scene table we have already observed, not at a tiny
        // default capacity which would immediately grow during rendering.
        std::size_t gpuSceneInstanceHighWater{};
        std::size_t gpuSceneMeshHighWater{};
        std::size_t gpuSceneMaterialHighWater{};
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> visibleInstanceBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> visibleInstanceCountBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> visibleMeshletBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> visibleMeshletCountBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> meshletCullingUniformBuffers;
        // GPU-driven grass compaction: count -> prefix -> scatter -> indirect.
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> grassBinCountBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> grassBinOffsetBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> grassBinCursorBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> compactGrassInstanceBuffers;
        // Source buffers for the dedicated grass renderer. They contain no
        // RendererInstanceData or GPUScene records.
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> grassClusterBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> generatedGrassInstanceBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> grassDeformationBuffers;
        std::uint64_t grassDeformationVersion{};
        std::array<std::uint64_t, MAX_FRAMES_IN_FLIGHT> uploadedGrassDeformationVersions{};
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> grassIndirectBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> grassDrawCountBuffers;
        // Dedicated command streams. They must never alias generic object
        // indirect buffers: each stream is consumed by a grass-only shader.
        struct GrassRenderLists final {
            // Output of packed-grass frustum culling, before pass-specific
            // distance classification.
            Buffer visibleInstances;
            Buffer visibleCount;
            // Cluster-first culling: the second pass is dispatched indirectly
            // over this compact list, never over off-frustum blades.
            Buffer visibleClusters;
            Buffer visibleClusterCount;
            Buffer bladeCullDispatch;
            // classify, main, shadow and velocity dispatch commands (3 x 12 B).
            Buffer dispatchIndirect;
            // The three streams consumed by the later main/shadow/velocity
            // command builders.  They intentionally do not alias.
            Buffer mainVisibleInstances;
            Buffer shadowVisibleInstances;
            Buffer velocityVisibleInstances;
            Buffer classifyCounts;
            std::array<Buffer, 3> binCounts;
            std::array<Buffer, 3> binOffsets;
            std::array<Buffer, 3> binCursors;
            std::array<Buffer, 3> drawInstances;
            Buffer mainIndirect;
            Buffer mainDrawCount;
            Buffer shadowIndirect;
            Buffer shadowDrawCount;
            Buffer velocityIndirect;
            Buffer velocityDrawCount;
        };
        // Game View and Scene View are never recorded as visible render paths
        // in the same frame. Keep one physical scratch allocation and let the
        // per-view descriptor sets select its contents with their own camera.
        // Scene View retains only its final color target for ImGui caching.
        struct ViewRenderScratchResources final {
            std::array<GrassRenderLists, MAX_FRAMES_IN_FLIGHT> grassRenderLists;
            std::array<Buffer, MAX_FRAMES_IN_FLIGHT> grassClassifyUniformBuffers;
            std::array<Buffer, MAX_FRAMES_IN_FLIGHT> grassPackedCullUniformBuffers;
            std::array<std::array<Buffer, 3>, MAX_FRAMES_IN_FLIGHT> grassPackedStreamUniformBuffers;
            std::array<Buffer, MAX_FRAMES_IN_FLIGHT> uniformBuffers;
            std::array<Buffer, MAX_FRAMES_IN_FLIGHT> cullingUniformBuffers;
            std::array<Buffer, MAX_FRAMES_IN_FLIGHT> foliageCullingUniformBuffers;
            std::array<Buffer, MAX_FRAMES_IN_FLIGHT> indirectBuffers;
            std::array<Buffer, MAX_FRAMES_IN_FLIGHT> foliageIndirectBuffers;
            std::array<Buffer, MAX_FRAMES_IN_FLIGHT> drawCountBuffers;
            std::array<Buffer, MAX_FRAMES_IN_FLIGHT> foliageDrawCountBuffers;
        } viewRenderScratch;
        std::array<GrassRenderLists, MAX_FRAMES_IN_FLIGHT>& grassRenderLists =
            viewRenderScratch.grassRenderLists;
        // Compatibility names keep independent descriptor-set wiring explicit,
        // while referring to the same physical scratch storage.
        std::array<GrassRenderLists, MAX_FRAMES_IN_FLIGHT>& sceneGrassRenderLists =
            viewRenderScratch.grassRenderLists;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> grassIndirectUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> grassPrefixUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& grassClassifyUniformBuffers = viewRenderScratch.grassClassifyUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& grassPackedCullUniformBuffers = viewRenderScratch.grassPackedCullUniformBuffers;
        std::array<std::array<Buffer, 3>, MAX_FRAMES_IN_FLIGHT>& grassPackedStreamUniformBuffers = viewRenderScratch.grassPackedStreamUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& sceneGrassClassifyUniformBuffers = viewRenderScratch.grassClassifyUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& sceneGrassPackedCullUniformBuffers = viewRenderScratch.grassPackedCullUniformBuffers;
        std::array<std::array<Buffer, 3>, MAX_FRAMES_IN_FLIGHT>& sceneGrassPackedStreamUniformBuffers = viewRenderScratch.grassPackedStreamUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& uniformBuffers = viewRenderScratch.uniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& sceneUniformBuffers = viewRenderScratch.uniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> cullingObjectBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& cullingUniformBuffers = viewRenderScratch.cullingUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& foliageCullingUniformBuffers = viewRenderScratch.foliageCullingUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& sceneCullingUniformBuffers = viewRenderScratch.cullingUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& sceneFoliageCullingUniformBuffers = viewRenderScratch.foliageCullingUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowCullingUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedCullingUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& indirectBuffers = viewRenderScratch.indirectBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& foliageIndirectBuffers = viewRenderScratch.foliageIndirectBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& sceneIndirectBuffers = viewRenderScratch.indirectBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& sceneFoliageIndirectBuffers = viewRenderScratch.foliageIndirectBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowIndirectBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedIndirectBuffers;
        // Compact caster-ID streams, one for each shadow clip level.
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowCandidateBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedCandidateBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowCandidateCountBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedCandidateCountBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowCandidateDispatchBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedCandidateDispatchBuffers;
        // One CPU-populated SSBO per frame, shared by both shadow cull passes.
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowPageWorkBuffers;
        // GPU-written receiver requests. They are host-visible only so the
        // CPU cache allocator can consume a completed frame slot without a
        // submission-time readback or GPU stall.
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> vsmRequestedPageBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> vsmCompactedPageBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> vsmCompactedPageCountBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> vsmPageMarkingUniformBuffers;
        std::array<bool, MAX_FRAMES_IN_FLIGHT> vsmRequestsReady{};
        // Forward+ lists are view-relative: the Game and Scene cameras can
        // have different extents and depth partitions in the same frame.
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> clusteredLightRangeBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> clusteredLightIndexBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> sceneClusteredLightRangeBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> sceneClusteredLightIndexBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> clusteredLightingUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> sceneClusteredLightingUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& drawCountBuffers = viewRenderScratch.drawCountBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& foliageDrawCountBuffers = viewRenderScratch.foliageDrawCountBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& sceneDrawCountBuffers = viewRenderScratch.drawCountBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& sceneFoliageDrawCountBuffers = viewRenderScratch.foliageDrawCountBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowDrawCountBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedDrawCountBuffers;
        std::array<Culling::GPUCullingPass, MAX_FRAMES_IN_FLIGHT> gpuCullingPasses;
        std::array<Culling::GPUInstanceCullingPass, MAX_FRAMES_IN_FLIGHT> instanceCullingPasses;
        std::array<Culling::GPUCullingPass, MAX_FRAMES_IN_FLIGHT> foliageGpuCullingPasses;
        std::array<Culling::GPUCullingPass, MAX_FRAMES_IN_FLIGHT> sceneGpuCullingPasses;
        std::array<Culling::GPUCullingPass, MAX_FRAMES_IN_FLIGHT> sceneFoliageGpuCullingPasses;
        std::array<Culling::GPUCullingPass, MAX_FRAMES_IN_FLIGHT> shadowCullingPasses;
        std::array<Culling::GPUCullingPass, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedCullingPasses;
        std::array<Culling::IndexedIndirectDrawCount, MAX_FRAMES_IN_FLIGHT> indirectDraws;
        std::array<Culling::IndexedIndirectDrawCount, MAX_FRAMES_IN_FLIGHT> foliageIndirectDraws;
        std::array<Culling::IndexedIndirectDrawCount, MAX_FRAMES_IN_FLIGHT> sceneIndirectDraws;
        std::array<Culling::IndexedIndirectDrawCount, MAX_FRAMES_IN_FLIGHT> sceneFoliageIndirectDraws;
        std::array<Culling::IndexedIndirectDrawCount, MAX_FRAMES_IN_FLIGHT> shadowIndirectDraws;
        std::array<Culling::IndexedIndirectDrawCount, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedIndirectDraws;
        Culling::HiZBuffer hiZBuffer;
        Culling::HiZPass hiZPass;
        VkDescriptorPool cullingDescriptorPool = VK_NULL_HANDLE;
        VkDescriptorSetLayout hiZCopyDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout hiZReduceDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout cullingDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout instanceCullingDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout meshletCullingDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout grassBuildDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout grassDispatchBuildDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout grassPrefixDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout grassScatterDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout grassFinalizeDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout grassPackedCullDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout grassBladeCullDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout grassClassifyDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout grassPackedBinDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout grassPackedScatterDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout grassPackedFinalizeDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout clusteredLightingDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout vsmPageMarkingDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout vsmPageCompactDescriptorSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout hiZCopyPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout hiZReducePipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout cullingPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout instanceCullingPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout meshletCullingPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout grassBuildPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout grassDispatchBuildPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout grassPrefixPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout grassScatterPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout grassFinalizePipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout grassPackedCullPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout grassBladeCullPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout grassClassifyPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout grassPackedBinPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout grassPackedScatterPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout grassPackedFinalizePipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout clusteredLightingPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout vsmPageMarkingPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout vsmPageCompactPipelineLayout = VK_NULL_HANDLE;
        VkPipeline hiZCopyPipeline = VK_NULL_HANDLE;
        VkPipeline hiZReducePipeline = VK_NULL_HANDLE;
        VkPipeline cullingPipeline = VK_NULL_HANDLE;
        VkPipeline instanceCullingPipeline = VK_NULL_HANDLE;
        VkPipeline meshletCullingPipeline = VK_NULL_HANDLE;
        VkPipeline grassBuildPipeline = VK_NULL_HANDLE;
        VkPipeline grassDispatchBuildPipeline = VK_NULL_HANDLE;
        VkPipeline grassPrefixPipeline = VK_NULL_HANDLE;
        VkPipeline grassScatterPipeline = VK_NULL_HANDLE;
        VkPipeline grassFinalizePipeline = VK_NULL_HANDLE;
        VkPipeline grassPackedCullPipeline = VK_NULL_HANDLE;
        VkPipeline grassBladeCullPipeline = VK_NULL_HANDLE;
        VkPipeline grassClassifyPipeline = VK_NULL_HANDLE;
        VkPipeline grassPackedBinPipeline = VK_NULL_HANDLE;
        VkPipeline grassPackedPrefixPipeline = VK_NULL_HANDLE;
        VkPipeline grassPackedScatterPipeline = VK_NULL_HANDLE;
        VkPipeline grassPackedFinalizePipeline = VK_NULL_HANDLE;
        VkPipeline clusteredLightingPipeline = VK_NULL_HANDLE;
        VkPipeline vsmPageMarkingPipeline = VK_NULL_HANDLE;
        VkPipeline vsmPageCompactPipeline = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> instanceCullSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> meshletCullSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> clusteredLightingSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sceneClusteredLightingSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> vsmPageMarkingSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> vsmPageCompactSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> grassBuildSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> grassVisibleDispatchBuildSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> grassStreamDispatchBuildSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sceneGrassVisibleDispatchBuildSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sceneGrassStreamDispatchBuildSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> grassPrefixSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> grassScatterSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> grassFinalizeSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> grassPackedCullSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> grassBladeCullSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> grassClassifySets{};
        std::array<std::array<VkDescriptorSet, 3>, MAX_FRAMES_IN_FLIGHT> grassPackedBinSets{};
        std::array<std::array<VkDescriptorSet, 3>, MAX_FRAMES_IN_FLIGHT> grassPackedPrefixSets{};
        std::array<std::array<VkDescriptorSet, 3>, MAX_FRAMES_IN_FLIGHT> grassPackedScatterSets{};
        std::array<std::array<VkDescriptorSet, 3>, MAX_FRAMES_IN_FLIGHT> grassPackedFinalizeSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sceneGrassPackedCullSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sceneGrassBladeCullSets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sceneGrassClassifySets{};
        std::array<std::array<VkDescriptorSet, 3>, MAX_FRAMES_IN_FLIGHT> sceneGrassPackedBinSets{};
        std::array<std::array<VkDescriptorSet, 3>, MAX_FRAMES_IN_FLIGHT> sceneGrassPackedPrefixSets{};
        std::array<std::array<VkDescriptorSet, 3>, MAX_FRAMES_IN_FLIGHT> sceneGrassPackedScatterSets{};
        std::array<std::array<VkDescriptorSet, 3>, MAX_FRAMES_IN_FLIGHT> sceneGrassPackedFinalizeSets{};
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
        std::vector<std::uint8_t> renderableChangeKinds;
        std::uint32_t renderableChangeEpoch = 0;
        // O(1) ECS-provided revision.  Do not derive this by traversing every
        // renderable during synchronization.
        std::uint64_t lastRenderTopologyRevision = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t lastParticleEmitterRevision = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t lastSmokeEmitterRevision = std::numeric_limits<std::uint64_t>::max();
        bool hiZValid = false;
        bool sceneViewportActive = false;
        VkExtent2D requestedSceneViewportExtent{};
        // Scene View is an off-screen cache. Redraw it only when its camera or
        // scene data changes; the ImGui panel keeps sampling its last image.
        bool sceneViewportNeedsRender = true;
        bool sceneViewportCacheValid = false;
        // Layout state is separate from cache validity: an empty cache still
        // needs a legal layout when its ImGui descriptor is sampled.
        bool sceneViewportImageInitialized = false;
        bool sceneViewportRendered = false;
        std::uint64_t sceneViewportRenderedRevision = 0;
        Vec3 renderedSceneViewportPosition{};
        float renderedSceneViewportYaw = 0.0F;
        float renderedSceneViewportPitch = 0.0F;
        Entity editorSelectedEntity = NullEntity;
        std::uint32_t editorSelectedRenderable = std::numeric_limits<std::uint32_t>::max();

        VkCommandPool commandPool{};
        VkCommandPool asyncComputeCommandPool{};
        UploadContext uploadContext;
        std::vector<VkCommandBuffer> commandBuffers;
        std::vector<VkCommandBuffer> postAsyncGraphicsCommandBuffers;
        std::vector<VkCommandBuffer> asyncComputeCommandBuffers;
        VkSemaphore asyncComputeTimeline{};
        std::uint64_t asyncComputeTimelineValue{};
        bool asyncHiZSubmittedThisFrame{};

        std::vector<VkSemaphore> imageAvailableSemaphores;
        std::vector<VkSemaphore> renderFinishedSemaphores;
        std::vector<VkFence> inFlightFences;
        uint32_t currentFrame = 0;
        // A fence is also the completion source for retired GPU-scene slots.
        // Values are monotonically increasing submission serials; unlike a
        // queue idle this lets reclamation progress one frame at a time.
        std::array<std::uint64_t, MAX_FRAMES_IN_FLIGHT> frameSubmissionValues{};
        std::uint64_t submittedFrameValue{};
        std::uint64_t completedFrameValue{};
        std::uint64_t taaSampleIndex = 0;
        float taaJitterX = 0.0F;
        float taaJitterY = 0.0F;
        bool taaResolveActive = false;
        Mat4 previousGameView{};
        Mat4 previousGameProjection{};
        Vec3 previousGameCameraPosition{};
        Vec3 previousGameCameraForward{};
        bool previousGameCameraValid = false;

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
                   lhs.textureIndices.w == rhs.textureIndices.w &&
                   lhs.terrainLayerTextures.x == rhs.terrainLayerTextures.x &&
                   lhs.terrainLayerTextures.y == rhs.terrainLayerTextures.y &&
                   lhs.terrainLayerTextures.z == rhs.terrainLayerTextures.z &&
                   lhs.terrainLayerTextures.w == rhs.terrainLayerTextures.w &&
                   lhs.auxiliaryTextureIndices.x == rhs.auxiliaryTextureIndices.x &&
                   lhs.auxiliaryTextureIndices.y == rhs.auxiliaryTextureIndices.y &&
                   lhs.auxiliaryTextureIndices.z == rhs.auxiliaryTextureIndices.z &&
                   lhs.auxiliaryTextureIndices.w == rhs.auxiliaryTextureIndices.w &&
                   lhs.extensionScalars.x == rhs.extensionScalars.x &&
                   lhs.extensionScalars.y == rhs.extensionScalars.y &&
                   lhs.extensionScalars.z == rhs.extensionScalars.z &&
                   lhs.extensionScalars.w == rhs.extensionScalars.w &&
                   lhs.extensionTextureIndices.x == rhs.extensionTextureIndices.x &&
                   lhs.extensionTextureIndices.y == rhs.extensionTextureIndices.y &&
                   lhs.extensionTextureIndices.z == rhs.extensionTextureIndices.z &&
                   lhs.extensionTextureIndices.w == rhs.extensionTextureIndices.w &&
                   lhs.emissiveColorIntensity.x == rhs.emissiveColorIntensity.x &&
                   lhs.emissiveColorIntensity.y == rhs.emissiveColorIntensity.y &&
                   lhs.emissiveColorIntensity.z == rhs.emissiveColorIntensity.z &&
                   lhs.emissiveColorIntensity.w == rhs.emissiveColorIntensity.w &&
                   lhs.textureCoordinateSets0 == rhs.textureCoordinateSets0 &&
                   lhs.textureCoordinateSets1 == rhs.textureCoordinateSets1 &&
                   lhs.textureCoordinateSets2 == rhs.textureCoordinateSets2 &&
                   lhs.textureTransforms == rhs.textureTransforms &&
                   lhs.textureTransformRotations == rhs.textureTransformRotations;
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
