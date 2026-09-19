# Contributing to GamEngine

Thank you for your interest in contributing to GamEngine.

GamEngine is a C++/Vulkan 3D engine focused on modern rendering techniques, GPU-driven rendering, high performance, and explicit control over graphics resources. Contributions are welcome, but changes to low-level rendering code must preserve correctness, maintainability, and measurable performance.

## Contribution priorities

GamEngine favors:

- correctness before optimization;
- profiling before optimization;
- measurable improvements over speculative micro-optimizations;
- explicit Vulkan synchronization and resource ownership;
- GPU-driven solutions where they reduce CPU overhead;
- predictable frame-time behavior;
- low steady-state CPU overhead;
- maintainable abstractions that do not hide expensive GPU work;
- clear ownership of render resources;
- incremental architectural changes instead of large unreviewable rewrites.

## Before contributing

For small fixes, documentation updates, tests, or isolated improvements, feel free to open a pull request directly.

For larger changes, please open an issue or discussion first. This is especially recommended for changes involving:

- renderer architecture;
- RenderGraph behavior;
- resource lifetime management;
- GPU-driven rendering;
- virtual shadow maps;
- GTAO;
- temporal anti-aliasing;
- water rendering;
- meshlet rendering;
- shader interfaces;
- descriptor layouts;
- frame synchronization;
- public engine APIs;
- major ECS changes.

This helps avoid duplicated work and large changes that conflict with the current renderer direction.

## Development environment

The primary development environment is Windows with the Visual Studio toolchain.

Recommended tools:

- Windows 11;
- Visual Studio 2022 with the C++ workload;
- CMake;
- Vulkan SDK;
- Slang compiler;
- Git;
- NVIDIA Nsight Graphics for GPU profiling;
- a recent NVIDIA, AMD, or Intel Vulkan driver.

If you use another operating system or toolchain, make sure the complete engine, editor, shaders, and tests build successfully before submitting changes.

## Building the project

A typical Visual Studio build can be configured with:

```powershell
cmake -S . -B cmake-build-debug-visual-studio -G "Visual Studio 17 2022"
```

Build and package the Release editor with:

```powershell
cmake --build cmake-build-debug-visual-studio `
    --config Release `
    --target PackageEditor `
    --parallel 4
```

When debugging build-system or shader-compilation problems, use a verbose build:

```powershell
cmake --build cmake-build-debug-visual-studio `
    --config Release `
    --target PackageEditor `
    --verbose
```

If your change affects CMake configuration, shader compilation, generated resources, or packaging, test it from a clean build directory before submitting.

## Repository layout

The main repository layout is:

```text
Engine/
  Include/       Public and internal engine headers
  Source/        Engine implementation
  Shaders/       Slang shaders and shared shader libraries

Editor/          GamEngine editor
Player/          Standalone runtime
Assets/          Engine assets and resources
cmake/           CMake helpers and shader compilation
Tests/           Automated tests
```

Large architectural documentation should live in `docs/` or a dedicated architecture document rather than in this file.

## C++ guidelines

Follow the style already used in the surrounding code.

General expectations:

- prefer RAII for resource ownership;
- avoid raw `new` and `delete` unless there is a clear ownership reason;
- prefer standard containers and utilities;
- use `std::span` for non-owning contiguous ranges where appropriate;
- use `std::array` for fixed-size collections;
- use `std::vector` for dynamically sized owned collections;
- keep ownership explicit;
- prefer const-correct code;
- use `noexcept` only when the function can actually guarantee it;
- avoid hidden allocations in hot frame paths;
- avoid unnecessary copies of large render data;
- avoid global mutable state;
- keep functions focused and reasonably small;
- do not introduce abstractions that obscure GPU cost or synchronization.

Use existing naming conventions in the subsystem you are modifying.

Do not mix unrelated formatting changes into functional commits.

## Vulkan rules

Rendering changes must not introduce Vulkan validation errors.

Every change touching Vulkan resources must account for:

- image layouts;
- pipeline stages;
- access masks;
- queue ownership;
- resource lifetime;
- synchronization;
- descriptor validity;
- command-buffer lifetime;
- frames in flight.

### Dynamic rendering

GamEngine uses Vulkan dynamic rendering in modern render paths.

Dynamic rendering does not perform implicit image layout transitions.

Every transition required before or after `vkCmdBeginRendering` / `vkCmdEndRendering` must be performed explicitly or be owned by the RenderGraph.

Do not assume behavior previously provided by:

```text
VkRenderPass::initialLayout
VkRenderPass::finalLayout
```

exists with dynamic rendering.

### Image layouts

The layout declared in a descriptor or rendering attachment must match the actual image layout at the time the image is accessed.

Examples include:

```text
VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
VK_IMAGE_LAYOUT_GENERAL
VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
```

Do not fix validation errors by changing only the descriptor's declared layout. Fix the actual resource transition and synchronization.

### Synchronization

Prefer Vulkan Synchronization 2:

```cpp
VkMemoryBarrier2
VkBufferMemoryBarrier2
VkImageMemoryBarrier2
VkDependencyInfo
vkCmdPipelineBarrier2
```

Barriers should be as narrow as practical.

Avoid using:

```text
VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
VK_ACCESS_2_MEMORY_READ_BIT
VK_ACCESS_2_MEMORY_WRITE_BIT
```

as a default solution when a narrower dependency is known.

Broad barriers may be acceptable during diagnostics, but production code should describe the actual producer and consumer stages.

### Queue synchronization

Do not introduce unnecessary synchronization between graphics, compute, and transfer queues.

Preserve asynchronous compute and transfer paths when possible.

Avoid:

```text
vkDeviceWaitIdle
vkQueueWaitIdle
```

inside normal frame execution.

These calls are acceptable during shutdown, exceptional resource rebuilds, or carefully justified diagnostics.

### Resource lifetime

Resources referenced by an in-flight command buffer must remain alive until GPU execution completes.

When replacing resources:

- respect frame fences or timeline semaphores;
- do not destroy descriptors or image views still referenced by submitted work;
- do not overwrite per-frame resources that belong to another in-flight frame.

## RenderGraph guidelines

RenderGraph-managed resources should have one clear owner for synchronization and layout tracking.

Prefer declaring resource usage through the graph rather than manually inserting barriers around graph-owned resources.

A render pass should declare whether it:

- reads a texture;
- writes a texture;
- samples a texture;
- uses a color attachment;
- uses a depth attachment;
- writes storage;
- reads or writes buffers;
- exports a resource for later external use.

Do not silently change an image layout inside a pass if the RenderGraph believes it owns that layout, unless the resource is explicitly documented as externally managed.

`setFinalTextureState()` or equivalent APIs must correspond to a real GPU transition when required. Updating only internal bookkeeping is not sufficient.

When adding a new RenderGraph transition, verify both:

1. the graph's tracked state;
2. the actual Vulkan image layout observed by validation layers.

## Shader guidelines

GamEngine shaders are primarily written in Slang and compiled to SPIR-V.

When adding or modifying shaders:

- keep shader interfaces synchronized with the C++ side;
- keep descriptor bindings synchronized with descriptor-set layouts;
- keep push-constant layouts synchronized with C++ structures;
- keep buffer structures ABI-compatible across C++ and Slang;
- avoid unnecessary divergence in hot pixel and compute shaders;
- avoid excessive register pressure without measuring its effect;
- avoid unbounded loops in performance-critical paths;
- use explicit LOD only when required;
- preserve debug information in profiling builds.

Any descriptor binding change must update both:

```text
the Slang declaration
and
the corresponding Vulkan descriptor-set layout/write code
```

in the same change.

### Shader debug information

Shaders used for profiling should retain source-level debug information.

The normal shader build should include the project's configured Slang debug flags, such as:

```text
-g2
```

Do not globally switch Release shaders to `-O0` just to make profiling easier.

Disabling optimization changes generated GPU code and can make performance captures unrepresentative.

If a shader fails to appear in Nsight source-level profiling, verify the final packaged `.spv`, not only the intermediate build output.

## Performance-sensitive changes

Changes to hot rendering paths should include measurements when they may affect CPU or GPU performance.

Examples:

- forward rendering;
- shadow rendering;
- GTAO;
- TAA;
- bloom;
- water;
- meshlet processing;
- culling;
- RenderGraph execution;
- GPU scene updates;
- upload paths;
- descriptor updates;
- frame synchronization.

Prefer frame time over FPS when reporting performance.

For example:

```text
GPU: NVIDIA GeForce ...
Driver: ...
Resolution: 1920x1080
Scene: ...
Build: Release

Before:
GPU frame time: 8.10 ms
GTAO: 1.35 ms

After:
GPU frame time: 7.62 ms
GTAO: 0.91 ms

Profiler:
NVIDIA Nsight Graphics
```

Do not claim a performance improvement from a single noisy sample.

Use the same:

- scene;
- camera position;
- resolution;
- graphics settings;
- build configuration;
- driver;
- profiling method.

When optimizing GPU code, inspect the actual bottleneck before changing code.

Useful metrics include:

- GPU pass duration;
- SM throughput;
- texture/L1 throughput;
- L2 throughput;
- VRAM throughput;
- warp occupancy;
- long scoreboard stalls;
- register pressure;
- draw/dispatch count;
- CPU submission cost.

## CPU performance

Avoid adding steady-state per-frame work when data can instead be updated:

- on resource creation;
- on scene changes;
- on transform changes;
- on material changes;
- on visibility changes;
- once per dirty resource.

Avoid repeatedly rebuilding large vectors, hash tables, descriptors, or render objects when nothing changed.

Prefer revision tracking and dirty flags where they make ownership and behavior clearer.

## Memory and allocations

Avoid avoidable allocations in frame-critical code.

In hot paths, prefer:

- persistent buffers;
- reusable vectors;
- arenas;
- frame-local allocators;
- preallocated staging buffers;
- ring buffers;
- cached descriptors;
- dirty-range uploads.

Do not increase RAM or VRAM usage significantly without documenting the reason and expected benefit.

## Testing

Before submitting a pull request, verify that:

- the project configures successfully;
- the Release editor builds;
- shaders compile successfully;
- the editor starts successfully;
- the test scene loads;
- modified features work as intended;
- no new Vulkan validation errors are produced;
- no new compiler warnings are introduced;
- automated tests pass;
- there are no obvious graphical regressions.

For renderer changes, run at least one representative scene with Vulkan Validation Layers enabled.

For visual changes, test multiple camera angles and distances.

For synchronization changes, test:

- first frame;
- scene reload;
- viewport resize;
- swapchain recreation;
- Play;
- Stop;
- switching editor views;
- changing graphics settings when applicable.

## Visual changes

Pull requests that modify rendering output should include screenshots or a short video when practical.

Examples include:

- lighting;
- shadows;
- GTAO;
- water;
- sky;
- particles;
- post-processing;
- TAA;
- material rendering;
- editor viewport rendering.

When fixing an artifact, include:

```text
Before
After
Steps to reproduce
```

## Vulkan validation reports

When reporting or fixing a Vulkan validation issue, include the complete VUID.

Good:

```text
VUID-vkCmdBeginRendering-pRenderingInfo-09592
```

Do not report only:

```text
Vulkan error
```

The complete validation message usually contains enough information to identify the resource state that is wrong.

## Debug utilities

When adding major persistent Vulkan resources, consider assigning meaningful Vulkan debug names when debug utilities are available.

Useful examples:

```text
Game.HDR
SceneViewport.Color
Game.Depth
Scene.Depth
GTAO.Raw
GTAO.Filtered
TAA.History0
TAA.History1
VSM.GameAtlas
VSM.SceneAtlas
Water.OpaqueColor
```

Meaningful object names make validation and GPU captures significantly easier to diagnose.

## Tests

New engine behavior should include automated tests when practical.

Tests are especially valuable for:

- math;
- ECS behavior;
- serialization;
- resource state tracking;
- RenderGraph dependencies;
- scene hierarchy;
- asset processing;
- deterministic CPU algorithms.

GPU output tests may require a separate strategy and are not expected for every rendering change.

## Commit messages

Use concise commit messages that describe the change.

Preferred prefixes:

```text
feat:
fix:
perf:
refactor:
docs:
test:
build:
chore:
```

Examples:

```text
fix: transition VSM atlas before forward sampling
perf: reduce GTAO depth sampling latency
refactor: migrate shadow rendering to dynamic rendering
docs: document RenderGraph synchronization rules
test: add hierarchy transform regression coverage
```

Avoid messages such as:

```text
update
changes
fix stuff
work
test123
```

Keep commits focused when possible.

A commit that fixes a Vulkan layout bug should not also reformat unrelated editor code.

## Pull requests

A pull request should explain:

- what problem it solves;
- what changed;
- why the chosen solution was used;
- how it was tested;
- whether it changes rendering output;
- whether it affects performance;
- any known limitations.

A useful pull request description can follow this format:

```markdown
## Problem

Describe the problem or limitation.

## Changes

Describe the implementation.

## Validation

- Release build succeeds
- Vulkan validation clean
- Tested scene reload
- Tested Play/Stop

## Performance

Before:
...

After:
...

## Visual comparison

Attach screenshots or video if applicable.
```

## Large pull requests

Avoid mixing unrelated systems into a single pull request.

Prefer separate changes for:

```text
renderer architecture
GTAO
VSM
water
editor UI
asset pipeline
ECS
documentation
```

Large refactors should remain reviewable and should ideally preserve behavior before introducing new features.

## Bug reports

A useful bug report should include:

- commit SHA;
- operating system;
- GPU;
- GPU driver version;
- build configuration;
- exact reproduction steps;
- expected behavior;
- actual behavior;
- complete validation output when relevant;
- screenshot or video for graphical issues.

For performance regressions, also include:

- resolution;
- scene;
- graphics settings;
- CPU frame time;
- GPU frame time;
- profiler used.

## Feature requests

Feature requests should describe the problem being solved rather than only the desired implementation.

Good:

```text
Large scenes become CPU-bound because visibility submission scales with the
number of renderers. A GPU-driven meshlet path could reduce submission cost.
```

Less useful:

```text
Add mesh shaders.
```

Explain the target use case, expected benefit, and known trade-offs.

## Documentation

Public APIs and non-obvious systems should be documented.

Comments should explain:

- why something is required;
- synchronization assumptions;
- ownership assumptions;
- unusual performance trade-offs;
- non-obvious mathematical choices.

Avoid comments that merely repeat the code.

Good:

```cpp
// The atlas is sampled by the forward pass immediately after shadow rendering.
// Dynamic rendering has no finalLayout transition, so publish depth writes and
// transition explicitly before descriptor sampling.
```

Less useful:

```cpp
// Transition image.
```

## Generated and third-party files

Do not manually edit generated files unless the generation pipeline explicitly requires it.

Do not modify third-party code solely to match GamEngine formatting.

If third-party code must be patched, keep the patch minimal and document why it is needed.

## Security and robustness

Do not assume asset data is valid.

Importers, serializers, and runtime asset systems should validate:

- counts;
- offsets;
- sizes;
- indices;
- file paths;
- resource limits.

Avoid integer overflows and out-of-bounds accesses when processing external assets.

## Final checklist

Before opening a pull request:

- [ ] The project builds successfully.
- [ ] The Release editor starts.
- [ ] Modified shaders compile.
- [ ] Vulkan Validation Layers report no new errors.
- [ ] No new compiler warnings are introduced.
- [ ] Tests pass.
- [ ] The change was tested in a representative scene.
- [ ] Visual changes include screenshots or video where useful.
- [ ] Performance-sensitive changes include measurements.
- [ ] No unnecessary per-frame allocations were added.
- [ ] No unnecessary queue/device waits were added.
- [ ] Descriptor layouts match shader bindings.
- [ ] Image layouts and synchronization are explicit and correct.
- [ ] The pull request explains the problem, implementation, and validation.

Thank you for contributing to GamEngine.
