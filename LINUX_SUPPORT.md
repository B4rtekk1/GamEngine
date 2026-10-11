# Linux support audit

The supported Linux build targets x86-64 with Vulkan 1.4 and a C++23 compiler.
The Windows backend remains available alongside the Linux backend.

## Platform boundaries reviewed

| Area | Linux implementation |
| --- | --- |
| Window, input, file dialogs, process launch | SDL3; native X11/Wayland decorations |
| Asset import | Filesystem copy and Miniz ZIP extraction; Unicode paths and Windows archive separators |
| Runtime filenames | `GamEnginePlayer`, `libEngine.so`, `libGameScripts.so` |
| Script modules | `dlopen`/`dlsym`; absolute load paths; exported C ABI |
| Script hot reload | Versioned module copies; GCC builds disable GNU-unique symbols to isolate module generations |
| Recursive file watching | Inotify, including directory moves, missing/recreated roots and queue overflow recovery |
| Interactive terminal | PTY shell, input/output, resize and child cleanup |
| User directories | Absolute XDG data/config paths and `user-dirs.dirs` for localized Documents |
| Font resources | Bundled Droid Sans from ImGui, including attribution and Apache 2.0 license |
| Runtime library lookup | `$ORIGIN` for development copies, game exports and installed packages |
| Dependencies and shaders | PhysX, Compressonator, ISPC, Slang and FidelityFX Vulkan backend |
| Distribution | Portable editor package, scripting SDK and Linux runtime template |

The Win32 title bar, Windows user paths, file watcher and ConPTY sources are
selected only for their platform. `Installer/Bootstrapper` is a native Windows
installer; Linux distribution uses `PackageEditor` directly.

FidelityFX has local Linux compatibility changes inside its Git submodule.
These changes must be retained when sharing this working tree. The Release
shader compiler additionally required correcting MD5 hex conversion: the
old code passed a 32-byte `snprintf` capacity after advancing inside a 33-byte
buffer. At iteration 1 the debugger observed only 31 bytes remaining. glibc's
fortified Release build aborted. Direct hex encoding removes the invalid
capacity and keeps the same digest format.

## Validation

The Linux Debug engine, Player, editor, shaders and all test executables build.
The `linux-release` preset builds `PackageEditor` successfully, including
FidelityFX shader permutations. Player-only configuration with
`GE_BUILD_EDITOR=OFF` also succeeds.
The 11 new platform/import/export tests pass. The export integration test:

- compiles a generated script class from the SDK in a project path containing
  spaces and Polish characters;
- loads two compiled module generations concurrently and verifies their
  inline static state remains separate;
- verifies runtime libraries, shaders, fonts, licenses and cooked content;
- launches the exported Player with an empty `LD_LIBRARY_PATH` and verifies
  that execution reaches project manifest validation before video initialization.

Installation into a separate directory was checked with `ldd`: the editor
and Player resolve Engine and SDL3 from the package. Bundled `slangc -version`
also succeeds. Script compilation and export were exercised using this
installed SDK rather than a source-tree SDK.
The Release package is available in `out/build/linux-release/GamEngineEditor`.

The complete Debug CTest run has 200 tests: 193 pass and 7 fail. The failing
assertions are outside the platform changes:

- `TerrainComponent.BuildsCheckerboardGridAndSculptsHeightmap`: terrain vertex color expectations.
- `CullingTypes.ObjectAndUniformDataHaveGpuCompatibleAlignment`: expected uniform size 288, actual 304.
- `RenderGraphTests.RebindsFrameLocalResourcesAndUploadWaitsFromACompiledTemplate`: upload wait count.
- `RenderGraphTests.PlansUploadWaitsAtTheConsumerBatch`: upload wait order.
- `Scene.RetainsTheLastPrimaryCameraWhenDestroyed`: camera destruction behavior.
- `PhysXPhysics.SphereTramplesAndGrassGraduallyRecovers`: grass recovery.
- `SceneSerializer.LoadsVersion22AndResavesAsVersion23`: outdated output scene version expectation.

Previously uncompilable tests were updated to the current component, mesh
handle and scene inspection APIs so these failures remain visible.

Window creation and GPU rendering could not be validated in the execution
environment: SDL reports `No available video device`. X11/Wayland dialogs,
resize, Play/Stop and Vulkan validation still need a graphical session.
Windows compilation was not available in this Linux environment.

## Reproduce

```bash
cmake --preset linux-tests
cmake --build --preset linux-tests --parallel 4
ctest --preset linux-tests
cmake --preset linux-release
cmake --build --preset linux-release --target PackageEditor --parallel 4
```

For a focused verification:

```bash
ctest --test-dir out/build/linux-tests --output-on-failure \
  -R 'AssetImporterTest|LinuxPlatformTest|LinuxDynamicLibraryTest|GameBuildPipeline'
```

To verify export from an installed editor, set
`GE_TEST_EDITOR_ROOT_OVERRIDE` to its package directory for the
`GameBuildPipeline` test. CMake and a compatible compiler must be on `PATH`.
