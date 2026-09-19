## Summary

Describe what this pull request changes and why.

<!--
Keep this section concise.
Examples:
- Fixes an invalid Vulkan image layout transition in Scene View.
- Reduces GTAO sampling cost by batching depth fetches.
- Adds a new editor workflow for material assets.
-->

## Problem

What problem, limitation, regression, or feature request does this PR address?

If applicable, link the related issue:

```text
Closes #
Related to #
```

## Changes

List the important implementation changes.

- 
- 
- 

## Type of change

Select all that apply.

- [ ] Bug fix
- [ ] New feature
- [ ] Performance improvement
- [ ] Refactor
- [ ] Renderer / Vulkan change
- [ ] Shader change
- [ ] RenderGraph change
- [ ] Editor change
- [ ] Asset pipeline change
- [ ] ECS / scene change
- [ ] Build system change
- [ ] Tests
- [ ] Documentation
- [ ] Breaking change

## Validation

Describe how the change was tested.

- [ ] Release build succeeds
- [ ] Editor starts successfully
- [ ] Representative scene loads successfully
- [ ] Modified shaders compile successfully
- [ ] Automated tests pass
- [ ] No new compiler warnings
- [ ] No new Vulkan validation errors
- [ ] Play / Stop tested when relevant
- [ ] Scene reload tested when relevant
- [ ] Viewport resize tested when relevant
- [ ] Swapchain recreation tested when relevant

Additional validation details:

```text
Build configuration:
Operating system:
GPU:
Driver:
Scene:
Steps tested:
```

## Vulkan / RenderGraph checklist

Complete this section if the PR touches rendering, Vulkan resources, synchronization, or RenderGraph code.

- [ ] Image layouts match actual resource usage
- [ ] Descriptor image layouts match the real image layouts
- [ ] Dynamic rendering transitions are explicit
- [ ] Pipeline stage masks describe the real producer and consumer
- [ ] Access masks describe the real accesses
- [ ] Queue ownership is correct
- [ ] Resource lifetimes are valid for all in-flight frames
- [ ] No unnecessary `vkDeviceWaitIdle`
- [ ] No unnecessary `vkQueueWaitIdle`
- [ ] No broad synchronization was added without justification
- [ ] RenderGraph state tracking matches real Vulkan state
- [ ] No resource is transitioned both manually and by the RenderGraph unintentionally

If any item does not apply, explain briefly:

```text
N/A:
```

## Shader checklist

Complete this section if the PR changes shaders or shader interfaces.

- [ ] Slang code compiles successfully
- [ ] Descriptor bindings match C++ descriptor-set layouts
- [ ] Push constants match the C++ layout
- [ ] Shared buffer structures remain ABI-compatible
- [ ] New shader variants are added to the build system where required
- [ ] Final packaged SPIR-V is generated correctly
- [ ] Profiling/debug information remains available where expected
- [ ] Register pressure or divergence changes were measured when performance-sensitive

## Performance

Complete this section for changes that may affect CPU or GPU performance.

If there is no meaningful performance impact, write:

```text
No measurable performance impact expected.
```

Otherwise provide before/after measurements.

```text
GPU:
CPU:
Driver:
Resolution:
Scene:
Build configuration:
Profiler:

Before:
GPU frame time:
CPU frame time:
Affected pass:

After:
GPU frame time:
CPU frame time:
Affected pass:
```

For GPU optimizations, include relevant profiler evidence where useful, for example:

```text
SM throughput:
L1/TEX:
L2:
VRAM:
Warp occupancy:
Long scoreboard stalls:
Draw/dispatch count:
```

Do not report only FPS when frame time is available.

## Visual comparison

Required for changes that intentionally alter rendering output.

### Before

<!-- Add screenshot, video, or capture -->

### After

<!-- Add screenshot, video, or capture -->

If there is no visual change:

```text
No intended visual change.
```

## Regression risk

What could this change break?

Examples:

- first-frame resource initialization;
- Scene View;
- Game View;
- Play / Stop;
- TAA history;
- GTAO;
- VSM;
- water;
- MSAA;
- swapchain recreation;
- shader hot reload;
- scene reload;
- editor viewport descriptors;
- async compute or transfer synchronization.

Describe the main risks:

- 
- 

## Memory impact

If the change adds persistent buffers, textures, caches, history resources, or other allocations, describe the cost.

```text
RAM impact:
VRAM impact:
Per-frame allocation impact:
```

If none:

```text
No meaningful memory impact.
```

## Breaking changes

Does this PR change any public API, serialized format, shader interface, asset format, or project behavior?

- [ ] No
- [ ] Yes

If yes, describe migration requirements:

```text
Migration:
```

## Additional notes

Add implementation notes, limitations, follow-up work, or context that may help review.

- 
- 

## Final checklist

- [ ] The PR is focused on one logical change
- [ ] Unrelated formatting changes were avoided
- [ ] Comments explain non-obvious decisions rather than restating code
- [ ] No secrets, credentials, tokens, or private data were added
- [ ] New external input is validated where relevant
- [ ] New allocations are bounded where relevant
- [ ] Performance claims are backed by measurements
- [ ] Visual changes include comparison material
- [ ] Documentation was updated when behavior or public APIs changed
- [ ] I have reviewed the final diff
