# GamEngine

GamEngine is modern experimental 3D game engine. Engine uses Vulkan 1.4, SDL3 and other third party tools when necessary.
Engine is highly focused on GPU driven rendering - low CPU overhead.

# DEMO

https://github.com/user-attachments/assets/890f518c-f8e2-4121-b0ee-bb9da5fcbfc0

## Features

### Rendering
 
- PBR - Physically based rendering
- VSM - Custom experimental implementation of Virtual Shadow Maps
- RTCS - Ray Traced Contact Shadows
- TAA/MSAA - Anti-aliasing methods
- GPU particle system.
- Highly experimental Virtual Water System(in progress)


### Engine

- Native c++ scripting
- Scripts hot reloading
- Nvidia PhysX integration
- Assets manager/compressor
- Standalone game builds

### Editor

- Scene hierarchy
- Asset manager
- Shader graph
- Component inspector


## Build

### Requirements

- Windows 11 x64 or Linux x86-64 - tested on Ubuntu 26.04
- C++ compiler - MSVC 2022 recommended for Windows
- Cmake 4.4+
- Vulkan SDK
- Slang
- SDL3
- GLM

### Building

Clone:
```bash
git clone https://github.com/B4rtekk1/GamEngine.git
cd GamEngine
```

Linux:
```bash
cmake --preset linux-release
cmake --build --preset linux-release --parallel "$(nproc)"
./out/build/linux-release/Editor/Editor
```

Windows:
```bash
cmake --preset release
cmake build --preset release
./out/build/linux-release/Editor/Editor.exe
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Licensed under the [Apache License 2.0](LICENSE).
