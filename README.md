# GamEngine

GamEngine is an experimental 3D game engine written in **C++23**, built around **Vulkan 1.4** and a modern GPU-driven rendering architecture.

It includes an integrated editor, native C++ gameplay scripting, asset cooking, standalone game builds, and a renderer designed around GPU-side visibility and scene processing.

> GamEngine is under active development. APIs, project formats, rendering systems, and editor workflows may change.

## Quick Start

### Requirements

Currently supported:

* Windows 10 / 11 x64
* Visual Studio 2022 with C++ development tools
* CMake 4.4+
* Git
* Vulkan SDK with Vulkan 1.4 support
* Slang compiler (`slangc`)
* SDL3

Several other dependencies, including PhysX, Dear ImGui and ImNodes, are downloaded automatically by CMake.

### Clone

```bash
git clone https://github.com/B4rtekk1/GamEngine.git
cd GamEngine
```

### Build the Editor

Development build:

```bash
cmake --preset dev
cmake --build --preset dev
```

The default project is:

```text
MyGame/
```

A development build of the editor is typically located under:

```text
out/build/dev/Editor/Debug/
```

Run:

```text
Editor.exe
```

### Release Build

```bash
cmake --preset release
cmake --build --preset release
```

Release artifacts are generated under:

```text
out/build/release/
```

## Projects

By default, GamEngine uses the project stored in:

```text
MyGame/
├── GamEngine.project
└── Assets/
```

A different project can be selected during CMake configuration:

```bash
cmake --preset dev -DGAMEENGINE_GAME_DIRECTORY="C:/Path/To/MyGame"
cmake --build --preset dev
```

The selected directory must contain:

```text
GamEngine.project
Assets/
```

## Editor

The GamEngine Editor provides the main development workflow for creating scenes and games.

Main tools include:

* Scene Hierarchy
* Component Inspector
* Asset Manager
* Shader Graph
* Terrain tools
* CPU/GPU Profiler
* Console and integrated terminal
* Play / Pause workflow
* Script hot reload
* Shader hot reload
* Standalone game builds

Gameplay code can be written as native C++ modules and reloaded during development.

## Building a Game

Standalone Windows builds can be created directly from the editor using **Build Game**.

The export pipeline packages the runtime, cooked project content, compiled shaders, project configuration and gameplay module.

A typical game build looks like:

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

The exported game does not require the GamEngine source tree.

## Portable Editor

A portable editor package can be created with:

```bash
cmake --preset release
cmake --build --preset release --target PackageEditor
```

The resulting package is placed in:

```text
out/build/release/GamEngineEditor/
```

The package contains the editor, engine runtime, shader compiler support, C++ scripting SDK and standalone runtime template.

A standalone runtime package can also be generated separately:

```bash
cmake --build --preset release --target PackageRuntime
```

Output:

```text
out/build/release/GamEngineRuntime/
```

## Rendering

GamEngine uses a GPU-oriented renderer designed to minimize CPU-side draw submission and visibility work.

Currently implemented systems include:

* GPU-driven rendering
* GPU frustum culling
* Hi-Z occlusion culling
* Meshlet rendering
* GPU Scene database
* Render Graph
* Physically Based Rendering
* Directional Virtual Shadow Maps
* GTAO
* Temporal Anti-Aliasing
* MSAA
* Image-Based Lighting
* Reflection probes
* Bloom
* ACES tonemapping
* GPU particle simulation
* GPU-driven grass
* Experimental Virtual Water
* Slang shader pipeline
* Shader Graph

## Engine Systems

The runtime currently provides:

* Entity Component System
* Scene hierarchy
* Prefabs
* Asset management and cooking
* glTF / GLB importing
* NVIDIA PhysX integration
* Native C++ gameplay scripting
* Script hot reload
* CPU and GPU profiling


## Development Status

GamEngine is currently an experimental engine rather than a stable production SDK.

The renderer, editor, asset pipeline, scripting API and project format are still evolving. Breaking changes should be expected between revisions.

The primary supported platform is currently **Windows x64**.

## Contributing

See:

* [CONTRIBUTING.md](CONTRIBUTING.md)
* [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)
* [SECURITY.md](SECURITY.md)

## License

GamEngine is licensed under the **Apache License 2.0**.

See [LICENSE](LICENSE) for details.
