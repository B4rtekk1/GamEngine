# GamEngine

GamEngine is a high-performance 3D game engine written in **C++23** and built on **Vulkan 1.4**.
The project focuses on modern GPU-driven rendering, scalable real-time graphics, native C++ gameplay scripting, and an integrated editor/runtime workflow.

The engine is under active development. APIs, rendering systems, project formats, and editor workflows may change as the project evolves.

## Highlights

* Vulkan 1.4 renderer written in modern C++23
* GPU-driven scene rendering and visibility processing
* Meshlet-based rendering infrastructure
* Hi-Z and GPU culling
* Forward physically based rendering
* Directional Virtual Shadow Maps
* Temporal anti-aliasing and MSAA
* GTAO
* Image-based lighting and reflection probes
* Bloom and ACES tonemapping
* GPU particle simulation
* GPU-driven grass rendering
* Experimental Virtual Water system
* PhysX integration
* Native C++ gameplay scripting with hot reload support
* Shader Graph editor and Slang shader pipeline
* Scene editor with hierarchy, components, assets, profiler, console, and terminal
* Standalone Windows game export pipeline
* Automated unit tests for core engine systems

## Architecture

GamEngine is split into several major modules:

```text
GamEngine/
├── Engine/        Core runtime, renderer, ECS, physics, scripting and assets
├── Editor/        GamEngine editor and authoring tools
├── Player/        Standalone game runtime
├── Platform/      Platform-specific services
├── Tests/         Automated tests
├── Tools/         Build and asset-generation utilities
├── Assets/        Development project assets
├── MyGame/        Default example project
├── DEMO/          Demo project and assets
├── cmake/         Build, dependency and packaging scripts
└── ThridParty/    Embedded third-party source dependencies
```

### Engine

The `Engine` shared library contains the runtime systems used by both the editor and standalone games.

Major subsystems include:

* application and game loop
* entity-component system
* scenes, prefabs and transform hierarchy
* input
* physics
* native scripting
* asset management and cooking
* runtime UI
* Vulkan rendering
* Render Graph
* GPU scene database
* profiling and diagnostics

### Editor

The editor is built with Dear ImGui and ImNodes and provides an integrated environment for scene authoring and debugging.

Current editor functionality includes:

* Scene Hierarchy
* Components / Inspector
* Asset Manager
* Shader Graph
* Profiler
* Console
* integrated terminal
* terrain tools
* scene play/pause workflow
* script hot reload
* shader hot reload
* project management
* standalone game builds

### Player

`GamEnginePlayer` is a game-agnostic standalone runtime.

A packaged game contains the runtime, cooked project content, project manifest, and a separately compiled native gameplay module:

```text
Game/
├── Game.exe
├── Engine.dll
├── SDL3.dll
├── GameScripts.dll
├── GamEngine.project
├── shaders/
└── Content/
```

## Rendering

GamEngine uses a modern Vulkan rendering architecture designed around GPU-side visibility processing and scalable scene submission.

### GPU-driven rendering

The renderer includes infrastructure for:

* GPU scene storage
* GPU frustum culling
* GPU instance culling
* Hi-Z occlusion infrastructure
* indexed indirect drawing
* meshlets
* hierarchical meshlet data
* meshlet visibility processing
* GPU-driven shadow visibility
* GPU-driven grass visibility

The goal is to keep CPU submission cost low while allowing increasingly large scenes to be processed primarily on the GPU.

### Physically Based Rendering

The forward renderer supports physically based materials and scene lighting.

Implemented lighting-related systems include:

* direct lighting
* PBR material evaluation
* environment lighting
* image-based lighting
* BRDF lookup table generation
* reflection probes
* environment baking
* HDR rendering

### Virtual Shadow Maps

Directional shadows use a virtualized shadow-map architecture with physical page storage and virtual page allocation.

The shadow system contains infrastructure for:

* virtual page marking
* page allocation
* page compaction
* page clearing and commit
* multiple virtual shadow levels
* GPU shadow culling
* meshlet-based shadow rendering
* configurable shadow filtering quality
* shadow residency/debug views

Available quality levels are:

* Low
* Medium
* High
* Ultra

### Ambient Occlusion

GamEngine includes a GTAO implementation with configurable quality presets.

The pipeline contains dedicated stages for:

* depth preparation
* GTAO evaluation
* spatial filtering
* denoising
* upsampling
* optional temporal infrastructure
* diagnostic debug views

### Anti-aliasing

Supported anti-aliasing modes include:

* Off
* MSAA 2x
* MSAA 4x
* TAA

The temporal pipeline stores HDR and depth history and supports motion-vector-based reprojection.

### Post-processing

Current post-processing includes:

* GTAO
* TAA
* Bloom
* ACES tonemapping

### Image-Based Lighting

IBL quality can be configured independently from other renderer settings.

The renderer contains support for:

* environment cubemaps
* prefiltered environment lighting
* BRDF LUTs
* reflection probes
* reflection probe capture
* configurable reflection resolutions

### Particles

The particle system uses GPU simulation and billboard rendering.

### Grass

The terrain grass renderer contains dedicated GPU generation, classification, culling, indirect rendering, shadow and velocity passes.

It also includes support for wind and deformation data.

### Virtual Water

GamEngine contains an experimental GPU-owned Virtual Water renderer intended for large water surfaces.

The current architecture includes:

* virtualized water pages
* clipmap-style ocean topology
* GPU page culling
* indirect water rendering
* sparse persistent state
* local interaction events
* adaptive shading
* underwater state
* temporal history
* screen-space reflection depth hierarchy
* authored water bodies and far-ocean rendering

This system is still evolving and should be considered experimental.

## Shader System

GamEngine uses **Slang** for engine shaders.

Built-in shaders are compiled during the CMake build and packaged with the editor/runtime.

Shader categories currently include:

```text
Common
Culling
Environment
Forward
Grass
Particles
PostProcess
Samples
Shadow
UI
Water
```

### Shader Graph

The editor includes a node-based Shader Graph system.

The current graph model supports nodes such as:

* constants and vector values
* material properties
* arithmetic
* Lerp
* Clamp / Saturate
* Sin / Cos
* Dot / Normalize / Length
* texture sampling
* UV
* Time
* Normal
* View Direction
* Fresnel
* Surface Output

Shader Graph assets are compiled into Slang/SPIR-V for use by the renderer.

## Scripting

Gameplay code is written as native C++ scripts.

Scripts derive from `Engine::Script` and provide lifecycle callbacks:

```cpp
class PlayerController final : public Engine::Script {
public:
    void onCreate() override {
    }

    void onEnable() override {
    }

    void onUpdate(float deltaTime) override {
    }

    void onDisable() override {
    }

    void onDestroy() override {
    }
};
```

The scripting API exposes higher-level access to:

* the script entity
* actors
* parent/child relationships
* transforms
* scenes
* physics

Native scripts are compiled into `GameScripts.dll`.

The editor supports script module rebuilding and hot reload.

## Physics

GamEngine integrates NVIDIA PhysX.

The engine contains support for:

* rigid bodies
* colliders
* runtime physics state
* scene-bound physics queries
* physics-safe transform commands for dynamic rigid bodies

PhysX is fetched automatically by CMake.

## Asset Pipeline

The engine contains its own asset management and cooking infrastructure.

Current asset-related functionality includes:

* glTF / GLB loading
* PBR material import
* embedded and external glTF textures
* texture cooking
* custom `.gmesh` mesh assets
* custom `.gtex` texture assets
* meshlet generation
* asset handles and residency tracking
* virtual texture infrastructure
* environment and BRDF assets

The editor can import individual files, directories, and ZIP archives on Windows.

## Projects

A GamEngine project is described by a `GamEngine.project` manifest.

Example:

```ini
# GamEngine project
name = MyGame
asset_root = Assets
startup_scene = Assets/Scenes/Main.scene
```

Project paths are stored relative to the project directory so projects can be moved without rewriting absolute paths.

The repository contains two project directories:

* `MyGame/` — default development project
* `DEMO/` — demonstration project

## Requirements

The current development configuration is primarily targeted at **Windows x64**.

Required tools:

* Windows 10 or Windows 11 x64
* Visual Studio 2022 with C++ development tools
* CMake 4.4 or newer
* Git
* Vulkan SDK with Vulkan 1.4 development files
* `slangc` available to CMake
* SDL3 CMake package available to `find_package(SDL3 CONFIG ...)`

The Vulkan SDK installation is also used as a discovery location for GLM and Vulkan Memory Allocator headers.

Most remaining third-party dependencies are downloaded automatically through CMake `FetchContent`.

Notable dependencies include:

* Vulkan
* SDL3
* Slang
* GLM
* Vulkan Memory Allocator
* NVIDIA PhysX
* Dear ImGui
* ImNodes
* AMD Compressonator Core
* TinyEXR
* GoogleTest

## Building

Clone the repository:

```bash
git clone https://github.com/B4rtekk1/GamEngine.git
cd GamEngine
```

### Development build

Configure:

```bash
cmake --preset dev
```

Build:

```bash
cmake --build --preset dev
```

### Release build

Configure:

```bash
cmake --preset release
```

Build:

```bash
cmake --build --preset release
```

Generated build files are placed under:

```text
out/build/<preset>/
```

The default CMake project uses:

```text
MyGame/
```

as the game project directory.

To build the engine against another GamEngine project:

```bash
cmake --preset dev -DGAMEENGINE_GAME_DIRECTORY="C:/Path/To/Project"
cmake --build --preset dev
```

The selected directory must contain:

```text
GamEngine.project
Assets/
```

## Tests

Tests are disabled in the default development preset.

Configure the test build:

```bash
cmake --preset dev-tests
```

Build:

```bash
cmake --build --preset dev-tests
```

Run the test suite:

```bash
ctest --preset dev-tests
```

The test suite covers multiple engine areas, including:

* math
* ECS
* components
* time
* task scheduling
* scene construction
* projects
* scripting
* GPU scene management
* Render Graph
* Shader Graph
* physics
* UI/layout

## Packaging

GamEngine provides dedicated CMake packaging targets.

### Portable Editor

Create a portable editor package:

```bash
cmake --preset release
cmake --build --preset release --target PackageEditor
```

The package is generated under the release build directory as:

```text
GamEngineEditor/
```

The portable editor contains the engine runtime, shader sources, compiled shaders, scripting SDK, Slang runtime files, and the standalone Win64 runtime template required by the game build pipeline.

### Runtime Template

A standalone runtime template can be generated with:

```bash
cmake --build --preset release --target PackageRuntime
```

The resulting package is placed in:

```text
GamEngineRuntime/
```

## Building a Game

The editor contains a native **Build Game** pipeline.

A game export:

1. builds the project's native C++ scripts in Release mode;
2. creates `GameScripts.dll`;
3. copies the prebuilt Win64 runtime;
4. renames `GamEnginePlayer.exe` to the project name;
5. copies cooked project content;
6. writes a release `GamEngine.project` manifest;
7. validates the required runtime files.

The default output directory is:

```text
<Project>/Build/Windows/<ProjectName>/
```

The current game export pipeline targets Windows x64.

## Quality Presets

GamEngine exposes coarse rendering presets:

| Preset | Shadows | GTAO   | Anti-aliasing |
| ------ | ------- | ------ | ------------- |
| Low    | Low     | Low    | Off           |
| Medium | Medium  | Medium | TAA           |
| High   | High    | High   | TAA           |
| Ultra  | Ultra   | Ultra  | TAA           |

Renderer features can also be controlled independently through `Engine::RenderConfig`.

## Diagnostics and Profiling

GamEngine contains CPU/GPU diagnostic infrastructure for engine development.

Available systems include:

* engine diagnostics
* editor console
* CPU profiling
* Vulkan GPU timestamp profiling
* renderer debug views
* GTAO debug modes
* PBR lighting debug modes
* Virtual Shadow Map debug modes
* GPU memory budget management

## Platform Support

The codebase currently contains platform implementations for both Windows and Linux for selected services such as:

* file watching
* user paths
* platform abstraction

However, the primary development preset uses Visual Studio 2022 and the current standalone **Build Game** implementation packages a **Win64** runtime.

Windows is therefore the primary supported development target at the moment.

## Repository Status

GamEngine is an actively developed engine and should currently be treated as an experimental project rather than a stable production SDK.

Several systems are already substantial, but renderer architecture, editor APIs, asset formats, and tooling may continue to change.

## Contributing

Contributions, bug reports, and technical discussion are welcome.

Before contributing, see:

* `CONTRIBUTING.md`
* `CODE_OF_CONDUCT.md`
* `SECURITY.md`

## License

GamEngine is licensed under the **Apache License 2.0**.

See [LICENSE](LICENSE) for the full license text.
