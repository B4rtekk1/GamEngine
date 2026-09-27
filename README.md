# GamEngine

A modern experimental 3D game engine written in **C++23**, built around **Vulkan 1.4** and GPU-driven rendering.

GamEngine focuses on real-time graphics, low CPU overhead, native C++ gameplay, and an integrated editor.

## Demo

https://github.com/user-attachments/assets/890f518c-f8e2-4121-b0ee-bb9da5fcbfc0

## Features

### Rendering

- GPU-driven rendering
- Physically Based Rendering
- Virtual Shadow Maps
- Temporal Anti-Aliasing
- GPU particles
- And more...

### Engine

- Entity Component System
- Native C++ gameplay scripting
- Asset management and cooking
- NVIDIA PhysX integration
- Standalone game builds
- And more...

## Editor

- Scene Hierarchy
- Component Inspector
- Asset Manager
- Shader Graph
- CPU / GPU Profiler
- And more...

## Build

### Requirements

- Windows 10 / 11
- Visual Studio 2022
- CMake 4.4+
- Vulkan SDK
- Slang
- SDL3
- Git

Clone:

```bash
git clone --recurse-submodules https://github.com/B4rtekk1/GamEngine.git
cd GamEngine
```

Build the editor:

```bash
cmake --preset dev
cmake --build --preset dev
```

Release:

```bash
cmake --preset release
cmake --build --preset release
```

## Projects

Projects use a simple structure:

```text
MyGame/
├── GamEngine.project
└── Assets/
```

A custom project can be selected during configuration:

```bash
cmake --preset dev -DGAMEENGINE_GAME_DIRECTORY="C:/Path/To/MyGame"
cmake --build --preset dev
```

## Game Builds

Standalone builds can be exported directly from the editor.

A typical build contains:

```text
Game/
├── Game.exe
├── Engine.dll
├── GameScripts.dll
├── GamEngine.project
├── shaders/
└── Content/
```

The exported game does not require the GamEngine source tree.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Licensed under the [Apache License 2.0](LICENSE).