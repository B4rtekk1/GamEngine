#pragma once
Renderer::~Renderer() { shutdown(); }

Renderer::Renderer(RenderConfig config)
    : optimizationFeatures_(config.features), antialiasingLevel_(config.antialiasing),
      shadowQuality_(config.shadowQuality),
      shadowDebugView_(config.shadowDebugView),
      grassSettings_(config.grass),
      state_(std::make_unique<State>()) {}

void Renderer::setOptimizationFeatures(RenderOptimizationFeatures features) noexcept {
    optimizationFeatures_ = features;
}

const RenderOptimizationFeatures& Renderer::optimizationFeatures() const noexcept {
    return optimizationFeatures_;
}

void Renderer::setAntialiasingLevel(AntialiasingLevel level) noexcept {
    antialiasingLevel_ = level;
}

AntialiasingLevel Renderer::antialiasingLevel() const noexcept {
    return antialiasingLevel_;
}

void Renderer::setShadowQuality(const ShadowQuality quality) noexcept {
    shadowQuality_ = quality;
}

ShadowQuality Renderer::shadowQuality() const noexcept {
    return shadowQuality_;
}

void Renderer::setShadowDebugView(const ShadowDebugView view) noexcept {
    shadowDebugView_ = view;
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

std::vector<GpuSceneMemoryAllocation> Renderer::gpuSceneMemory() const {
    return backend_ ? backend_->gpuSceneMemory() : std::vector<GpuSceneMemoryAllocation>{};
}

void Renderer::initializeCore(Scene& scene, void* nativeWindow) {
    auto* window = static_cast<SDL_Window*>(nativeWindow);
    if (backend_) throw std::logic_error("Renderer is already initialized");
    backend_ = std::make_unique<Backend>(scene, window, optimizationFeatures_, antialiasingLevel_, shadowQuality_, shadowDebugView_, grassSettings_,
                                         state_->assetManager, state_->forwardPass, state_->skyPass,
                                         state_->tonemapPass, state_->temporalAaPass, state_->bloomPass, state_->gtaoPass,
                                         state_->particlePipeline,
                                         state_->canvasRenderer);
    backend_->initializeCore();
}

void Renderer::initializeScene(Scene& scene) {
    if (!backend_) throw std::logic_error("Renderer core must be initialized first");
    backend_->initializeSceneResources(scene);
}

void Renderer::initialize(Scene& scene, void* nativeWindow) {
    initializeCore(scene, nativeWindow);
    initializeScene(scene);
}

void Renderer::beginFrame() { Backend::beginFrame(); }
EditorEventState Renderer::pollEditorEvents() const {
    return backend_ ? backend_->pollEditorEvents() : EditorEventState{};
}
void Renderer::beginEditorUiFrame() const { backend_->beginEditorUiFrame(); }
void Renderer::processEvent(const void* nativeEvent) const {
    if (backend_) backend_->processEvent(*static_cast<const SDL_Event*>(nativeEvent));
}

void Renderer::setEditorSceneCameraInput(const bool active) const {
    if (backend_) backend_->setEditorSceneCameraInput(active);
}

void Renderer::setGameCameraInput(const bool active) const {
    if (backend_) backend_->setGameCameraInput(active);
}
void Renderer::requestGameMouseCapture() const {
    if (backend_) backend_->requestGameMouseCapture();
}
void Renderer::setSceneViewportActive(const bool active) const {
    if (backend_) backend_->setSceneViewportActive(active);
}
void Renderer::setSceneViewportExtent(const std::uint32_t width, const std::uint32_t height) const {
    if (backend_) backend_->setSceneViewportExtent(width, height);
}
void Renderer::updateEditorSceneCameraInput() const {
    if (backend_) backend_->updateEditorSceneCameraInput();
}
void Renderer::setEditorSelection(const Entity entity) const {
    if (backend_) backend_->setEditorSelection(entity);
}
bool Renderer::setEnvironmentEquirectangular(const std::filesystem::path& path) const {
    return backend_ && backend_->setEnvironmentEquirectangular(path);
}
void Renderer::bakeReflectionProbe(const Entity entity) const {
    if (backend_) backend_->bakeReflectionProbe(entity);
}
void Renderer::renderFrame() const { backend_->renderFrame(); }
void Renderer::synchronizeScene(Scene& scene) const {
    if (!backend_) return;
    if (!backend_->sceneResourcesReady()) backend_->initializeSceneResources(scene);
    else backend_->synchronizeSceneResources(scene);
}
void Renderer::updateMeshGeometry(const Entity entity, const std::uint32_t firstVertex,
                                  const std::uint32_t vertexCount) const {
    if (backend_) backend_->updateMeshGeometry(entity, firstVertex, vertexCount);
}
void Renderer::reloadScene(Scene& scene, void* nativeWindow) {
    static_cast<void>(nativeWindow);
    if (backend_) {
        if (!backend_->sceneResourcesReady()) {
            backend_->initializeSceneResources(scene);
            return;
        }
        if (backend_->antialiasingLevel != antialiasingLevel_) {
            backend_->reconfigureAntialiasing(antialiasingLevel_);
        }
        // Scene snapshots replace the complete ECS registry. Incremental
        // synchronization is only valid for edits made against the current
        // registry; it can otherwise retain GPU-scene IDs and buffer
        // capacities from the discarded registry. Rebuild every
        // registry-derived resource before the next render frame.
        backend_->reloadSceneResources(scene);
    } else initialize(scene, nativeWindow);
}
void Renderer::reconfigureAntialiasing() const {
    if (!backend_) return;
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
    if (backend_) backend_->setEditorCameraRotation(yaw, pitch);
}
void Renderer::setEditorCameraPosition(const Vec3 position) const noexcept {
    if (backend_) backend_->setEditorCameraPosition(position);
}
void Renderer::shutdown() noexcept {
    backend_.reset();
    if (ImGui::GetCurrentContext() != nullptr &&
        ImGui::GetIO().BackendPlatformUserData != nullptr) {
        ImGui_ImplSDL3_Shutdown();
    }
}
