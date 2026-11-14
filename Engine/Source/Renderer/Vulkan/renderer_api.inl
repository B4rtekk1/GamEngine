#pragma once
Renderer::~Renderer() {
    shutdown();
}

Renderer::Renderer(RenderConfig config)
    : optimizationFeatures_(config.features), renderPath_(config.renderPath), antialiasingLevel_(config.antialiasing),
      shadowQuality_(config.shadowQuality), rtContactShadowSettings_(config.rtContactShadows),
      gtaoQuality_(config.gtaoQuality), gtaoDebugView_(config.gtaoDebugView), pbrDebugView_(config.pbrDebugView),
      iblQuality_(config.iblQuality), shadowDebugView_(config.shadowDebugView), grassSettings_(config.grass),
      ssgiSettings_(config.ssgi), ddgiSettings_(config.ddgi), postProcessSettings_(config.postProcess),
      configuredPostProcess_(config.postProcess), state_(std::make_unique<State>()) {
    configuredGlobalIllumination_ = globalIllumination();
}

void Renderer::setOptimizationFeatures(RenderOptimizationFeatures features) noexcept {
    if (optimizationFeatures_.shadows == features.shadows &&
        optimizationFeatures_.instancedRendering == features.instancedRendering &&
        optimizationFeatures_.meshDeduplication == features.meshDeduplication &&
        optimizationFeatures_.transformCaching == features.transformCaching &&
        optimizationFeatures_.materialCaching == features.materialCaching &&
        optimizationFeatures_.gpuCulling == features.gpuCulling &&
        optimizationFeatures_.occlusionCulling == features.occlusionCulling)
        return;
    optimizationFeatures_ = features;
    if (backend_)
        backend_->markRenderSettingsChanged();
}

const RenderOptimizationFeatures& Renderer::optimizationFeatures() const noexcept {
    return optimizationFeatures_;
}

void Renderer::setRenderPath(const RenderPath path) noexcept {
    if (renderPath_ == path)
        return;
    renderPath_ = path;
    if (backend_)
        backend_->markRenderSettingsChanged();
}

RenderPath Renderer::renderPath() const noexcept {
    return renderPath_;
}

void Renderer::setAntialiasingLevel(AntialiasingLevel level) noexcept {
    if (antialiasingLevel_ == level)
        return;
    antialiasingLevel_ = level;
    if (backend_)
        backend_->markRenderSettingsChanged();
}

AntialiasingLevel Renderer::antialiasingLevel() const noexcept {
    return antialiasingLevel_;
}

void Renderer::setShadowQuality(const ShadowQuality quality) noexcept {
    if (shadowQuality_ == quality)
        return;
    shadowQuality_ = quality;
    if (backend_)
        backend_->markRenderSettingsChanged();
}

ShadowQuality Renderer::shadowQuality() const noexcept {
    return shadowQuality_;
}

void Renderer::setContactShadowMode(const ContactShadowMode mode) noexcept {
    if (rtContactShadowSettings_.mode == mode)
        return;
    rtContactShadowSettings_.mode = mode;
    if (backend_)
        backend_->markRenderSettingsChanged();
}

ContactShadowMode Renderer::contactShadowMode() const noexcept {
    return rtContactShadowSettings_.mode;
}

bool Renderer::contactShadowsActive() const noexcept {
    return backend_ && backend_->rtContactActive;
}

void Renderer::setRtContactShadowSettings(const RtContactShadowSettings& settings) noexcept {
    if (rtContactShadowSettings_.mode == settings.mode &&
        rtContactShadowSettings_.maxDistance == settings.maxDistance &&
        rtContactShadowSettings_.normalBias == settings.normalBias &&
        rtContactShadowSettings_.resolutionScale == settings.resolutionScale)
        return;
    rtContactShadowSettings_ = settings;
    if (backend_)
        backend_->markRenderSettingsChanged();
}

const RtContactShadowSettings& Renderer::rtContactShadowSettings() const noexcept {
    return rtContactShadowSettings_;
}

void Renderer::applyRenderQualityPreset(const RenderQualityPreset preset) noexcept {
    const RenderQualityPresetSettings settings = renderQualityPresetSettings(preset);
    if (shadowQuality_ == settings.shadows && gtaoQuality_ == settings.gtao &&
        antialiasingLevel_ == settings.antialiasing)
        return;
    shadowQuality_ = settings.shadows;
    gtaoQuality_ = settings.gtao;
    antialiasingLevel_ = settings.antialiasing;
    if (backend_)
        backend_->markRenderSettingsChanged();
}

void Renderer::setGtaoQuality(const GtaoQuality quality) noexcept {
    if (gtaoQuality_ == quality)
        return;
    gtaoQuality_ = quality;
    if (backend_)
        backend_->markRenderSettingsChanged();
}
GtaoQuality Renderer::gtaoQuality() const noexcept {
    return gtaoQuality_;
}
void Renderer::setGtaoDebugView(const GtaoDebugView view) noexcept {
    if (gtaoDebugView_ == view)
        return;
    gtaoDebugView_ = view;
    if (backend_)
        backend_->markRenderSettingsChanged();
}
GtaoDebugView Renderer::gtaoDebugView() const noexcept {
    return gtaoDebugView_;
}
// SSGI resources always exist at the swapchain size, so these settings are
// read every frame and never recreate GPU objects.
void Renderer::setSsgiSettings(const SsgiSettings& settings) noexcept {
    ssgiSettings_ = settings;
}
const SsgiSettings& Renderer::ssgiSettings() const noexcept {
    return ssgiSettings_;
}
void Renderer::setDdgiSettings(const DdgiSettings& settings) noexcept {
    ddgiSettings_ = settings;
}
const DdgiSettings& Renderer::ddgiSettings() const noexcept {
    return ddgiSettings_;
}
bool Renderer::ddgiSupported() const noexcept {
    return backend_ && backend_->ddgiPass.available();
}
void Renderer::setGlobalIllumination(const GlobalIlluminationSettings& settings) noexcept {
    ssgiSettings_ = settings.ssgi;
    ddgiSettings_ = settings.ddgi;
    ddgiSettings_.enabled = settings.mode == GlobalIlluminationMode::RayTracedProbes;
    // SSGI also stands in when DDGI is unsupported; Off disables both.
    if (settings.mode == GlobalIlluminationMode::Off)
        ssgiSettings_.quality = SsgiQuality::Off;
    else if (settings.mode == GlobalIlluminationMode::ScreenSpace && ssgiSettings_.quality == SsgiQuality::Off)
        ssgiSettings_.quality = SsgiQuality::Medium;
}
GlobalIlluminationSettings Renderer::globalIllumination() const noexcept {
    GlobalIlluminationSettings settings{.ssgi = ssgiSettings_, .ddgi = ddgiSettings_};
    settings.mode = ddgiSettings_.enabled                  ? GlobalIlluminationMode::RayTracedProbes
                    : ssgiSettings_.quality == SsgiQuality::Off ? GlobalIlluminationMode::Off
                                                                : GlobalIlluminationMode::ScreenSpace;
    settings.ddgi.enabled = false;
    return settings;
}
// Read every frame by the tonemap pass; no GPU object depends on them.
void Renderer::setPostProcessSettings(const PostProcessSettings& settings) noexcept {
    postProcessSettings_ = settings;
}
const PostProcessSettings& Renderer::postProcessSettings() const noexcept {
    return postProcessSettings_;
}
void Renderer::applySceneSettings(const Scene& scene) noexcept {
    setGlobalIllumination(scene.globalIllumination().value_or(configuredGlobalIllumination_));
    setPostProcessSettings(scene.postProcess().value_or(configuredPostProcess_));
}
void Renderer::requestHdrCapture() noexcept {
    if (backend_)
        backend_->hdrCaptureRequested = true;
}
bool Renderer::takeHdrCapture(std::uint32_t& width, std::uint32_t& height, std::vector<std::uint16_t>& rgbaHalf) {
    if (!backend_ || !backend_->hdrCaptureReady)
        return false;
    width = backend_->hdrCaptureExtent.width;
    height = backend_->hdrCaptureExtent.height;
    rgbaHalf = std::move(backend_->hdrCapturePixels);
    backend_->hdrCaptureReady = false;
    return true;
}
void Renderer::requestFrameCapture() noexcept {
    if (backend_)
        backend_->frameCaptureRequested = true;
}
bool Renderer::takeFrameCapture(std::uint32_t& width, std::uint32_t& height, std::vector<std::uint8_t>& rgba) {
    if (!backend_ || !backend_->frameCaptureReady)
        return false;
    width = backend_->frameCaptureExtent.width;
    height = backend_->frameCaptureExtent.height;
    rgba = std::move(backend_->frameCapturePixels);
    backend_->frameCaptureReady = false;
    return true;
}
VsmStatistics Renderer::vsmStatistics() const noexcept {
    if (!backend_)
        return {};
    const auto& stats = backend_->shadowPass.cacheStatistics();
    return {.newPages = stats.newPages,
            .refreshedPages = stats.refreshedPages,
            .dirtyMarkedPages = stats.dirtyMarkedPages,
            .levelsScrolled = stats.levelsScrolled,
            .levelsInvalidated = stats.levelsInvalidated,
            .deferredRequests = stats.deferredRequests,
            .residentPages = stats.residentPages,
            .pageBudget = backend_->adaptiveShadowPageBudget};
}
DdgiStatistics Renderer::ddgiStatistics() const noexcept {
    if (!backend_)
        return {};
    const auto& scene = backend_->clusterScene.memoryStats();
    return {.tracedProbes = backend_->ddgiPass.tracedProbes(),
            .totalProbes = DdgiPass::ProbeCount,
            .sceneBytesBuilt = scene.builtBytes,
            .sceneBytesCompacted = scene.compactedBytes,
            .clusters = scene.clusters,
            .clusterTemplates = scene.templates,
            .displacedClusters = scene.displacedClusters,
            .clusterBlases = scene.blases,
            .compacted = scene.compacted};
}
void Renderer::setPbrDebugView(const PbrDebugView view) noexcept {
    if (pbrDebugView_ == view)
        return;
    pbrDebugView_ = view;
    if (backend_)
        backend_->markRenderSettingsChanged();
}
PbrDebugView Renderer::pbrDebugView() const noexcept {
    return pbrDebugView_;
}
void Renderer::setIblQuality(const IblQuality quality) noexcept {
    if (iblQuality_ == quality)
        return;
    iblQuality_ = quality;
    if (backend_)
        backend_->markRenderSettingsChanged();
}
IblQuality Renderer::iblQuality() const noexcept {
    return iblQuality_;
}

void Renderer::setShadowDebugView(const ShadowDebugView view) noexcept {
    if (shadowDebugView_ == view)
        return;
    shadowDebugView_ = view;
    if (backend_)
        backend_->markRenderSettingsChanged();
}

ShadowDebugView Renderer::shadowDebugView() const noexcept {
    return shadowDebugView_;
}

std::optional<GpuProfileFrame> Renderer::gpuProfile() const noexcept {
    return backend_ ? backend_->gpuProfile() : std::nullopt;
}

std::vector<GpuMemoryHeapBudget> Renderer::gpuMemoryHeaps() const {
    return backend_ ? backend_->gpuMemoryHeaps() : std::vector<GpuMemoryHeapBudget>{};
}

std::vector<GpuMemoryCategoryBudget> Renderer::gpuMemoryCategories() const {
    return backend_ ? backend_->gpuMemoryCategories() : std::vector<GpuMemoryCategoryBudget>{};
}

std::string Renderer::gpuMemoryAllocationsJson() const {
    return backend_ ? backend_->gpuMemoryAllocationsJson() : std::string{};
}

std::vector<GpuSceneMemoryAllocation> Renderer::gpuSceneMemory() const {
    return backend_ ? backend_->gpuSceneMemory() : std::vector<GpuSceneMemoryAllocation>{};
}

void Renderer::setProjectRoot(std::filesystem::path root) {
    state_->projectRoot = std::move(root).lexically_normal();
    // The editor switches projects without recreating the renderer. Project
    // content and caches (environment, IBL in Library/) are resolved through
    // this path whenever scene resources are rebuilt, so the next scene load
    // uses the new project. The Vulkan pipeline cache keeps the directory it
    // was opened with; its contents do not depend on the project.
    if (backend_) {
        backend_->projectRoot = state_->projectRoot;
        // IES light profiles resolve against the project's assets.
        backend_->iesProfileCache.clear();
    }
}

void Renderer::setEditorUiBackend(EditorUiBackend* backend) noexcept {
    if (backend_)
        return;
    editorUiBackend_ = backend;
}

void Renderer::initializeCore(Scene& scene, void* nativeWindow) {
    auto* window = static_cast<SDL_Window*>(nativeWindow);
    if (backend_)
        throw std::logic_error("Renderer is already initialized");
    backend_ = std::make_unique<Backend>(
        scene, window, optimizationFeatures_, renderPath_, antialiasingLevel_, shadowQuality_, rtContactShadowSettings_,
        gtaoQuality_, gtaoDebugView_, pbrDebugView_, iblQuality_, shadowDebugView_, grassSettings_, ssgiSettings_,
        ddgiSettings_, postProcessSettings_, state_->projectRoot,
        state_->assetManager, state_->forwardPass, state_->skyPass, state_->tonemapPass, state_->temporalAaPass,
        state_->bloomPass, state_->gtaoPass, state_->particlePipeline, state_->canvasRenderer, editorUiBackend_);
    backend_->initializeCore();
}

void Renderer::initializeScene(Scene& scene) {
    if (!backend_)
        throw std::logic_error("Renderer core must be initialized first");
    applySceneSettings(scene);
    backend_->initializeSceneResources(scene);
}

void Renderer::initialize(Scene& scene, void* nativeWindow) {
    initializeCore(scene, nativeWindow);
    initializeScene(scene);
}

void Renderer::beginFrame() {
    Backend::beginFrame();
}
EditorEventState Renderer::pollEditorEvents() const {
    return backend_ ? backend_->pollEditorEvents() : EditorEventState{};
}
void Renderer::beginEditorUiFrame() const {
    backend_->beginEditorUiFrame();
}
void Renderer::processEvent(const void* nativeEvent) const {
    if (backend_)
        backend_->processEvent(*static_cast<const SDL_Event*>(nativeEvent));
}

void Renderer::setEditorSceneCameraInput(const bool active) const {
    if (backend_)
        backend_->setEditorSceneCameraInput(active);
}

void Renderer::setGameCameraInput(const bool active) const {
    if (backend_)
        backend_->setGameCameraInput(active);
}
void Renderer::setSceneViewportActive(const bool active) const {
    if (backend_)
        backend_->setSceneViewportActive(active);
}
void Renderer::setSceneViewportExtent(const std::uint32_t width, const std::uint32_t height) const {
    if (backend_)
        backend_->setSceneViewportExtent(width, height);
}
void Renderer::updateEditorSceneCameraInput() const {
    if (backend_)
        backend_->updateEditorSceneCameraInput();
}
void Renderer::setEditorSelection(const Entity entity) const {
    if (backend_)
        backend_->setEditorSelection(entity);
}
bool Renderer::setEnvironmentEquirectangular(const std::filesystem::path& path) const {
    return backend_ && backend_->setEnvironmentEquirectangular(path);
}
void Renderer::bakeReflectionProbe(const Entity entity) const {
    if (backend_)
        backend_->bakeReflectionProbe(entity);
}
void Renderer::addWaterInteraction(const Vec3 worldPosition, const float radius, const float strength) const {
    if (backend_)
        backend_->addWaterInteraction(worldPosition, radius, strength);
}
void Renderer::renderFrame() const {
    backend_->renderFrame();
}
void Renderer::synchronizeScene(Scene& scene) const {
    if (!backend_)
        return;
    if (!backend_->sceneResourcesReady())
        backend_->initializeSceneResources(scene);
    else
        backend_->synchronizeSceneResources(scene);
}
void Renderer::updateMeshGeometry(const Entity entity, const std::uint32_t firstVertex,
                                  const std::uint32_t vertexCount) const {
    if (backend_)
        backend_->updateMeshGeometry(entity, firstVertex, vertexCount);
}
void Renderer::reloadScene(Scene& scene, void* nativeWindow) {
    static_cast<void>(nativeWindow);
    applySceneSettings(scene);
    if (backend_) {
        if (!backend_->sceneResourcesReady()) {
            backend_->initializeSceneResources(scene);
            return;
        }
        if (backend_->antialiasingLevel != antialiasingLevel_ || backend_->configuredGtaoQuality != gtaoQuality_) {
            backend_->reconfigureAntialiasing(antialiasingLevel_);
        }
        // Scene snapshots replace the complete ECS registry. Incremental
        // synchronization is only valid for edits made against the current
        // registry; it can otherwise retain GPU-scene IDs and buffer
        // capacities from the discarded registry. Rebuild every
        // registry-derived resource before the next render frame.
        backend_->reloadSceneResources(scene);
    } else
        initialize(scene, nativeWindow);
}
void Renderer::reconfigureAntialiasing() const {
    if (!backend_)
        return;
    backend_->reconfigureAntialiasing(antialiasingLevel_);
}
bool Renderer::reloadShaders() const {
    return backend_ && backend_->reloadShaders();
}
ViewportHandle Renderer::gameViewport() const noexcept {
    return {reinterpret_cast<std::uintptr_t>(backend_ ? backend_->gameViewportTexture() : VK_NULL_HANDLE)};
}
ViewportHandle Renderer::sceneViewport() const noexcept {
    return {reinterpret_cast<std::uintptr_t>(backend_ ? backend_->sceneViewportTexture() : VK_NULL_HANDLE)};
}
float Renderer::editorCameraYaw() const noexcept {
    return backend_ ? backend_->editorCameraYaw() : 0.0F;
}
float Renderer::editorCameraPitch() const noexcept {
    return backend_ ? backend_->editorCameraPitch() : 0.0F;
}
Vec3 Renderer::editorCameraPosition() const noexcept {
    return backend_ ? backend_->editorCameraPosition() : Vec3{};
}
Vec3 Renderer::editorGizmoPosition(const Entity entity) const noexcept {
    return backend_ ? backend_->editorGizmoPosition(entity) : Vec3{};
}
void Renderer::setEditorCameraRotation(const float yaw, const float pitch) const noexcept {
    if (backend_)
        backend_->setEditorCameraRotation(yaw, pitch);
}
void Renderer::setEditorCameraPosition(const Vec3 position) const noexcept {
    if (backend_)
        backend_->setEditorCameraPosition(position);
}
void Renderer::shutdown() noexcept {
    backend_.reset();
}
