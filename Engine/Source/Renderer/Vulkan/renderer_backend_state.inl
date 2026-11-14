// Frame fences are the retirement timeline for resources owned by the
// graphics submission chain (including dependent async compute).
class GpuRetirementQueue final {
  public:
    void retire(std::function<void()> destroy, const std::uint64_t value) {
        if (destroy)
            entries_.push_back({value, std::move(destroy)});
    }

    void collect(const std::uint64_t completed) {
        const auto firstReady = std::stable_partition(
            entries_.begin(), entries_.end(), [completed](const Entry& entry) { return entry.value > completed; });
        for (auto entry = firstReady; entry != entries_.end(); ++entry)
            entry->destroy();
        entries_.erase(firstReady, entries_.end());
    }

    void collectAll() {
        collect(UINT64_MAX);
    }

  private:
    struct Entry final {
        std::uint64_t value{};
        std::function<void()> destroy;
    };
    std::vector<Entry> entries_;
};

private:
friend class Renderer;
SDL_Window* window = nullptr;
EditorUiBackend* editorUiBackend = nullptr;

VkInstance instance{};
VkDebugUtilsMessengerEXT debugMessenger{};
VkSurfaceKHR surface{};

VulkanDevice vulkanDevice;
PipelineCache pipelineCache;
VkDevice device = VK_NULL_HANDLE;

Swapchain swapchain;
// Compatibility handles are retained until the renderer's remaining
// resize/reload call sites are consolidated. Dynamic rendering never
// creates or assigns them.
VkFramebuffer hdrFramebuffer = VK_NULL_HANDLE;
VkFramebuffer lightingHdrFramebuffer = VK_NULL_HANDLE;
VkFramebuffer waterHdrFramebuffer = VK_NULL_HANDLE;
// The editor's Scene View uses this actual render output rather than a
// UI-only placeholder. It has the same attachment formats as the game
// path, so both views share the forward/sky/particle pipelines.
ViewportRenderTarget sceneViewportTarget;
// Scene View may use a resolution different from the swapchain, so it
// needs a depth attachment with the same extent as its color target.
DepthBuffer sceneViewportDepthBuffer;
bool sceneViewportDepthInitialized{false};
VkFramebuffer sceneViewportFramebuffer = VK_NULL_HANDLE;
// With AA off, Scene View preserves its color image between redraws.
// This pass starts from the descriptor's sampled layout instead of
// discarding the cached image through an UNDEFINED transition.
ForwardPass sceneViewportForwardPass;
// Scene View depth prepass and resolved shadow visibility, as in the Game
// View: lighting then shades every pixel once and reads its shadow, instead
// of running the virtual-shadow filter for every overdrawn fragment.
ForwardPass sceneViewportDepthPrepass;
HdrBuffer sceneViewportVelocity;
HdrBuffer sceneViewportNormals;
DirectionalVisibilityPass sceneDirectionalVisibilityPass;
VkDescriptorSet gameViewportDescriptor = VK_NULL_HANDLE;
std::array<VkDescriptorSet, 2> gameViewportTemporalDescriptors{};
VkDescriptorSet gameViewportDisplayDescriptor = VK_NULL_HANDLE;
VkDescriptorSet sceneViewportDescriptor = VK_NULL_HANDLE;
bool editorUiActive = false;
bool sceneResourcesInitialized = false;

MsaaResources msaa;
HdrBuffer hdrBuffer;
// HDR is persistent. Its first graph import must start from UNDEFINED;
// subsequent frames begin after the post path left it sampled.
bool hdrBufferInitialized{false};
// Immutable opaque HDR source sampled by the refracting water pass.
HdrBuffer opaqueSceneColor;
bool opaqueSceneColorInitialized{false};
// Scene View has a distinct opaque source because Virtual Water
// refracts the editor's off-screen color target, never Game View HDR.
HdrBuffer sceneOpaqueColor;
bool sceneOpaqueColorInitialized{false};
HdrBuffer velocityBuffer;
// Octahedrally encoded view-space normals written by the opaque depth prepass.
HdrBuffer gtaoViewNormalBuffer;
RtContactShadowPass rtContactShadowPass;
// Ray-traced shadows of point and spot lights; its masks always exist so the
// forward binding is valid, and start cleared ("no receiver").
LocalLightShadowPass localLightShadowPass;
TileLightCullingPass tileLightCulling;
DeferredLocalLightingPass deferredLocalLighting;
// Set while recording a frame whose Game View local lights are deferred.
bool deferredLocalThisFrame = false;
// Visibility-buffer prototype (GAMENGINE_VISBUFFER=1): the depth prepass
// writes triangle ids and a compute pass shades the PBR slots from them.
VisibilityBufferPass visibilityBuffer;
bool visibilityResolveThisFrame = false;
// Any material displaces its vertices (the resolve has no displacement).
bool materialsDisplaced = true;
static bool visibilityBufferRequested() noexcept {
    static const bool requested = [] {
        const char* value = std::getenv("GAMENGINE_VISBUFFER");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return requested;
}
// Set when this frame's tracer drew stochastic light samples (MegaLights).
bool megaLightsTracedThisFrame = false;
// TransformSystem world-change serial the renderable sync last consumed.
std::uint64_t renderableWorldTransformSerial = 0;
std::array<bool, MAX_FRAMES_IN_FLIGHT> localShadowMaskInitialized{};
// Shadow rays trace whichever acceleration structure is faster on this GPU.
// Cluster (Mega Geometry) traversal is hardware accelerated only on recent
// architectures, so it is measured on the real scene rather than assumed:
// alternating trial phases time both, then the faster one is kept.
enum class ShadowTraceBackend : std::uint8_t { Measuring, Cluster, Classic };
// GAMENGINE_SHADOW_BACKEND=cluster|classic skips the timing trial.
ShadowTraceBackend shadowTraceBackend{[] {
    const char* forced = std::getenv("GAMENGINE_SHADOW_BACKEND");
    if (forced != nullptr && std::string_view{forced} == "cluster") {
        return ShadowTraceBackend::Cluster;
    }
    if (forced != nullptr && std::string_view{forced} == "classic") {
        return ShadowTraceBackend::Classic;
    }
    return ShadowTraceBackend::Measuring;
}()};
std::uint32_t shadowTraceTrialFrame{};
std::array<bool, MAX_FRAMES_IN_FLIGHT> shadowTraceSampled{};
std::array<bool, MAX_FRAMES_IN_FLIGHT> shadowTraceUsedCluster{};
// [0] classic triangle TLAS, [1] cluster scene: shadow pass GPU milliseconds.
std::array<std::vector<float>, 2> shadowTraceSamples;
// Where the measured winner is kept (project Library, per GPU and driver), so
// the trial runs once instead of on every launch: it traced the slower
// backend for half of its ~200 frames and built both structures meanwhile.
std::filesystem::path shadowTraceDecisionPath;

void loadShadowTraceDecision(const std::filesystem::path& directory) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(vulkanDevice.physical(), &properties);
    shadowTraceDecisionPath = directory / ("shadow-trace-" + std::to_string(properties.vendorID) + "-" +
                                          std::to_string(properties.deviceID) + "-" +
                                          std::to_string(properties.driverVersion) + ".txt");
    // GAMENGINE_SHADOW_BACKEND already chose one.
    if (shadowTraceBackend != ShadowTraceBackend::Measuring)
        return;
    std::ifstream input(shadowTraceDecisionPath);
    std::string backend;
    if (!(input >> backend))
        return;
    if (backend == "cluster")
        shadowTraceBackend = ShadowTraceBackend::Cluster;
    else if (backend == "classic")
        shadowTraceBackend = ShadowTraceBackend::Classic;
}

void saveShadowTraceDecision(const float classicMs, const float clusterMs) const noexcept {
    if (shadowTraceDecisionPath.empty())
        return;
    std::error_code error;
    std::filesystem::create_directories(shadowTraceDecisionPath.parent_path(), error);
    std::ofstream output(shadowTraceDecisionPath, std::ios::trunc);
    // First token: the winner; the timings are informational. Delete the
    // file to measure again.
    output << (shadowTraceBackend == ShadowTraceBackend::Cluster ? "cluster" : "classic") << "\nclassic_ms "
           << classicMs << "\ncluster_ms " << clusterMs << '\n';
}
// Game View screen-space flags as written to the frame UBO, before the local
// shadow bit is known.
std::uint32_t gameScreenSpaceEffects{1};
DirectionalVisibilityPass directionalVisibilityPass;
SsgiPass ssgiPass;
// Debug HDR readback (Renderer::requestHdrCapture).
bool hdrCaptureRequested{false};
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> hdrCaptureBuffers{};
std::array<bool, MAX_FRAMES_IN_FLIGHT> hdrCapturePending{};
std::vector<std::uint16_t> hdrCapturePixels;
VkExtent2D hdrCaptureExtent{};
bool hdrCaptureReady{false};
// Readback of the final presented image (Renderer::requestFrameCapture).
bool frameCaptureRequested{false};
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> frameCaptureBuffers{};
std::array<bool, MAX_FRAMES_IN_FLIGHT> frameCapturePending{};
std::array<VkExtent2D, MAX_FRAMES_IN_FLIGHT> frameCaptureExtents{};
std::vector<std::uint8_t> frameCapturePixels;
VkExtent2D frameCaptureExtent{};
bool frameCaptureReady{false};
DdgiPass ddgiPass;
ClusterRayTracingScene clusterScene;
// Mega Geometry scene shared by DDGI and ray-traced local light shadows.
// Set whenever meshlet geometry or its heap buffers change; the cluster
// BLASes are then rebuilt before the next trace.
bool clusterGeometryDirty{true};
std::uint64_t clusterTopologyRevision{~std::uint64_t{0}};
bool ddgiTracedThisFrame{false};
std::vector<ClusterRayTracingScene::InstanceInput> clusterInstances;
std::vector<ClusterRayTracingScene::BlasInput> clusterBlases;
std::vector<ClusterRayTracingScene::VariantInput> clusterVariants;
// World bounds of each cluster scene instance; DDGI wakes probes around moves.
std::vector<std::array<float, 6>> clusterInstanceBounds;
// Probe-cache change detection between consecutive DDGI frames.
std::vector<std::array<float, 6>> ddgiPreviousBounds;
std::vector<std::array<float, 12>> ddgiPreviousTransforms;
std::vector<std::array<float, 6>> ddgiWakeBoxes;
std::uint64_t ddgiLightHash{};
VkImageView ddgiEnvironmentView{VK_NULL_HANDLE};
// Current Game View space to the clip space of the last captured SSGI
// radiance. Built before previousGameView advances to this frame.
Mat4 ssgiViewToPreviousClip;
// The viewport graph traced SSGI this frame, so the lit HDR must be captured.
bool ssgiTracedThisFrame{false};
AccelerationStructureManager accelerationStructures;
bool rayTracingBlasDirty{true};
// TLAS input is reconstructed only when ECS render state changes. Each
// frame slot still receives its initial build before being reused.
std::uint64_t lastRtTlasTopologyRevision{};
bool rtTlasInputDirty{true};
// A transform change must reach every in-flight TLAS, not just the
// slot which happened to rebuild the shared CPU instance list.
std::array<bool, MAX_FRAMES_IN_FLIGHT> rtTlasUpdatePending{};
std::vector<AccelerationStructureManager::InstanceBuildInput> rtTlasInstances;
// Reused BLAS build list; the ray-traced contact shadow path rebuilds it only
// when geometry changes, but the old code reallocated the vector every time.
std::vector<AccelerationStructureManager::MeshBuildInput> rtMeshBuildInputs;
GpuTimestampProfiler gpuTimestampProfiler;
// Retained across frames: reset() clears declarations, while the graph
// keeps its compiled topology cache.
// The authoritative description of external inputs consumed anywhere
// in this frame.  Its upload waits replace the old global transfer
// timeline wait at submission time.
RenderGraph::RenderGraph frameGraph;
// Incrementally migrated callbacks for the Game View raster path.
RenderGraph::RenderGraph viewportFrameGraph;
// Early compute is submitted in the same command buffer for now, but
// owns a separate graph so it can migrate independently of the
// depth/Hi-Z graph below.
RenderGraph::RenderGraph earlyFrameGraph;
// The final, presentation-facing chain is deliberately independent of
// the viewport graph: both Game and Scene rendering finish before it,
// while its output is the acquired swapchain image.
GpuProfileFrame lastGpuProfile{};
std::uint32_t adaptiveShadowPageBudget{64};
float smoothedGameVsmMs{0.0F};
std::array<bool, MAX_FRAMES_IN_FLIGHT> gameVsmProfiled{};
// Single-sample depth target populated by the forward pass's MSAA depth resolve.
DepthBuffer hiZDepthBuffer;

ForwardPass & forwardPass;
ForwardPass lightingForwardPass;
WaterPass waterPass;
// Transmissive materials over the lit Game View / Scene View (Hi-Z SSR).
GlassPass glassPass;
GlassPass sceneGlassPass;
// Glass resources (scene snapshot with mips, depth pyramid, private depth:
// ~40 MiB per view at 1080p) are created on the first frame that draws
// glass, from these parameters, not for every scene.
GlassPass::CreateInfo glassPassInfo{};
GlassPass::CreateInfo sceneGlassPassInfo{};
Water::VirtualWaterRenderer virtualWaterRenderer;
Water::VirtualWaterRenderer sceneVirtualWaterRenderer;
GraphicsPipeline & particlePipeline;
GraphicsPipeline sceneParticlePipeline;
std::unique_ptr<Particles::ParticleSystem> particleSystem;
VkPipelineLayout particleComputePipelineLayout = VK_NULL_HANDLE;
VkPipeline particleComputePipeline = VK_NULL_HANDLE;
SkyPass & skyPass;
TonemapPass & tonemapPass;
TemporalAaPass & temporalAaPass;
BloomPass & bloomPass;
GtaoPass & gtaoPass;
UI::CanvasRenderer& canvasRenderer;
Texture2D fpsFontTexture;
Texture2D fallbackMaterialTexture;
ImageBasedLighting imageBasedLighting;
ReflectionProbeManager reflectionProbeManager;
// Probe extraction is static in the common case. Each frame-local SSBO
// is refreshed only when its fence makes it safe after an ECS change.
bool reflectionProbeTableInitialized = false;
std::uint64_t reflectionProbeComponentRevision = 0;
std::uint64_t reflectionProbeTransformRevision = 0;
std::uint32_t reflectionProbeBufferDirtyMask = (1U << MAX_FRAMES_IN_FLIGHT) - 1U;
std::filesystem::path environmentEquirectangularPath;
// What the live IBL maps were built from. A scene reload (leaving Play mode,
// undo past a snapshot) keeps them when nothing here changed: rebuilding
// meant decoding the whole panorama again, about two seconds per reload.
struct IblBuildKey final {
    std::filesystem::path environment;
    std::filesystem::file_time_type modified{};
    std::filesystem::path library;
    IblQuality quality{};
    bool operator==(const IblBuildKey&) const = default;
};
std::optional<IblBuildKey> builtIblKey;
// Caches belong to the opened game project, never beside the Editor executable.
std::filesystem::path projectRoot;
// Parsed IES light profiles by resolved path (nothing for unreadable files).
std::unordered_map<std::string, std::optional<LightProfileTable>> iesProfileCache;
// Terrain height input for procedural grass generation. It is kept
// separate from material textures so compute can sample it directly.
Texture2D grassHeightTexture;
Texture2D grassDensityTexture;
std::vector<VkDescriptorImageInfo> materialTextureDescriptors;
struct TextureId final {
    std::uint64_t owner{};
    std::uint32_t imageIndex{};
    bool operator==(const TextureId&) const noexcept = default;
};
struct TextureIdHash final {
    std::size_t operator()(const TextureId& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.owner) ^ (std::hash<std::uint32_t>{}(id.imageIndex) + 0x9e3779b9U);
    }
};
struct TextureGpuResource final {
    Texture2D texture;
    std::uint32_t descriptorIndex{};
    std::uint32_t refCount{};
};
std::unordered_map<TextureId, TextureGpuResource, TextureIdHash> textureGpuResources;
std::unordered_map<std::uint64_t, std::vector<TextureId>> meshTextureIds;
std::unordered_map<const Mesh*, std::vector<std::uint32_t>> meshTextureSlots;
// Decoded geometry reused by consecutive topology rebuilds. Keyed by the
// process-unique GPU resource identity so a re-imported asset cannot pick
// up stale geometry, and bounded so a long editing session cannot keep
// every source mesh resident.
struct DecodedMeshSource final {
    std::shared_ptr<const MeshSourceData> source;
    std::uint64_t bytes{};
    std::uint64_t lastUse{};
};
std::unordered_map<std::uint64_t, DecodedMeshSource> decodedMeshSources;
std::uint64_t decodedMeshSourceBytes{};
std::uint64_t decodedMeshSourceClock{};
static constexpr std::uint64_t DecodedMeshSourceBudget = 256ULL * 1024ULL * 1024ULL;
std::uint32_t nextMaterialTextureSlot{1};
std::vector<std::uint32_t> freeMaterialTextureSlots;
DepthBuffer depthBuffer;
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
bool rtContactActive{false};
bool fallbackCameraWarningReported{false};
SceneFrameDataCache sceneFrameDataCache;
SkyPass sceneSkyPass;
Scene & scene;
Registry & registry;
const RenderOptimizationFeatures& optimizationFeatures;
const RenderPath& renderPath;
AntialiasingLevel antialiasingLevel;
const ShadowQuality& shadowQuality;
const RtContactShadowSettings& rtContactShadowSettings;
const GtaoQuality& gtaoQuality;
GtaoQuality configuredGtaoQuality;
const GtaoDebugView& gtaoDebugView;
const PbrDebugView& pbrDebugView;
const IblQuality& iblQuality;
const ShadowDebugView& shadowDebugView;
const GrassRenderSettings& grassSettings;
const SsgiSettings& ssgiSettings;
const DdgiSettings& ddgiSettings;
const PostProcessSettings& postProcessSettings;
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
Vec3 & sceneCenter;
float& sceneRadius;
bool& hasShadowCasters;
Buffer vertexBuffer;
Buffer indexBuffer;
// Mesh-shader payload mirrors the append-only indexed geometry heap.
// It remains separate so the conventional indexed fallback does not
// pay a descriptor or vertex-input cost for meshlet data.
Buffer meshletBuffer;
Buffer meshletClusterBuffer;
Buffer meshletVertexBuffer;
Buffer meshletTriangleBuffer;
std::uint32_t globalMeshletCount{};
std::uint32_t meshletVisibleCapacity{};
// Meshlet draw streams pack material slots like drawBinOffsets: slot s owns
// commands [meshletBinOffsets[s], meshletBinOffsets[s + 1]). They follow the
// 32 per-slot draw counts in meshletIndexedDrawCountBuffers (meshlet_build_indexed).
std::array<std::uint32_t, Culling::DrawBinOffsetCount> meshletBinOffsets{};
// Commands meshletIndexedIndirectBuffers hold for packed slots (excluding
// the maxDrawCount tail vkCmdDrawIndexedIndirectCount requires).
std::uint32_t meshletBinBudget{};
// The indexed meshlet forward path consumes visibleMeshletBuffers.
bool meshletForwardPathActive = true;
// Set whenever the material table changes; the depth prepass then re-checks
// whether any material is displaced (see ForwardPass::useReducedShaders).
bool displacedMaterialsDirty = true;
// Changes whenever something the depth buffer shows may have changed apart
// from the camera: a renderable's transform, material or culling data, mesh
// geometry, or the scene itself. The VSM page-request cache keys on it
// instead of the registry-wide revisions, which also move with the camera
// entity and invalidated the cache on every frame of a still view.
std::uint64_t depthContentRevision = 0;
// Geometry Heap. Mesh ranges are never derived from dense ECS order.
// MeshId is a stable indirection handle; the allocation below is its
// current physical location and may therefore be recycled or moved.
struct GeometryHeapAllocation final {
    std::uint32_t firstVertex{};
    std::uint32_t vertexCount{};
    std::uint32_t firstIndex{};
    std::uint32_t indexCount{};
};
struct GeometryHeapRange final {
    std::uint32_t first{};
    std::uint32_t count{};
};
struct RetiredGeometryHeapAllocation final {
    GeometryHeapAllocation allocation{};
    std::uint64_t reclaimAfter{};
};
std::unordered_map<const void*, GeometryHeapAllocation> geometryHeapAllocations;
std::unordered_map<const void*, MeshId> geometryHeapMeshIds;
std::vector<GeometryHeapRange> geometryHeapFreeVertices;
std::vector<GeometryHeapRange> geometryHeapFreeIndices;
std::vector<RetiredGeometryHeapAllocation> retiredGeometryHeapAllocations;
std::uint32_t nextGeometryHeapMeshId{};
std::uint32_t geometryHeapVertexHighWater{};
std::uint32_t geometryHeapIndexHighWater{};
// Meshlet streams use the same stable-resource model as indexed
// geometry.  A topology delta appends only the payload for its new
// MeshGpuResource; existing assets retain their shader-visible IDs.
struct MeshletHeapAllocation final {
    std::uint32_t firstMeshlet{};
    std::uint32_t meshletCount{};
    std::uint32_t firstCluster{};
    std::uint32_t clusterCount{};
    std::uint32_t firstVertexIndex{};
    std::uint32_t vertexIndexCount{};
    std::uint32_t firstTriangle{};
    std::uint32_t triangleCount{};
};
std::unordered_map<const void*, MeshletHeapAllocation> meshletHeapAllocations;
struct RetiredMeshletHeapAllocation final {
    MeshletHeapAllocation allocation{};
    std::uint64_t reclaimAfter{};
};
std::vector<GeometryHeapRange> meshletHeapFreeRanges;
std::vector<GeometryHeapRange> meshletClusterHeapFreeRanges;
std::vector<GeometryHeapRange> meshletVertexHeapFreeRanges;
std::vector<GeometryHeapRange> meshletTriangleHeapFreeRanges;
std::vector<RetiredMeshletHeapAllocation> retiredMeshletHeapAllocations;
std::uint32_t meshletHeapHighWater{};
std::uint32_t meshletClusterHeapHighWater{};
std::uint32_t meshletVertexHeapHighWater{};
std::uint32_t meshletTriangleHeapHighWater{};
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> instanceBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> reflectionProbeBuffers;
// Allocated only with TAA. Descriptor binding 8 falls back to the
// current transform buffer when this array is empty.
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> previousTransformBuffers;
// A descriptor set may still refer to a prior transform buffer from a
// submitted frame. Retire each allocation with that frame's fence.
std::array<std::vector<Buffer>, MAX_FRAMES_IN_FLIGHT> deferredPreviousTransformBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> materialBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> gpuSceneInstanceBuffers;
// Compact, transform-derived bounds for coarse GPU visibility. Kept
// index-aligned with gpuSceneInstanceBuffers.
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> gpuVisibilityInstanceBuffers;
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
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> meshletCullDispatchBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> visibleMeshletBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> visibleMeshletCountBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> meshletIndexedIndirectBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> meshletIndexedDrawCountBuffers;
std::array<Culling::IndexedIndirectDrawCount, MAX_FRAMES_IN_FLIGHT> meshletIndexedIndirectDraws;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> meshletCullingUniformBuffers;
// Mesh-shader forward path: per-slot visible meshlet entries (packed like
// meshletBinOffsets) and one VkDrawMeshTasksIndirectCommandEXT per slot.
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> meshletDrawListBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> meshletDrawCommandBuffers;
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
// VSM grass never reuses the camera-wide shadow commands.  These
// arrays contain one command range and count per physical page.
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> grassShadowPageMatricesBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> grassShadowPageIndirectBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> grassShadowPageDrawCountBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> grassShadowPageCullUniformBuffers;
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
    std::array<Buffer, MAX_FRAMES_IN_FLIGHT> sceneCullingUniformBuffers;
    std::array<Buffer, MAX_FRAMES_IN_FLIGHT> foliageCullingUniformBuffers;
    std::array<Buffer, MAX_FRAMES_IN_FLIGHT> indirectBuffers;
    std::array<Buffer, MAX_FRAMES_IN_FLIGHT> foliageIndirectBuffers;
    std::array<Buffer, MAX_FRAMES_IN_FLIGHT> drawCountBuffers;
    std::array<Buffer, MAX_FRAMES_IN_FLIGHT> foliageDrawCountBuffers;
} viewRenderScratch;
std::array<GrassRenderLists, MAX_FRAMES_IN_FLIGHT>& grassRenderLists = viewRenderScratch.grassRenderLists;
// Compatibility names keep independent descriptor-set wiring explicit,
// while referring to the same physical scratch storage.
std::array<GrassRenderLists, MAX_FRAMES_IN_FLIGHT>& sceneGrassRenderLists = viewRenderScratch.grassRenderLists;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> grassIndirectUniformBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> grassPrefixUniformBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& grassClassifyUniformBuffers = viewRenderScratch.grassClassifyUniformBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& grassPackedCullUniformBuffers =
    viewRenderScratch.grassPackedCullUniformBuffers;
std::array<std::array<Buffer, 3>, MAX_FRAMES_IN_FLIGHT>& grassPackedStreamUniformBuffers =
    viewRenderScratch.grassPackedStreamUniformBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& sceneGrassClassifyUniformBuffers =
    viewRenderScratch.grassClassifyUniformBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& sceneGrassPackedCullUniformBuffers =
    viewRenderScratch.grassPackedCullUniformBuffers;
std::array<std::array<Buffer, 3>, MAX_FRAMES_IN_FLIGHT>& sceneGrassPackedStreamUniformBuffers =
    viewRenderScratch.grassPackedStreamUniformBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& uniformBuffers = viewRenderScratch.uniformBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& sceneUniformBuffers = viewRenderScratch.uniformBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> cullingObjectBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& cullingUniformBuffers = viewRenderScratch.cullingUniformBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& foliageCullingUniformBuffers = viewRenderScratch.foliageCullingUniformBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& sceneCullingUniformBuffers = viewRenderScratch.sceneCullingUniformBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT>& sceneFoliageCullingUniformBuffers =
    viewRenderScratch.foliageCullingUniformBuffers;
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
// Per-page compact float3x4 streams consumed directly by shadow VS.
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowInstanceTransformBuffers;
// Parallel identity stream: transform compaction is page-local and
// must not discard the source GPU-scene instance ID.
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowVisibleInstanceBuffers;
// Per-meshlet VSM caster refinement (shadow_meshlet_refine.slang): per page,
// the objects gpu_culling hands over, and the meshlet draws built from them.
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowMeshletObjectBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowMeshletObjectCountBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowMeshletCommandBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowMeshletCommandCountBuffers;
// Mesh-shader casters: one VkDrawMeshTasksIndirectCommandEXT per page.
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowMeshletTaskCommandBuffers;
std::uint32_t shadowMeshletPassCapacity = 0;
// The same per-page lists for the two-sided (foliage, alpha-tested) stream.
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedMeshletObjectBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedMeshletObjectCountBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedMeshletCommandBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedMeshletCommandCountBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedMeshletTaskCommandBuffers;
std::uint32_t shadowTwoSidedMeshletPassCapacity = 0;
// Per-page capacities of the VSM caster lists, [0] single-sided and [1]
// two-sided: commands (shadow casters of that category) and compact
// instance transforms. Each of the 256 page slots reserves this much.
std::array<std::uint32_t, 2> shadowPageCasterCapacity{};
std::array<std::uint32_t, 2> shadowPageInstanceCapacity{};
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedInstanceTransformBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedVisibleInstanceBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowInstanceTransformCountBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedInstanceTransformCountBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowInstanceMaterialBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedInstanceMaterialBuffers;
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
std::array<bool, MAX_FRAMES_IN_FLIGHT> vsmRequestsFromScene{};
std::array<bool, MAX_FRAMES_IN_FLIGHT> sceneVsmFeedbackPending{};
VsmFeedbackCache sceneVsmFeedbackCache;
VsmFeedbackKey sceneVsmFeedbackKey;
std::vector<std::uint32_t> sceneVsmReceiverRequests;
// Reused per frame instead of allocating a fresh page-request list while
// recording the scene view.
std::vector<std::uint32_t> vsmRequestScratch;
// Legacy frame uploads are declared and consumed inside a single
// recordCommandBuffer call, so their handle lists can live across frames.
std::vector<RenderGraph::BufferHandle> frameUploadBufferScratch;
std::vector<RenderGraph::TextureHandle> frameUploadTextureScratch;
std::array<Mat4, ShadowMap::ClipLevelCount> sceneVsmReceiverMatrices{};
bool sceneVsmReceiverFeedbackValid{};
std::array<VsmFeedbackCache, MAX_FRAMES_IN_FLIGHT> vsmFeedbackCaches;
// Coverage of the depth source, recorded when its Hi-Z is produced.
std::array<VsmFeedbackKey, MAX_FRAMES_IN_FLIGHT> hiZFeedbackKeys;
// Virtual page keys are valid only in the layout that generated them.
std::array<std::array<Mat4, ShadowMap::ClipLevelCount>, MAX_FRAMES_IN_FLIGHT> vsmRequestClipMatrices{};
// Forward+ lists are view-relative: the Game and Scene cameras can
// have different extents and depth partitions in the same frame.
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> clusteredLightRangeBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> clusteredLightIndexBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> sceneClusteredLightRangeBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> sceneClusteredLightIndexBuffers;
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> clusteredLightingUniformBuffers;
// Every point and spot light of the frame, read by cluster culling and forward lighting.
std::array<Buffer, MAX_FRAMES_IN_FLIGHT> localLightBuffers;
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
// Hi-Z is temporal input for the matching frame slot.  Sharing one
// image between slots lets in-flight frames overwrite the hierarchy
// while a later culling dispatch is sampling it.
std::array<Culling::HiZBuffer, MAX_FRAMES_IN_FLIGHT> hiZBuffers;
std::array<Culling::HiZPass, MAX_FRAMES_IN_FLIGHT> hiZPasses;
VkDescriptorPool cullingDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSetLayout hiZCopyDescriptorSetLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout hiZReduceDescriptorSetLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout cullingDescriptorSetLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout instanceCullingDescriptorSetLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout meshletCullingDescriptorSetLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout meshletDispatchDescriptorSetLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout meshletIndirectDescriptorSetLayout = VK_NULL_HANDLE;
// Set 1 of the mesh-shader forward pipelines (MeshletDraw.slang). Created
// once with the forward passes, which embed it in their pipeline layouts.
VkDescriptorSetLayout meshletDrawDescriptorSetLayout = VK_NULL_HANDLE;
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
VkDescriptorSetLayout grassShadowPageCullDescriptorSetLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout clusteredLightingDescriptorSetLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout vsmPageMarkingDescriptorSetLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout vsmPageCompactDescriptorSetLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout shadowMeshletRefineDescriptorSetLayout = VK_NULL_HANDLE;
VkPipelineLayout hiZCopyPipelineLayout = VK_NULL_HANDLE;
VkPipelineLayout hiZReducePipelineLayout = VK_NULL_HANDLE;
VkPipelineLayout cullingPipelineLayout = VK_NULL_HANDLE;
VkPipelineLayout instanceCullingPipelineLayout = VK_NULL_HANDLE;
VkPipelineLayout meshletCullingPipelineLayout = VK_NULL_HANDLE;
VkPipelineLayout meshletDispatchPipelineLayout = VK_NULL_HANDLE;
VkPipelineLayout meshletIndirectPipelineLayout = VK_NULL_HANDLE;
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
VkPipelineLayout grassShadowPageCullPipelineLayout = VK_NULL_HANDLE;
VkPipelineLayout clusteredLightingPipelineLayout = VK_NULL_HANDLE;
VkPipelineLayout vsmPageMarkingPipelineLayout = VK_NULL_HANDLE;
VkPipelineLayout vsmPageCompactPipelineLayout = VK_NULL_HANDLE;
VkPipelineLayout shadowMeshletRefinePipelineLayout = VK_NULL_HANDLE;
VkPipeline hiZCopyPipeline = VK_NULL_HANDLE;
VkPipeline hiZReducePipeline = VK_NULL_HANDLE;
VkPipeline cullingPipeline = VK_NULL_HANDLE;
VkPipeline instanceCullingPipeline = VK_NULL_HANDLE;
VkPipeline meshletCullingPipeline = VK_NULL_HANDLE;
VkPipeline meshletDispatchPipeline = VK_NULL_HANDLE;
VkPipeline meshletIndirectPipeline = VK_NULL_HANDLE;
// meshlet_build_mesh_draws / meshlet_build_mesh_commands; they reuse the
// meshlet-indirect and meshlet-dispatch pipeline layouts.
VkPipeline meshletDrawBinPipeline = VK_NULL_HANDLE;
VkPipeline meshletDrawCommandPipeline = VK_NULL_HANDLE;
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
VkPipeline grassShadowPageCullPipeline = VK_NULL_HANDLE;
VkPipeline clusteredLightingPipeline = VK_NULL_HANDLE;
VkPipeline vsmPageMarkingPipeline = VK_NULL_HANDLE;
VkPipeline vsmPageCompactPipeline = VK_NULL_HANDLE;
VkPipeline shadowMeshletRefinePipeline = VK_NULL_HANDLE;
// Mesh-shader casters: the refine variant listing meshlets, and the pass
// deriving the per-page mesh-task commands from its counts.
VkPipeline shadowMeshletMeshRefinePipeline = VK_NULL_HANDLE;
VkPipeline shadowMeshletMeshCommandPipeline = VK_NULL_HANDLE;
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> instanceCullSets{};
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> meshletCullSets{};
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> meshletDispatchSets{};
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> meshletIndirectSets{};
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> meshletDrawBinSets{};
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> meshletDrawCommandSets{};
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> meshletDrawSets{};
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> clusteredLightingSets{};
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sceneClusteredLightingSets{};
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> vsmPageMarkingSets{};
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> vsmPageCompactSets{};
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> shadowMeshletRefineSets{};
// Set 1 of the mesh-shader caster pipeline (shadow_meshlet_mesh.slang).
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> shadowMeshletDrawSets{};
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedMeshletRefineSets{};
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> shadowTwoSidedMeshletDrawSets{};
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
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> grassShadowPageCullSets{};
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sceneGrassPackedCullSets{};
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sceneGrassBladeCullSets{};
std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sceneGrassClassifySets{};
std::array<std::array<VkDescriptorSet, 3>, MAX_FRAMES_IN_FLIGHT> sceneGrassPackedBinSets{};
std::array<std::array<VkDescriptorSet, 3>, MAX_FRAMES_IN_FLIGHT> sceneGrassPackedPrefixSets{};
std::array<std::array<VkDescriptorSet, 3>, MAX_FRAMES_IN_FLIGHT> sceneGrassPackedScatterSets{};
std::array<std::array<VkDescriptorSet, 3>, MAX_FRAMES_IN_FLIGHT> sceneGrassPackedFinalizeSets{};
std::vector<Culling::GPUObjectData> gpuObjects;
// First command of each material program slot in the binned indirect
// streams, for the single-sided [0] and two-sided/foliage [1] lists. Slots
// are packed back to back, sized by their object counts.
std::array<std::array<std::uint32_t, Culling::DrawBinOffsetCount>, 2> drawBinOffsets{};
// Old and new bounds of renderables whose shadow contribution changed
// in this frame. ShadowPass evicts only overlapping virtual pages
// instead of rebuilding the complete atlas for one moving body/blade.
std::vector<Culling::GPUObjectData> dirtyShadowObjects;
std::uint64_t lastGrassShadowWindRevision{std::numeric_limits<std::uint64_t>::max()};
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
std::vector<std::size_t> changedRenderableIndices;
std::vector<std::size_t> changedRenderableBatches;
std::vector<std::size_t> settledTransformIndices;
std::vector<std::uint8_t> renderableChangeKinds;
std::uint32_t renderableChangeEpoch = 0;
// O(1) ECS-provided revision.  Do not derive this by traversing every
// renderable during synchronization.
std::uint64_t lastRenderTopologyRevision = std::numeric_limits<std::uint64_t>::max();
std::uint64_t lastParticleEmitterRevision = std::numeric_limits<std::uint64_t>::max();
std::uint64_t lastSmokeEmitterRevision = std::numeric_limits<std::uint64_t>::max();
std::array<bool, MAX_FRAMES_IN_FLIGHT> hiZValid{};
std::array<glm::mat4, MAX_FRAMES_IN_FLIGHT> hiZViewProjections{};
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
std::uint64_t renderSettingsRevision = 0;
std::uint64_t sceneViewportRenderedSettingsRevision = 0;
Vec3 renderedSceneViewportPosition{};
float renderedSceneViewportYaw = 0.0F;
float renderedSceneViewportPitch = 0.0F;
Entity editorSelectedEntity = NullEntity;
std::uint32_t editorSelectedRenderable = std::numeric_limits<std::uint32_t>::max();

VkCommandPool commandPool{};
VkCommandPool asyncComputeCommandPool{};
VkCommandPool transferCommandPool{};
UploadContext uploadContext;
std::vector<VkCommandBuffer> commandBuffers;
std::vector<VkCommandBuffer> postViewportGraphicsCommandBuffers;
struct GraphCommandBuffer {
    RenderGraph::Queue queue{};
    VkCommandBuffer buffer{};
};
std::array<std::vector<GraphCommandBuffer>, MAX_FRAMES_IN_FLIGHT> renderGraphCommandBuffers;
std::array<std::vector<GraphCommandBuffer>, MAX_FRAMES_IN_FLIGHT> presentationGraphCommandBuffers;
VkSemaphore renderGraphTimeline{};
std::uint64_t renderGraphTimelineValue{};
VkSemaphore renderGraphComputeTimeline{};
std::uint64_t renderGraphComputeTimelineValue{};
VkSemaphore renderGraphTransferTimeline{};
std::uint64_t renderGraphTransferTimelineValue{};
bool presentationGraphSubmittedThisFrame{};
// Set once vkAcquireNextImageKHR has succeeded for the current frame slot.
bool swapchainImageAcquired{};
std::uint32_t acquiredImageIndex{};
// Final timeline values of the viewport graph, joined by the presentation submission.
std::array<std::uint64_t, 3> viewportGraphCompletion{};

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
GpuRetirementQueue gpuRetirementQueue;
std::uint64_t taaSampleIndex = 0;
float taaJitterX = 0.0F;
float taaJitterY = 0.0F;
bool taaResolveActive = false;
Mat4 previousGameView{};
Mat4 previousGameProjection{};
Mat4 particlePreviousGameViewProjection{};
Entity previousGameCamera{NullEntity};
Vec3 previousGameCameraPosition{};
Vec3 previousGameCameraForward{};
Vec3 previousGameCameraRight{1.0F, 0.0F, 0.0F};
Vec3 previousGameCameraUp{0.0F, 1.0F, 0.0F};
Vec3 particlePreviousGameCameraRight{1.0F, 0.0F, 0.0F};
Vec3 particlePreviousGameCameraUp{0.0F, 1.0F, 0.0F};
bool previousGameCameraValid = false;

bool framebufferResized = false;
bool cleanedUp = false;

uint32_t fpsFrameCount = 0;
double fpsElapsedTime = 0.0;

static constexpr uint8_t allFrameBits = static_cast<uint8_t>((1u << MAX_FRAMES_IN_FLIGHT) - 1u);

[[nodiscard]] static uint8_t frameBit(const uint32_t frame) noexcept {
    return static_cast<uint8_t>(1U << frame);
}

[[nodiscard]] static bool sameTransform(const Transform& lhs, const Transform& rhs) noexcept {
    // NOLINTNEXTLINE(readability-identifier-length)
    const auto sameVector = [](const Vec3& a, const Vec3& b) {
        return a.x() == b.x() && a.y() == b.y() && a.z() == b.z();
    };
    return sameVector(lhs.position, rhs.position) && sameVector(lhs.rotation, rhs.rotation) &&
           sameVector(lhs.scale, rhs.scale);
}

static void copySceneBackground(VkCommandBuffer buffer, VkImage source, VkImage snapshot, VkExtent2D extent,
                                VkImageLayout sourceLayout) {
    VkImageMemoryBarrier2 before[2]{};
    for (auto& barrier : before) {
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        // The scene colour and its snapshot are only produced or
        // consumed by raster, compute and transfer work.
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    }
    before[0].image = source;
    before[0].oldLayout = sourceLayout;
    before[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    before[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    before[1].image = snapshot;
    before[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    before[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    before[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 2;
    dependency.pImageMemoryBarriers = before;
    vkCmdPipelineBarrier2(buffer, &dependency);
    const VkImageCopy copy{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           {0, 0, 0},
                           {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           {0, 0, 0},
                           {extent.width, extent.height, 1}};
    vkCmdCopyImage(buffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snapshot, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &copy);
    for (auto& barrier : before) {
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = barrier.dstAccessMask;
        barrier.oldLayout = barrier.newLayout;
    }
    before[0].newLayout = sourceLayout;
    before[0].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    before[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    before[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    before[1].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    before[1].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    vkCmdPipelineBarrier2(buffer, &dependency);
}

[[nodiscard]] static bool sameMaterial(const GPUMaterialData& lhs, const GPUMaterialData& rhs) noexcept {
    return lhs.baseColorMetallic.x == rhs.baseColorMetallic.x && lhs.baseColorMetallic.y == rhs.baseColorMetallic.y &&
           lhs.baseColorMetallic.z == rhs.baseColorMetallic.z && lhs.baseColorMetallic.w == rhs.baseColorMetallic.w &&
           lhs.roughnessAmbientOcclusion.x == rhs.roughnessAmbientOcclusion.x &&
           lhs.roughnessAmbientOcclusion.y == rhs.roughnessAmbientOcclusion.y &&
           lhs.roughnessAmbientOcclusion.z == rhs.roughnessAmbientOcclusion.z &&
           lhs.roughnessAmbientOcclusion.w == rhs.roughnessAmbientOcclusion.w &&
           lhs.textureIndices.x == rhs.textureIndices.x && lhs.textureIndices.y == rhs.textureIndices.y &&
           lhs.textureIndices.z == rhs.textureIndices.z && lhs.textureIndices.w == rhs.textureIndices.w &&
           lhs.terrainLayerTextures.x == rhs.terrainLayerTextures.x &&
           lhs.terrainLayerTextures.y == rhs.terrainLayerTextures.y &&
           lhs.terrainLayerTextures.z == rhs.terrainLayerTextures.z &&
           lhs.terrainLayerTextures.w == rhs.terrainLayerTextures.w &&
           lhs.auxiliaryTextureIndices.x == rhs.auxiliaryTextureIndices.x &&
           lhs.auxiliaryTextureIndices.y == rhs.auxiliaryTextureIndices.y &&
           lhs.auxiliaryTextureIndices.z == rhs.auxiliaryTextureIndices.z &&
           lhs.auxiliaryTextureIndices.w == rhs.auxiliaryTextureIndices.w &&
           lhs.extensionScalars.x == rhs.extensionScalars.x && lhs.extensionScalars.y == rhs.extensionScalars.y &&
           lhs.extensionScalars.z == rhs.extensionScalars.z && lhs.extensionScalars.w == rhs.extensionScalars.w &&
           lhs.displacementParams == rhs.displacementParams &&
           lhs.extensionTextureIndices.x == rhs.extensionTextureIndices.x &&
           lhs.extensionTextureIndices.y == rhs.extensionTextureIndices.y &&
           lhs.extensionTextureIndices.z == rhs.extensionTextureIndices.z &&
           lhs.extensionTextureIndices.w == rhs.extensionTextureIndices.w && lhs.specularColor == rhs.specularColor &&
           lhs.emissiveColorIntensity.x == rhs.emissiveColorIntensity.x &&
           lhs.emissiveColorIntensity.y == rhs.emissiveColorIntensity.y &&
           lhs.emissiveColorIntensity.z == rhs.emissiveColorIntensity.z &&
           lhs.emissiveColorIntensity.w == rhs.emissiveColorIntensity.w &&
           lhs.textureCoordinateSets0 == rhs.textureCoordinateSets0 &&
           lhs.textureCoordinateSets1 == rhs.textureCoordinateSets1 &&
           lhs.textureCoordinateSets2 == rhs.textureCoordinateSets2 && lhs.textureTransforms == rhs.textureTransforms &&
           lhs.textureTransformRows1 == rhs.textureTransformRows1 &&
           lhs.waterShallowColorRoughness == rhs.waterShallowColorRoughness &&
           lhs.waterDeepColorIor == rhs.waterDeepColorIor &&
           lhs.waterAbsorptionRefraction == rhs.waterAbsorptionRefraction &&
           lhs.waterScatteringMaxDepth == rhs.waterScatteringMaxDepth && lhs.waterFoam == rhs.waterFoam &&
           lhs.waterTextureIndices == rhs.waterTextureIndices && lhs.waterWaves == rhs.waterWaves &&
           lhs.waterWaveMotion == rhs.waterWaveMotion && lhs.waterWaveCount == rhs.waterWaveCount &&
           lhs.glassParams == rhs.glassParams && lhs.glassAttenuation == rhs.glassAttenuation &&
           lhs.glassTextureIndices == rhs.glassTextureIndices &&
           lhs.glassTextureCoordinateSets == rhs.glassTextureCoordinateSets &&
           lhs.glassTextureTransforms == rhs.glassTextureTransforms && lhs.glassTextureRows1 == rhs.glassTextureRows1;
}

void markDirty(
    const std::size_t index, uint8_t RenderableRecord::* dirtyFrames,
    std::array<std::vector<std::size_t>, MAX_FRAMES_IN_FLIGHT>& dirtyIndices) { // NOLINT(readability-named-parameter)
    RenderableRecord& record = renderables[index];
    ++depthContentRevision;
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        const uint8_t bit = frameBit(frame);
        if ((record.*dirtyFrames & bit) == 0) {
            dirtyIndices[frame].push_back(index);
        }
    }
    record.*dirtyFrames |= allFrameBits;
}

