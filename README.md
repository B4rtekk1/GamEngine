# GamEngine

GamEngine is a high-performance 3D game engine written in **C++23** and built on **Vulkan 1.4**.

The project focuses on modern GPU-driven rendering, native C++ scripting and an integrated editor/runtime workflow.

> GamEngine is under active development. APIs, rendering systems, and project formats may change.

## Features

### Rendering

* Vulkan 1.4 renderer
* GPU-driven rendering
* GPU frustum and Hi-Z occlusion culling
* Meshlet rendering
* Physically Based Rendering
* Directional Virtual Shadow Maps
* GTAO
* Temporal Anti-Aliasing and MSAA
* Image-Based Lighting
* Reflection probes
* Bloom
* ACES tonemapping
* GPU particle simulation
* GPU-driven grass
* Experimental Virtual Water system
* Slang shader pipeline
* Shader Graph

### Engine

* Entity Component System
* Scene hierarchy and prefabs
* NVIDIA PhysX integration
* Native C++ gameplay scripting
* Script hot reload
* Asset management and cooking
* glTF / GLB import
* Render Graph
* GPU Scene database
* CPU and GPU profiling

### Editor

GamEngine includes an editor built with Dear ImGui.

It provides:

* Scene Hierarchy
* Inspector / Components
* Asset Manager
* Shader Graph
* Profiler
* Console
* Terrain tools
* Play / Pause workflow
* Shader and script hot reload
* Standalone game builds

## Project Structure

```text
GamEngine/
├── Engine/      Engine runtime and renderer
├── Editor/      GamEngine Editor
├── Player/      Standalone game runtime
├── Platform/    Platform-specific code
├── Tests/       Automated tests
├── Tools/       Build and asset tools
├── Assets/      Development assets
├── MyGame/      Default project
├── DEMO/        Demo project
└── cmake/       Build and packaging scripts
```

## Requirements

The primary supported platform is currently **Windows x64**.

Required tools:

* Windows 10 / 11
* Visual Studio 2022 with C++ tools
* CMake 4.4+
* Git
* Vulkan SDK
* Slang compiler (`slangc`)
* SDL3

Other dependencies are downloaded automatically through CMake where possible.

Major dependencies include:

* Vulkan
* SDL3
* Slang
* GLM
* Vulkan Memory Allocator
* NVIDIA PhysX
* Dear ImGui
* ImNodes
* GoogleTest

## Building

Clone the repository:

```bash
git clone https://github.com/B4rtekk1/GamEngine.git
cd GamEngine
```

### Development

```bash
cmake --preset dev
cmake --build --preset dev
```

### Release

```bash
cmake --preset release
cmake --build --preset release
```

Build output is placed in:

```text
out/build/<preset>/
```

## Packaging

Create a portable GamEngine Editor package:

```bash
cmake --preset release
cmake --build --preset release --target PackageEditor
```

The package is generated as:

```text
GamEngineEditor/
```

A standalone runtime package can be created with:

```bash
cmake --build --preset release --target PackageRuntime
```

## Building Games

The editor includes a **Build Game** pipeline for Windows x64.

Exported games contain the standalone runtime, cooked content, shaders, project configuration, and native gameplay module.

Typical output:

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

## Tests

```bash
cmake --preset dev-tests
cmake --build --preset dev-tests
ctest --preset dev-tests
```

## Status

GamEngine is an experimental engine under active development.

The renderer, editor, asset pipeline, and tooling are still evolving and should not yet be considered a stable production SDK.

## Contributing

See:

* [CONTRIBUTING.md](CONTRIBUTING.md)
* [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)
* [SECURITY.md](SECURITY.md)

## License

GamEngine is licensed under the **Apache License 2.0**.

See [LICENSE](LICENSE) for details.
