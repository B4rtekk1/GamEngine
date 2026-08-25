Renderer::~Renderer() = default;

Renderer::Renderer(RenderConfig config)
    : optimizationFeatures_(config.features), antialiasingLevel_(config.antialiasing),
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

void Renderer::initialize(Scene& scene, void* nativeWindow) {
    auto* window = static_cast<SDL_Window*>(nativeWindow);
    if (backend_) throw std::logic_error("Renderer is already initialized");
    backend_ = std::make_unique<Backend>(scene, window, optimizationFeatures_, antialiasingLevel_,
                                         state_->assetManager, state_->forwardPass, state_->skyPass,
                                         state_->tonemapPass, state_->particlePipeline,
                                         state_->canvasRenderer);
    backend_->initialize();
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
void Renderer::updateEditorSceneCameraInput() const {
    if (backend_) backend_->updateEditorSceneCameraInput();
}
void Renderer::setEditorSelection(const Entity entity) const {
    if (backend_) backend_->setEditorSelection(entity);
}
void Renderer::renderFrame() const { backend_->renderFrame(); }
void Renderer::synchronizeScene(Scene& scene) const {
    if (backend_) backend_->synchronizeSceneResources(scene);
}
void Renderer::reloadScene(Scene& scene, void* nativeWindow) {
    static_cast<void>(nativeWindow);
    if (backend_) {
        if (backend_->antialiasingLevel != antialiasingLevel_) {
            backend_->reconfigureAntialiasing(antialiasingLevel_);
        }
        backend_->reloadSceneResources(scene);
    } else initialize(scene, nativeWindow);
}
void Renderer::reconfigureAntialiasing() const {
    if (!backend_) return;
    backend_->reconfigureAntialiasing(antialiasingLevel_);
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
void Renderer::shutdown() noexcept {
    backend_.reset();
    if (ImGui::GetCurrentContext() != nullptr &&
        ImGui::GetIO().BackendPlatformUserData != nullptr) {
        ImGui_ImplSDL3_Shutdown();
    }
}
