#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imnodes.h"

#include "Engine/Renderer/Renderer.h"
#include "Engine/Assets/Content.h"
#include "Engine/Assets/AssetTypes.h"
#include "Engine/Scene/ScenePresets.h"
#include "Engine/Scene/SceneEditor.h"
#include "Engine/Core/Time.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Core/Diagnostics.h"
#include "Engine/Core/Transform.h"
#include "Engine/Core/Camera.h"
#include "Engine/Math/AABB.h"
#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/ECS/Components/TerrainGrassComponent.h"
#include "Engine/ECS/Components/SmokeEmitterComponent.h"
#include "Engine/ECS/Components/ProceduralCloudComponent.h"
#include "Engine/Renderer/Geometry/ProceduralCloud.h"
#include "Engine/Scene/Components/LightComponent.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Renderer/ShaderGraph/ShaderGraphVulkan.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptSystem.h"
#include "Engine/Scripting/ScriptRegistry.h"
#include "Engine/Scripting/ScriptModuleManager.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Project.h"
#include "Elements/EditorButton.h"
#include "Elements/NumericControl.h"
#include "Elements/TransformFields.h"
#include "Editor/Panels/EditorSceneSession.h"
#include "Editor/Panels/EditorStyle.h"
#include "Editor/Panels/ProfilerPanel.h"
#include "Editor/Panels/HierarchyPanel.h"
#include "Editor/Panels/ComponentsPanel.h"
#include "Editor/Panels/AssetManagerPanel.h"
#include "Editor/Panels/ConsolePanel.h"
#include "Editor/Panels/TerminalPanel.h"
#include "Editor/Panels/ShaderGraphPanel.h"
#include "Editor/Panels/AssetDragDrop.h"
#include "Engine/Renderer/ShaderGraph/ShaderNodeFactory.h"
#include "Engine/Renderer/ShaderGraph/ShaderGraphSerializer.h"
#include "Editor/App/EditorEntityHelpers.h"
#include "Editor/EditorState.h"
#include "Editor/EditorPreferences.h"
#include "Editor/EditorConstants.h"
#include "Editor/EditorUi.h"
#include "Editor/TerrainSculptState.h"
#include "ScriptHotReload.h"
#include "ShaderHotReload.h"
#include "Platform/UserPaths.h"

using Editor::EntityClipboard;
using Editor::SceneHistory;

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "EditorViewport.inl"

#include "TerrainToolsPanel.inl"

#include "EditorShell.inl"

namespace {
    [[nodiscard]] std::filesystem::path executableDirectory() {
        const char* const basePath = SDL_GetBasePath();
        if (basePath == nullptr) throw std::runtime_error("Could not determine executable directory");
        return std::filesystem::path{basePath};
    }

    [[nodiscard]] std::optional<std::filesystem::path> findDevelopmentBuildDirectory(
        const std::filesystem::path& executableDirectory) {
        for (auto directory = executableDirectory; !directory.empty(); directory = directory.parent_path()) {
            if (std::filesystem::is_regular_file(directory / "CMakeCache.txt")) return directory;
            if (directory == directory.root_path()) break;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::filesystem::path> findEngineShaderDirectory(
        const std::filesystem::path& buildDirectory) {
        for (auto directory = std::filesystem::current_path(); !directory.empty(); directory = directory.parent_path()) {
            if (std::filesystem::is_directory(directory / "Engine" / "Shaders")) return directory / "Engine" / "Shaders";
            if (directory == directory.root_path()) break;
        }
        for (auto directory = buildDirectory.parent_path(); !directory.empty(); directory = directory.parent_path()) {
            if (std::filesystem::is_directory(directory / "Engine" / "Shaders")) return directory / "Engine" / "Shaders";
            if (directory == directory.root_path()) break;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::filesystem::path findShaderGraphSourceDirectory(
        const std::filesystem::path& editorRoot) {
        const auto bundled = editorRoot / "ShaderSources";
        if (std::filesystem::is_regular_file(bundled / "Forward/forward_pbr.slang")) return bundled;

        const auto development = std::filesystem::path{GAMEENGINE_SOURCE_DIR} / "Engine/Shaders";
        if (std::filesystem::is_regular_file(development / "Forward/forward_pbr.slang")) return development;

        throw std::runtime_error("Shader Graph sources are not available. Reinstall the GamEngine Editor.");
    }

    [[nodiscard]] std::string buildConfigurationFromExecutableDirectory(
        const std::filesystem::path& executableDirectory) {
        std::string name = executableDirectory.filename().string();
        std::ranges::transform(name, name.begin(), [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if (name == "debug") return "Debug";
        if (name == "release") return "Release";
        if (name == "relwithdebinfo") return "RelWithDebInfo";
        if (name == "minsizerel") return "MinSizeRel";
        return {};
    }

    [[nodiscard]] bool runCmake(const std::initializer_list<std::string_view> arguments) {
        std::vector<std::string> values{"cmake"};
        values.reserve(values.size() + arguments.size());
        for (const auto argument : arguments) values.emplace_back(argument);
        std::vector<char *> argv;
        argv.reserve(values.size() + 1);
        for (auto &value : values) argv.push_back(value.data());
        argv.push_back(nullptr);

        const SDL_PropertiesID properties = SDL_CreateProperties();
        if (!properties) return false;
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
        SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
        SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
        SDL_Process *process = SDL_CreateProcessWithProperties(properties);
        SDL_DestroyProperties(properties);
        if (!process) return false;
        size_t outputSize = 0;
        int exitCode = -1;
        void *output = SDL_ReadProcess(process, &outputSize, &exitCode);
        SDL_free(output);
        SDL_DestroyProcess(process);
        return exitCode == 0;
    }

    [[nodiscard]] bool configureScriptBuild(const std::filesystem::path &editorRoot,
                                            const std::filesystem::path &projectRoot) {
        const auto source = editorRoot / "SDK" / "GameScripts";
        const auto sdk = editorRoot / "SDK";
        const auto build = projectRoot / "Library" / "ScriptBuild";
        if (!std::filesystem::is_regular_file(source / "CMakeLists.txt") ||
            !std::filesystem::is_regular_file(sdk / "Lib" / "Engine.lib")) return false;
        std::error_code error;
        std::filesystem::create_directories(build, error);
        if (error) return false;
        // The build tree belongs to the project. Its cache is intentionally
        // retained between Editor launches; file changes only build the target.
        if (std::filesystem::is_regular_file(build / "CMakeCache.txt")) {
            const auto globVerification = build / "CMakeFiles" / "VerifyGlobs.cmake";
            std::ifstream verification{globVerification, std::ios::binary};
            const std::string contents{std::istreambuf_iterator<char>{verification}, {}};
            // Earlier SDK revisions wrote native Windows paths into this CMake
            // script. CMake then interprets \U in a user path as an escape.
            if (contents.find('\\') == std::string::npos) return true;
            std::filesystem::remove_all(build, error);
            if (error) return false;
            std::filesystem::create_directories(build, error);
            if (error) return false;
        }
        return runCmake({"-S", source.string(), "-B", build.string(),
                         "-DGE_PROJECT_ROOT=" + projectRoot.string(),
                         "-DGE_SDK_ROOT=" + sdk.string(), "-DCMAKE_BUILD_TYPE=Release"});
    }

    std::filesystem::path findDefaultUiFont() {
        constexpr std::array<const char *, 5> candidates{
            "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/consola.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        };
        const auto font = std::ranges::find_if(candidates, [](const char *path) {
            return std::filesystem::is_regular_file(path);
        });
        if (font == candidates.end()) {
            throw std::runtime_error("No default TrueType font found for ImGui");
        }
        return *font;
    }
}
// NOLINTBEGIN(readability-magic-numbers)

int main(int argc, char** argv) {
    Editor::registerBuiltinComponents();
    try {
        Engine::Diagnostics::instance().initialize(Platform::UserPaths::editorLogs());
        const std::filesystem::path editorRoot = executableDirectory();
        const std::filesystem::path shaderGraphSourceDirectory = findShaderGraphSourceDirectory(editorRoot);
        std::optional<std::filesystem::path> projectPath;
        std::optional<std::filesystem::path> createProjectPath;
        for (int index = 1; index < argc; ++index) {
            if (std::string_view{argv[index]} == "--project") {
                if (++index == argc) throw std::runtime_error("--project requires a file path");
                projectPath = argv[index];
            } else if (std::string_view{argv[index]} == "--create-project") {
                if (++index == argc) throw std::runtime_error("--create-project requires a directory path");
                createProjectPath = argv[index];
            }
        }
        if (projectPath && createProjectPath) {
            throw std::runtime_error("Use either --project or --create-project, not both");
        }
        const Editor::EditorSession previousSession = Editor::loadSession();
        Engine::Project project = createProjectPath
                                            ? Engine::Project::create(*createProjectPath)
                                            : projectPath
                                            ? Engine::Project::load(*projectPath)
                                            : std::filesystem::is_regular_file(previousSession.projectManifest)
                                            ? Engine::Project::load(previousSession.projectManifest)
                                            : [&] {
                                                  try {
                                                      return Engine::Project::discover(
                                                          std::filesystem::current_path());
                                                  } catch (const std::runtime_error&) {
                                                      return Engine::Project::defaults(editorRoot);
                                                  }
                                              }();
        EditorSceneSession::setProjectRoot(project.rootPath());
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_Window *window = SDL_CreateWindow((project.name() + " Editor").c_str(),
                                              EditorConstants::windowWidth,
                                              EditorConstants::windowHeight,
                                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (window == nullptr) {
            throw std::runtime_error(SDL_GetError());
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImNodes::CreateContext();
        ImNodes::SetImGuiContext(ImGui::GetCurrentContext());
        ImNodes::StyleColorsDark();
        ImGuiIO &imguiIo = ImGui::GetIO();
        const std::filesystem::path statePath = Platform::UserPaths::editorState();
        std::filesystem::create_directories(statePath);
        const std::string imguiIniPath = (statePath / "imgui.ini").string();
        const bool restorePersistedLayout = std::filesystem::is_regular_file(imguiIniPath);
        imguiIo.IniFilename = imguiIniPath.c_str();
        imguiIo.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
        const std::filesystem::path uiFont = findDefaultUiFont();
        if (imguiIo.Fonts->AddFontFromFileTTF(uiFont.string().c_str(), 16.0F) == nullptr) {
            throw std::runtime_error("Could not load ImGui font: " + uiFont.string());
        }
        ImFont* terminalFont = imguiIo.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/consola.ttf", 16.0F);
        EditorStyle::apply();
        auto shaderGraphPanel = std::make_unique<Editor::ShaderGraphPanel>();
        Engine::ShaderGraphAsset shaderGraph{.name = "Untitled Shader Graph"};
        Engine::ShaderPinId shaderGraphPinId = 1;
        shaderGraph.nodes.push_back(Engine::ShaderNodeFactory::create(
            Engine::ShaderNodeType::SurfaceOutput, 1, shaderGraphPinId));
        shaderGraphPanel->open(shaderGraph);

        Engine::ScenePreset scene;
        Engine::ScriptModuleManager scriptModules{Engine::ScriptRegistry::instance()};
        std::optional<Editor::ScriptHotReload> scriptHotReload;
        std::optional<Editor::ShaderHotReload> shaderHotReload;
        const std::filesystem::path scriptBuildDirectory = project.rootPath() / "Library" / "ScriptBuild";
        const std::filesystem::path modulePath = project.rootPath() / "Library" / "ScriptModules" / "GameScripts.dll";
        if (!scriptModules.loadInitialModule(modulePath)) {
            Editor::ConsolePanel::info("No compiled game scripts module yet; it will be built for this project.");
        }
        // Shader hot reload still uses the source build. C++ script hot reload
        // above is deliberately independent of whether this is a package.
        const bool portableEditor = std::filesystem::is_regular_file(
            editorRoot / "Tools" / "Slang" /
#ifdef _WIN32
            "slangc.exe"
#else
            "slangc"
#endif
        );
        if (configureScriptBuild(editorRoot, project.rootPath())) {
            scriptHotReload.emplace(project.rootPath() / "Assets" / "Scripts", modulePath,
                                    scriptBuildDirectory, "Release");
            scriptHotReload->requestBuild();
            Editor::ConsolePanel::info("C++ script hot reload enabled.");
        } else {
            Editor::ConsolePanel::warning("C++ script toolchain is unavailable. Install CMake and MSVC Build Tools, then reopen the project.");
        }
        if (!portableEditor) {
            if (const auto buildDirectory = findDevelopmentBuildDirectory(editorRoot)) {
                if (const auto shaderDirectory = findEngineShaderDirectory(*buildDirectory)) {
                    shaderHotReload.emplace(*shaderDirectory, *buildDirectory,
                                             *buildDirectory / "resources" / "shaders", editorRoot / "shaders",
                                             buildConfigurationFromExecutableDirectory(editorRoot));
                } else {
                    Editor::ConsolePanel::warning("Shader hot reload is unavailable: could not locate Engine/Shaders.");
                }
            }
        }
        Engine::Assets::Content content{project.assetRoot()};
        content.setErrorHandler([](const std::string& message) {
            Editor::ConsolePanel::error("Asset: " + message);
        });
        const auto resolveShaderGraphMaterials = [&](const std::optional<std::filesystem::path>& updatedGraph = std::nullopt) {
            const auto forwardTemplate = shaderGraphSourceDirectory / "Forward/forward_pbr.slang";
            const auto generatedDirectory = content.assetRoot().parent_path() / "Library/ShaderGraphs";
            std::vector<Engine::Entity> graphMaterials;
            scene.editor().view<Engine::MeshRenderer>([&](const Engine::Entity entity, const Engine::MeshRenderer& meshRenderer) {
                if (meshRenderer.material.shaderSource == Engine::MaterialShaderSource::ShaderGraph &&
                    !meshRenderer.material.shaderGraphAsset.empty() &&
                    (!updatedGraph || meshRenderer.material.shaderGraphAsset.lexically_normal() == *updatedGraph))
                    graphMaterials.push_back(entity);
            });
            for (const Engine::Entity entity : graphMaterials) {
                auto material = scene.editor().read<Engine::MeshRenderer>(entity).material;
                const auto result = Engine::ShaderGraphMaterialCompiler{}.resolve(
                    material, content.assetRoot(), forwardTemplate, generatedDirectory);
                if (result.succeeded()) {
                    scene.editor().patch<Engine::MeshRenderer>(entity, [&](Engine::MeshRenderer& meshRenderer) {
                        meshRenderer.material = std::move(material);
                    });
                } else {
                    material.shaderProgram = {};
                    material.shaderProgramSpirv.clear();
                    scene.editor().patch<Engine::MeshRenderer>(entity, [&](Engine::MeshRenderer& meshRenderer) {
                        meshRenderer.material = std::move(material);
                    });
                    std::string error{"Could not resolve Shader Graph material"};
                    for (const auto& diagnostic : result.diagnostics) error += ": " + diagnostic.message;
                    Editor::ConsolePanel::error(error);
                }
            }
        };
        shaderGraphPanel->setSavedCallback([&](const std::filesystem::path& absolutePath) {
            const auto relativePath = absolutePath.lexically_relative(content.assetRoot()).lexically_normal();
            resolveShaderGraphMaterials(relativePath);
        });
        Editor::ConsolePanel::info("Editor started for project '" + project.name() + "'.");
        const bool restoreScene = !projectPath && !createProjectPath &&
                                  std::filesystem::is_regular_file(previousSession.scenePath);
        const auto initialScene = restoreScene
                                      ? previousSession.scenePath
                                      : project.startupScene();
        const bool loadInitialSceneAsync = std::filesystem::is_regular_file(initialScene);
        EditorSceneSession::setScenePath(initialScene);
        Engine::ScriptSystem scriptSystem{Engine::ScriptRegistry::instance()};
        Engine::PhysicsSystem physicsSystem{};
        // The editor is the visual authoring path, so shadows must be active
        // by default. The engine-level default stays conservative for clients
        // which explicitly optimize for an unshadowed renderer.
        Engine::Renderer renderer{Engine::RenderConfig{
            .features = Engine::RenderFeatures{.shadows = true}}};
        renderer.initializeCore(scene, window);
        SceneHistory history;
        history.reset(scene);
        std::optional<std::future<std::unique_ptr<Engine::ScenePreset>>> initialSceneLoad;
        bool startInitialSceneLoad = loadInitialSceneAsync;
        bool startEmptySceneResources = !loadInitialSceneAsync;
        bool initialSceneSyncPending = false;
        bool initialSceneFirstRenderableFramePending = false;
        std::optional<std::chrono::steady_clock::time_point> initialSceneLoadStartedAt;
        const auto reportSceneLoadStage = [](const std::string_view stage,
                                             const std::chrono::steady_clock::time_point startedAt) {
            const auto elapsed = std::chrono::steady_clock::now() - startedAt;
            const auto milliseconds = std::chrono::duration<double, std::milli>{elapsed}.count();
            Editor::ConsolePanel::info("[SceneLoad] " + std::string{stage} + ": " +
                                       std::to_string(milliseconds) + " ms");
        };
        EntityClipboard clipboard;
        Engine::Entity selectedEntity = Engine::NullEntity;
        std::vector<Engine::Entity> selectedEntities;
        const auto setSelection = [&](const Engine::Entity entity) {
            selectedEntities.clear();
            if (entity != Engine::NullEntity) selectedEntities.push_back(entity);
            selectedEntity = entity;
            renderer.setEditorSelection(selectedEntity);
        };
        const auto toggleSelection = [&](const Engine::Entity entity) {
            if (const auto found = std::ranges::find(selectedEntities, entity);
                found != selectedEntities.end()) {
                selectedEntities.erase(found);
                if (selectedEntity == entity)
                    selectedEntity = selectedEntities.empty() ? Engine::NullEntity : selectedEntities.back();
            } else {
                selectedEntities.push_back(entity);
                selectedEntity = entity;
            }
            renderer.setEditorSelection(selectedEntity);
        };
        constexpr auto targetFrame = EditorConstants::targetFrameMicroseconds;
        bool running = true;
        bool rendererReloadPending = false;
        bool playing = false;
        bool paused = false;
        bool showHierarchy = true;
        bool showViewport = true;
        bool showInspector = true;
        bool showAssetManager = true;
        bool showTerrainTools = true;
        bool showConsole = true;
        bool showShaderGraph = false;
        bool showTerminal = true;
        bool showProfiler = true;
        Editor::TerminalPanel terminal{project.rootPath(), terminalFont};
        double physicsAccumulator = 0.0;
        bool showGameView = false;
        GizmoMode gizmoMode = GizmoMode::Translate;
        SelectionTool selectionTool = SelectionTool::Rectangle;
        TerrainSculptState terrainSculpt;
        std::string playSceneSnapshot;
        std::string playModeError;
        // Restoring the editor snapshot replaces registry-owned render data.
        // Its GPU resources are rebuilt at the start of the following loop,
        // so never render the one intervening frame against stale buffers.
        bool skipRendererFrameAfterSceneRestore = false;
        constexpr auto autoSaveInterval = std::chrono::seconds{30};
        auto lastAutoSaveAttempt = std::chrono::steady_clock::now();
        std::uint64_t lastPersistedSceneRevision = scene.editor().mutationRevision();
        while (running) {
            const auto start = std::chrono::steady_clock::now();
            Engine::Profiler::beginFrame();
            Engine::Renderer::beginFrame();
            if (initialSceneLoad &&
                initialSceneLoad->wait_for(std::chrono::seconds::zero()) == std::future_status::ready) {
                try {
                    auto loadedScene = initialSceneLoad->get();
                    if (initialSceneLoadStartedAt) {
                        reportSceneLoadStage("CPU scene load finished", *initialSceneLoadStartedAt);
                    }
                    {
                        GE_PROFILE_SCOPE("SceneLoad.Adopt");
                        const auto stageStartedAt = std::chrono::steady_clock::now();
                        Engine::SceneSerializer::replace(scene, *loadedScene);
                        reportSceneLoadStage("Scene adopt", stageStartedAt);
                    }
                    {
                        GE_PROFILE_SCOPE("SceneLoad.ShaderGraphResolve");
                        const auto stageStartedAt = std::chrono::steady_clock::now();
                        resolveShaderGraphMaterials();
                        reportSceneLoadStage("Shader graphs ready", stageStartedAt);
                    }
                    {
                        GE_PROFILE_SCOPE("SceneLoad.HistoryReset");
                        const auto stageStartedAt = std::chrono::steady_clock::now();
                        history.reset(scene);
                        reportSceneLoadStage("History reset", stageStartedAt);
                    }
                    lastPersistedSceneRevision = scene.editor().mutationRevision();
                    setSelection(Engine::NullEntity);
                    initialSceneSyncPending = true;
                    Editor::ConsolePanel::info("[SceneLoad] ECS ready: " + initialScene.string());
                } catch (const std::exception& error) {
                    Editor::ConsolePanel::error("Could not load startup scene: " +
                                                std::string{error.what()});
                }
                initialSceneLoad.reset();
            }
            const Engine::EditorEventState events = renderer.pollEditorEvents();
            if (events.quitRequested) {
                running = false;
            }
            // Apply Scene View navigation before drawing its gizmo. Rendering
            // later in this frame then uses this exact same camera transform.
            renderer.updateEditorSceneCameraInput();
            if (events.togglePlay) {
                if (EditorSceneSession::setPlayMode(!playing, scene, playSceneSnapshot, playModeError,
                                                    EditorSceneSession::msaaSampleCount(renderer))) {
                    physicsSystem.reset();
                    playing = !playing;
                    if (!playing) resolveShaderGraphMaterials();
                    paused = false;
                    physicsAccumulator = 0.0;
                    showGameView = playing;
                    rendererReloadPending = !playing;
                    skipRendererFrameAfterSceneRestore = !playing;
                    Editor::ConsolePanel::info(playing ? "Entered Play mode." : "Stopped Play mode.");
                } else Editor::ConsolePanel::error("Could not change Play mode: " + playModeError);
            }
            if (events.togglePause && playing) {
                paused = !paused;
                physicsAccumulator = 0.0;
            }
            if (!running) {
                break;
            }

            // Antialiasing changes recreate render-target resources. Ordinary
            // scene edits are synchronized below without rebuilding the UI or
            // swapchain.
            if (rendererReloadPending) {
                renderer.reloadScene(scene, window);
                rendererReloadPending = false;
            }

            const std::uint64_t sceneStructureBeforeUi = scene.editor().structuralRevision();
            renderer.beginEditorUiFrame();
            bool antialiasingChanged = false;
            bool sceneLoaded = false;
            bool sceneSaved = false;
            bool sceneDeleted = false;
            bool playToggleRequested = false;
            bool pauseToggleRequested = false;
            bool undoRequested = false;
            bool redoRequested = false;
            bool copyRequested = false;
            bool pasteRequested = false;
            bool duplicateRequested = false;
            bool resetHistoryRequested = false;
            if (const Engine::Entity created = drawEditorMenuBar(scene, renderer, content, project,
                                                                 antialiasingChanged, sceneLoaded, sceneSaved,
                                                                 sceneDeleted,
                                                                 playing, paused, playToggleRequested,
                                                                 pauseToggleRequested, history.canUndo(),
                                                                 history.canRedo(), clipboard.canPaste(scene),
                                                                 undoRequested, redoRequested, copyRequested,
                                                                 pasteRequested, duplicateRequested,
                                                                 resetHistoryRequested, showHierarchy,
                                                                 showViewport, showInspector, showAssetManager,
                                                                 showTerrainTools, showConsole, showTerminal,
                                                                 showShaderGraph, showProfiler);
                created != Engine::NullEntity) {
                setSelection(created);
            }
            if (resetHistoryRequested) {
                history.reset(scene);
            }
            if (sceneDeleted) {
                Engine::ScenePreset emptyScene;
                Engine::SceneSerializer::replace(scene, emptyScene);
                scene.plane = Engine::NullEntity;
                scene.camera = Engine::NullEntity;
                scene.particleSystem = Engine::NullEntity;
                scene.editorGameObjects.clear();
                scene.editorCubes.clear();
                scene.editorPlanes.clear();
                scene.editorSpheres.clear();
                scene.editorCapsules.clear();
                scene.editorRamps.clear();
                scene.editorLights.clear();
                scene.editorTerrains.clear();
                scene.editorClouds.clear();
                history.reset(scene);
                lastPersistedSceneRevision = scene.editor().mutationRevision();
                setSelection(Engine::NullEntity);
                rendererReloadPending = true;
            }
            if (!playing && undoRequested && history.undo(scene)) {
                sceneLoaded = true;
            }
            if (!playing && redoRequested && history.redo(scene)) {
                sceneLoaded = true;
            }
            if (sceneLoaded) {
                resolveShaderGraphMaterials();
                // Loading replaces the registry, so any selection from the
                // previous scene is stale before the hierarchy/inspector are
                // drawn for this frame.
                setSelection(Engine::NullEntity);
                rendererReloadPending = true;
            }
            if (!playing && selectedEntity != Engine::NullEntity &&
                scene.editor().valid(selectedEntity)) {
                if (copyRequested) {
                    clipboard.copy(scene, selectedEntity);
                }
                if (pasteRequested) {
                    setSelection(clipboard.paste(scene));
                }
                if (duplicateRequested) {
                    setSelection(scene.editor().duplicate(selectedEntity));
                }
            }
            const auto setPlayMode = [&](const bool enabled) {
                if (!EditorSceneSession::setPlayMode(enabled, scene, playSceneSnapshot, playModeError,
                                                     EditorSceneSession::msaaSampleCount(renderer))) {
                    Editor::ConsolePanel::error("Could not change Play mode: " + playModeError);
                    return false;
                }
                physicsSystem.reset();
                playing = enabled;
                paused = false;
                physicsAccumulator = 0.0;
                showGameView = playing;
                if (!playing) {
                    resolveShaderGraphMaterials();
                    setSelection(Engine::NullEntity);
                    rendererReloadPending = true;
                    skipRendererFrameAfterSceneRestore = true;
                }
                Editor::ConsolePanel::info(playing ? "Entered Play mode." : "Stopped Play mode.");
                return true;
            };
            if (playToggleRequested) static_cast<void>(setPlayMode(!playing));
            if (pauseToggleRequested && playing) {
                paused = !paused;
                physicsAccumulator = 0.0;
            }
            const ImGuiViewport *viewport = ImGui::GetMainViewport();
            const ImVec2 dockSize{viewport->WorkSize.x,
                                  std::max(0.0F, viewport->WorkSize.y -
                                                    static_cast<float>(EditorConstants::statusBarHeight))};
            // Keep the status bar outside the dockspace. DockSpaceOverViewport
            // uses the complete work area, which allowed docked panels to
            // continue underneath the status bar.
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(dockSize);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
            ImGui::Begin("##editor-dockspace", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground);
            const ImGuiID dockspaceId = viewport->ID;
            ImGui::DockSpace(dockspaceId, {0.0F, 0.0F}, ImGuiDockNodeFlags_PassthruCentralNode);
            EditorStyle::configureDockLayout(dockSize, restorePersistedLayout);
            ImGui::End();
            ImGui::PopStyleVar();
            HierarchyPanel::Action hierarchyAction = HierarchyPanel::Action::None;
            Engine::Entity hierarchyActionEntity = Engine::NullEntity;
            if (showHierarchy) {
                if (const Engine::Entity clicked = HierarchyPanel::draw(
                        scene, content, selectedEntities, hierarchyAction, hierarchyActionEntity,
                        clipboard.canPaste(scene), playing, showHierarchy);
                    clicked != Engine::NullEntity) {
                    // Ctrl toggles individual rows; Shift extends the set.
                    // The last clicked object remains the active inspector/gizmo target.
                    if (ImGui::GetIO().KeyCtrl) toggleSelection(clicked);
                    else if (ImGui::GetIO().KeyShift) {
                        if (std::ranges::find(selectedEntities, clicked) == selectedEntities.end())
                            selectedEntities.push_back(clicked);
                        selectedEntity = clicked;
                        renderer.setEditorSelection(selectedEntity);
                    } else setSelection(clicked);
                }
            }
            if (!playing && hierarchyAction == HierarchyPanel::Action::Paste) {
                setSelection(clipboard.paste(scene));
            } else if (!playing && hierarchyActionEntity != Engine::NullEntity &&
                       scene.editor().valid(hierarchyActionEntity)) {
                if (hierarchyAction == HierarchyPanel::Action::Delete) {
                    scene.editor().destroy(hierarchyActionEntity);
                    if (selectedEntity == hierarchyActionEntity) {
                        setSelection(Engine::NullEntity);
                    }
                } else if (hierarchyAction == HierarchyPanel::Action::Duplicate) {
                    setSelection(scene.editor().duplicate(hierarchyActionEntity));
                } else if (hierarchyAction == HierarchyPanel::Action::Copy) {
                    clipboard.copy(scene, hierarchyActionEntity);
                }
            }
            if (!playing && !ImGui::GetIO().WantTextInput &&
                !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                bool gizmoModeChanged = false;
                if (ImGui::IsKeyPressed(ImGuiKey_W)) {
                    gizmoMode = GizmoMode::Translate;
                    gizmoModeChanged = true;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_E)) {
                    gizmoMode = GizmoMode::Rotate;
                    gizmoModeChanged = true;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_R)) {
                    gizmoMode = GizmoMode::Scale;
                    gizmoModeChanged = true;
                }
                if (gizmoModeChanged && selectedEntity != Engine::NullEntity &&
                    scene.editor().valid(selectedEntity)) {
                    renderer.setEditorSelection(selectedEntity);
                }
            }
            ViewportInteraction viewportInteraction{};
            if (showViewport) {
                viewportInteraction = drawViewport(
                    scene, content, selectedEntity, selectedEntities, renderer,
                    renderer.gameViewport(), renderer.sceneViewport(),
                    renderer.editorCameraYaw(), renderer.editorCameraPitch(), showGameView, gizmoMode,
                    selectionTool, terrainSculpt, playing, showViewport);
            }
            if (viewportInteraction.playModeAction == PlayModeAction::Start && !playing) {
                static_cast<void>(setPlayMode(true));
            } else if (viewportInteraction.playModeAction == PlayModeAction::Stop && playing) {
                static_cast<void>(setPlayMode(false));
            } else if (viewportInteraction.playModeAction == PlayModeAction::Restart && playing &&
                       setPlayMode(false)) {
                static_cast<void>(setPlayMode(true));
            }
            if (showTerrainTools) {
                drawTerrainToolsPanel(scene, content, selectedEntity, renderer, terrainSculpt,
                                      playing, showTerrainTools);
            }
            if (viewportInteraction.createdEntity != Engine::NullEntity) {
                setSelection(viewportInteraction.createdEntity);
            }
            if (!playing && viewportInteraction.sceneClicked) {
                constexpr float viewportAspect = EditorConstants::viewportWidthRatio /
                                                 EditorConstants::viewportHeightRatio;
                const Engine::Entity hit = pickSceneEntity(scene, physicsSystem, renderer,
                                                           viewportInteraction.normalizedX,
                                                           viewportInteraction.normalizedY, viewportAspect);
                if (ImGui::GetIO().KeyCtrl && hit != Engine::NullEntity) toggleSelection(hit);
                else if (ImGui::GetIO().KeyShift && hit != Engine::NullEntity) {
                    if (std::ranges::find(selectedEntities, hit) == selectedEntities.end()) selectedEntities.push_back(hit);
                    selectedEntity = hit;
                    renderer.setEditorSelection(selectedEntity);
                } else setSelection(hit);
            }
            if (!playing && viewportInteraction.selectionCommitted) {
                if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift) {
                    for (const Engine::Entity entity : viewportInteraction.selectedEntities) {
                        if (std::ranges::find(selectedEntities, entity) == selectedEntities.end())
                            selectedEntities.push_back(entity);
                    }
                    if (!selectedEntities.empty()) selectedEntity = selectedEntities.back();
                    renderer.setEditorSelection(selectedEntity);
                } else {
                    selectedEntities = std::move(viewportInteraction.selectedEntities);
                    selectedEntity = selectedEntities.empty() ? Engine::NullEntity : selectedEntities.back();
                    renderer.setEditorSelection(selectedEntity);
                }
            }
            const bool inspectorConsumesMouseWheel = showInspector &&
                ComponentsPanel::draw(scene, content, shaderGraphSourceDirectory, selectedEntities, selectedEntity,
                                      showInspector);
            if (showAssetManager) {
                if (const Engine::Entity created =
                        AssetManagerPanel::draw(scene, content, playing, showAssetManager,
                                                !project.manifestPath().empty(),
                                                [&](const std::filesystem::path& relativePath) {
                                                    try {
                                                        shaderGraph = Engine::ShaderGraphSerializer::load(
                                                            content.assetRoot() / relativePath);
                                                        shaderGraphPanel->open(shaderGraph,
                                                                               content.assetRoot() / relativePath);
                                                        showShaderGraph = true;
                                                    } catch (const std::exception& error) {
                                                        Editor::ConsolePanel::error("Could not open shader graph: " +
                                                                                    std::string{error.what()});
                                                    }
                                                },
                                                [&](const std::filesystem::path& deletedPath) {
                                                    if (deletedPath != EditorSceneSession::scenePath()) return;

                                                    EditorSceneSession::clearSavedScene();
                                                    Engine::ScenePreset emptyScene;
                                                    Engine::SceneSerializer::replace(scene, emptyScene);
                                                    scene.plane = Engine::NullEntity;
                                                    scene.camera = Engine::NullEntity;
                                                    scene.particleSystem = Engine::NullEntity;
                                                    scene.editorGameObjects.clear();
                                                    scene.editorCubes.clear();
                                                    scene.editorPlanes.clear();
                                                    scene.editorSpheres.clear();
                                                    scene.editorCapsules.clear();
                                                    scene.editorRamps.clear();
                                                    scene.editorLights.clear();
                                                    scene.editorTerrains.clear();
                                                    scene.editorClouds.clear();
                                                    history.reset(scene);
                                                    lastPersistedSceneRevision = scene.editor().mutationRevision();
                                                    setSelection(Engine::NullEntity);
                                                    rendererReloadPending = true;
                                                });
                    created != Engine::NullEntity) {
                    setSelection(created);
                }
            }
            if (showConsole) Editor::ConsolePanel::draw(showConsole);
            if (showTerminal) terminal.draw(showTerminal);
            if (showShaderGraph) shaderGraphPanel->draw(showShaderGraph);
            Editor::drawProfilerPanel(renderer, showProfiler);
            drawStatusBar(scene, selectedEntity, playing, paused);
            if (!playing && selectedEntity != Engine::NullEntity &&
                scene.editor().valid(selectedEntity) && !ImGui::GetIO().WantTextInput &&
                ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                scene.editor().destroy(selectedEntity);
                setSelection(Engine::NullEntity);
            }
            if (!playing && !ImGui::GetIO().WantTextInput &&
                ImGui::GetIO().KeyCtrl) {
                if (ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_N)) {
                    selectedEntity = scene.createGameObject();
                    renderer.setEditorSelection(selectedEntity);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Z) && history.undo(scene)) {
                    sceneLoaded = true;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Y) && history.redo(scene)) {
                    sceneLoaded = true;
                }
                if (selectedEntity != Engine::NullEntity && scene.editor().valid(selectedEntity)) {
                    if (ImGui::IsKeyPressed(ImGuiKey_C)) {
                        clipboard.copy(scene, selectedEntity);
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_D)) {
                        setSelection(scene.editor().duplicate(selectedEntity));
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_V)) {
                        setSelection(clipboard.paste(scene));
                    }
                }
                if (sceneLoaded) {
                    setSelection(Engine::NullEntity);
                    rendererReloadPending = true;
                }
            }
            renderer.setEditorSceneCameraInput(
                viewportInteraction.cameraInput && !inspectorConsumesMouseWheel);
            // A Stop click changes `playing` after drawViewport() has produced
            // its interaction state.  Do not pass that stale state to the
            // camera controller: it would interpret the same click as a
            // request to capture the game mouse again.
            renderer.setGameCameraInput(playing && viewportInteraction.gameCameraInput);
            if (playing && viewportInteraction.gameMouseCaptureRequested) {
                renderer.requestGameMouseCapture();
            }
            renderer.setSceneViewportActive(showViewport && !showGameView && !playing);

            if (antialiasingChanged) {
                rendererReloadPending = true;
            }
            const bool sceneStructureChanged = !sceneLoaded &&
                                               scene.editor().structuralRevision() != sceneStructureBeforeUi;
            // ImGui::Image has already captured this frame's viewport descriptor.
            // Rebuilding scene resources can release that descriptor, so defer the
            // rebuild until its draw commands have been submitted below.
            const bool sceneResourceSyncPending = sceneStructureChanged ||
                                                  viewportInteraction.terrainGrassChanged;
            if (!sceneResourceSyncPending && viewportInteraction.terrainGeometryChanged) {
                for (const Engine::Entity terrain : viewportInteraction.terrainGeometryEntities)
                    renderer.updateMeshGeometry(terrain);
            }
            if (terrainSculpt.strokeCompleted) {
                static_cast<void>(history.capture(scene));
                terrainSculpt.strokeCompleted = false;
                terrainSculpt.completedEntity = Engine::NullEntity;
                terrainSculpt.completedDirty = {};
                terrainSculpt.heightsBeforeStroke.clear();
            }
            // A history snapshot serializes the complete scene, including
            // decoded GLB image pixels. Capture only after an actual mutation
            // and once an interactive edit has finished; SceneHistory performs
            // the revision check before touching the serializer.
            const bool editingScene = ImGui::IsAnyItemActive() ||
                                      ImGui::IsMouseDown(ImGuiMouseButton_Left);
            if (!playing && !sceneLoaded && !editingScene) {
                static_cast<void>(history.capture(scene));
            }
            if (sceneSaved) {
                lastPersistedSceneRevision = scene.editor().mutationRevision();
                lastAutoSaveAttempt = std::chrono::steady_clock::now();
            }
            const auto now = std::chrono::steady_clock::now();
            const std::uint64_t currentSceneRevision = scene.editor().mutationRevision();
            if (!playing && EditorSceneSession::hasSavedScene() &&
                currentSceneRevision != lastPersistedSceneRevision &&
                now - lastAutoSaveAttempt >= autoSaveInterval) {
                lastAutoSaveAttempt = now;
                try {
                    const std::filesystem::path path = EditorSceneSession::scenePath();
                    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
                    Engine::SceneSerializer::save(scene, path,
                                                  EditorSceneSession::msaaSampleCount(renderer));
                    EditorSceneSession::markSceneSaved(path);
                    lastPersistedSceneRevision = currentSceneRevision;
                    Editor::ConsolePanel::info("Auto-saved scene: " + path.string());
                } catch (const std::exception& error) {
                    Editor::ConsolePanel::warning("Could not auto-save scene: " +
                                                  std::string{error.what()});
                }
            }
            if (playing && !paused) {
                GE_PROFILE_SCOPE("Game Update");
                physicsAccumulator += Engine::Time::deltaTime();
                int physicsSteps = 0;
                while (physicsAccumulator >= EditorConstants::physicsStep &&
                       physicsSteps < EditorConstants::maximumPhysicsStepsPerFrame) {
                    try {
                        GE_PROFILE_SCOPE("Physics");
                        physicsSystem.update(scene, static_cast<float>(EditorConstants::physicsStep));
                    } catch (const std::exception& error) {
                        Editor::ConsolePanel::error("Physics simulation stopped: " +
                                                    std::string{error.what()});
                        static_cast<void>(setPlayMode(false));
                        break;
                    }
                    physicsAccumulator -= EditorConstants::physicsStep;
                    ++physicsSteps;
                }
                if (playing) {
                    GE_PROFILE_SCOPE("Scripts");
                    scriptSystem.update(scene, static_cast<float>(Engine::Time::deltaTime()));
                }
            }
            if (scriptHotReload) {
                scriptHotReload->setProject(project.rootPath() / "Assets" / "Scripts");
                scriptHotReload->poll(
                    scriptModules, scene,
                    [](const std::string &message) { Editor::ConsolePanel::info(message); },
                    [](const std::string &message) { Editor::ConsolePanel::warning(message); });
            }
            if (shaderHotReload) {
                shaderHotReload->poll(
                    [&renderer] { return renderer.reloadShaders(); },
                    [](const std::string& message) { Editor::ConsolePanel::info(message); },
                    [](const std::string& message) { Editor::ConsolePanel::warning(message); });
            }
            ImGui::Render();
            const bool deferSceneGpuWork = skipRendererFrameAfterSceneRestore;
            {
                GE_PROFILE_SCOPE("Renderer");
                if (!deferSceneGpuWork) {
                    renderer.renderFrame();
                }
            }
            if (!deferSceneGpuWork && initialSceneFirstRenderableFramePending) {
                if (initialSceneLoadStartedAt) {
                    reportSceneLoadStage("First renderable frame", *initialSceneLoadStartedAt);
                }
                initialSceneFirstRenderableFramePending = false;
                initialSceneLoadStartedAt.reset();
            }
            if (!deferSceneGpuWork) {
                if (const auto gpu = renderer.gpuProfile()) {
                    Engine::Profiler::attachGpuFrame(gpu->frameNumber, gpu->frameMilliseconds, gpu->events);
                }
            }

            if (!deferSceneGpuWork && (sceneResourceSyncPending || initialSceneSyncPending)) {
                const bool completingInitialSceneLoad = initialSceneSyncPending;
                {
                    GE_PROFILE_SCOPE("SceneLoad.RendererSynchronize");
                    const auto stageStartedAt = std::chrono::steady_clock::now();
                    renderer.synchronizeScene(scene);
                    if (completingInitialSceneLoad) {
                        reportSceneLoadStage("GPU sync finished", stageStartedAt);
                    }
                }
                // Duplicated objects do not exist in the renderer's cached
                // renderable list until synchronization completes. Reapply
                // the selection so the next frame can outline it immediately.
                if (sceneStructureChanged) renderer.setEditorSelection(selectedEntity);
                initialSceneSyncPending = false;
                initialSceneFirstRenderableFramePending = completingInitialSceneLoad;
            }
            // renderer.reloadScene() runs at the next loop boundary, before
            // any further drawFrame() or incremental upload sees the restored
            // ECS snapshot.
            skipRendererFrameAfterSceneRestore = false;

            // Do not let file parsing, GLTF decoding or image decompression
            // postpone the first editor frame. The worker builds an isolated
            // Scene; only its completed registry is adopted on this thread.
            if (startInitialSceneLoad) {
                startInitialSceneLoad = false;
                initialSceneLoadStartedAt = std::chrono::steady_clock::now();
                Editor::ConsolePanel::info("Loading startup scene in background: " + initialScene.string());
                initialSceneLoad.emplace(std::async(std::launch::async, [initialScene] {
                    auto loadedScene = std::make_unique<Engine::ScenePreset>();
                    Engine::SceneSerializer::load(*loadedScene, initialScene);
                    return loadedScene;
                }));
            }
            if (startEmptySceneResources) {
                startEmptySceneResources = false;
                renderer.initializeScene(scene);
            }

            // Deliberately exclude the editor's frame limiter from CPU time:
            // it is idle time, not work the profiler should attribute.
            Engine::Profiler::endFrame();

            // Keep the editor UI responsive without unnecessarily throttling
            // the game simulation while Play Mode is active.
            if (const auto elapsed = std::chrono::steady_clock::now() - start;
                !playing && elapsed < targetFrame) {
                std::this_thread::sleep_for(targetFrame - elapsed);
            }
        }
        Editor::saveSession({.projectManifest = project.manifestPath(),
                             .scenePath = EditorSceneSession::scenePath()});
        terminal.shutdown();
        scriptModules.unload(scene);
        renderer.shutdown();
        shaderGraphPanel.reset();
        ImNodes::DestroyContext();
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        Engine::Diagnostics::instance().shutdown();
        return 0;
    } catch (const std::exception &error) {
        Engine::Diagnostics::instance().report(Engine::DiagnosticSeverity::Error, error.what(), {.subsystem = "Editor"});
        Engine::Diagnostics::instance().shutdown();
        std::fprintf(stderr, "Editor error: %s\n", error.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "GamEngine Editor error",
                                 error.what(), nullptr);
        return 1;
    }
}

// NOLINTEND(readability-magic-numbers)
