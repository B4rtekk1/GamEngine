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

- Linux x86-64 or Windows 10 / 11 x64
- A C++23 compiler: GCC 15+ or a compatible Clang/libstdc++ on Linux; Visual Studio 2022 on Windows
- CMake 4.4+
- Vulkan SDK
- Slang
- SDL3
- GLM headers
- Git

On Linux, install the Vulkan SDK (including `slangc` and `glslangValidator`), SDL3 development
files, GLM, and Make. Export `VULKAN_SDK` and add its `bin` directory to
`PATH` if the SDK is not installed system-wide. CMake downloads PhysX,
ImGui, the texture codecs, and ISPC during the first configuration.
Rendering requires a Vulkan 1.4 capable GPU and driver.

Clone:

```bash
git clone --recurse-submodules https://github.com/B4rtekk1/GamEngine.git
cd GamEngine
```

Build the editor:

Linux:

```bash
cmake --preset linux-dev
cmake --build --preset linux-dev --parallel 4
./out/build/linux-dev/Editor/Editor
```

Linux tests and portable Release package:

```bash
cmake --preset linux-tests
cmake --build --preset linux-tests --parallel 4
ctest --preset linux-tests
cmake --preset linux-release
cmake --build --preset linux-release --target PackageEditor --parallel 4
```

The portable editor is written to `out/build/linux-release/GamEngineEditor`.
Keep the directory together with its shared libraries, SDK, shader compiler,
and runtime template. Exported Linux games preserve executable permissions
and load their shared libraries relative to the executable directory.
The native bootstrapper in `Installer/Bootstrapper` is Windows-specific;
Linux uses the portable package directly.

The editor and runtime bundle their default UI font and license, so a game
does not depend on distribution-specific system font paths. See
[the Linux support audit](LINUX_SUPPORT.md) for validation results and limits.

Windows:

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

On Linux the executable has no extension, the engine is `libEngine.so`,
and the script module is `libGameScripts.so`. SDL3 is included in the runtime
template. Script authoring and hot reload require CMake and a compatible
C++23 compiler; running an exported game does not require these tools.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Licensed under the [Apache License 2.0](LICENSE).
