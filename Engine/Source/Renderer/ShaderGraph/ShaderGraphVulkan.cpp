#include "Engine/Renderer/ShaderGraph/ShaderGraphVulkan.h"
#include "Engine/Renderer/ShaderGraph/ShaderGraphSerializer.h"

#include <SDL3/SDL.h>

#include <fstream>
#include <format>
#include <stdexcept>

namespace Engine {
    namespace {
        constexpr std::string_view DeclarationMarker = "// GENERATED_SHADER_GRAPH_DECLARATIONS";

        [[nodiscard]] std::string readText(const std::filesystem::path &path) {
            std::ifstream file(path, std::ios::binary);
            if (!file) throw std::runtime_error("Could not open shader graph template: " + path.string());
            return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        }

        void writeText(const std::filesystem::path &path, const std::string &text) {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file) throw std::runtime_error("Could not write generated shader: " + path.string());
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!file) throw std::runtime_error("Could not finish generated shader: " + path.string());
        }

        [[nodiscard]] std::filesystem::path findSlangCompiler() {
            if (const char* const basePath = SDL_GetBasePath()) {
                const auto bundled = std::filesystem::path{basePath} / "Tools" / "Slang"
#if defined(_WIN32)
                                      / "slangc.exe";
#else
                                      / "slangc";
#endif
                if (std::filesystem::is_regular_file(bundled)) return bundled;
            }

#ifdef GAMEENGINE_SLANGC_PATH
            const std::filesystem::path development{GAMEENGINE_SLANGC_PATH};
            if (std::filesystem::is_regular_file(development)) return development;
#endif

            throw std::runtime_error("Slang compiler is not available. Reinstall the GamEngine Editor.");
        }
    }

    ShaderProgramId ShaderGraphSlangCompiler::makeProgramId(const std::string_view generatedSurface) noexcept {
        // FNV-1a over generated code means a changed literal, topology, or
        // property expression cannot accidentally reuse a stale VkPipeline.
        ShaderProgramId hash = 14695981039346656037ULL;
        const auto mix = [&hash](const std::uint8_t byte) { hash = (hash ^ byte) * 1099511628211ULL; };
        for (const unsigned char character: generatedSurface) mix(character);
        return hash == 0 ? 1 : hash;
    }

    ShaderGraphCompileResult ShaderGraphSlangCompiler::compile(const ShaderGraphAsset &graph,
                                                               const std::filesystem::path &forwardTemplate,
                                                               const std::filesystem::path &generatedDirectory,
                                                               ShaderGraphProgram &program) const {
        ShaderGraphCompileResult result = ShaderGraphCompiler{}.compile(graph);
        if (!result.succeeded()) return result;
        if (result.slang.find("properties.") != std::string::npos) {
            result.diagnostics.push_back({
                "Graph properties need a MaterialParameterBlock GPU layout before they can be emitted to Vulkan.", {}
            });
            return result;
        }

        std::string source;
        try {
            source = readText(forwardTemplate);
            const std::size_t marker = source.find(DeclarationMarker);
            if (marker == std::string::npos) {
                result.diagnostics.push_back({"Forward shader template has no Shader Graph declaration marker.", {}});
                return result;
            }
            const std::string declarations = R"(
#define ENGINE_SHADER_GRAPH 1
struct SurfaceInput
{
    float3 worldPosition;
    float3 worldNormal;
    float3 viewDirection;
    float2 uv0;
    float2 uv1;
    float3 tangent;
    float3 bitangent;
    float time;
};
struct MaterialSurface
{
    float3 baseColor;
    float metallic;
    float roughness;
    float ao;
    float3 normal;
    float3 emission;
    float alpha;
    float alphaCutoff;
};
)" + result.slang;
            source.replace(marker, DeclarationMarker.size(), declarations);
            program.id = makeProgramId(result.slang);
            const std::string stem = std::format("generated_{:016x}", program.id);
            program.slangPath = generatedDirectory / (stem + ".slang");
            program.spirvPath = generatedDirectory / (stem + ".spv");
            program.noVelocitySpirvPath = generatedDirectory / (stem + "_no_velocity.spv");
            writeText(program.slangPath, source);

            // The editor invokes this after debounce on its worker thread; Vulkan
            // sees only the finished SPIR-V via ShaderGraphPipelineCache.
            const std::filesystem::path slangcPath = findSlangCompiler();
            const std::string compiler = slangcPath.string();
            const std::string sourcePath = program.slangPath.string();
            const std::string includeDirectory = forwardTemplate.parent_path().parent_path().string();
            const std::string outputPath = program.spirvPath.string();
            const std::string noVelocityOutputPath = program.noVelocitySpirvPath.string();
            const char *arguments[] = {
                compiler.c_str(), sourcePath.c_str(),
                "-target", "spirv",
                "-profile", "glsl_460",
                "-emit-spirv-directly",
                "-matrix-layout-row-major",
                "-I", includeDirectory.c_str(),
                "-o", outputPath.c_str(),
                nullptr
            };

            const SDL_PropertiesID properties = SDL_CreateProperties();
            if (!properties) {
                throw std::runtime_error(std::string("Could not create process properties: ") + SDL_GetError());
            }

            SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
                                   static_cast<void *>(arguments));
            SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
            SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);

            SDL_Process *process = SDL_CreateProcessWithProperties(properties);
            SDL_DestroyProperties(properties);
            if (!process) {
                throw std::runtime_error(std::string("Could not launch slangc: ") + SDL_GetError());
            }

            size_t outputSize = 0;
            int exitCode = -1;
            void *processOutput = SDL_ReadProcess(process, &outputSize, &exitCode);
            std::string compilerOutput;
            if (processOutput) {
                compilerOutput.assign(static_cast<const char *>(processOutput), outputSize);
                SDL_free(processOutput);
            }
            SDL_DestroyProcess(process);

            if (exitCode != 0) {
                std::string message = "slangc failed while compiling generated Shader Graph module: " +
                                      slangcPath.string();
                if (!compilerOutput.empty()) {
                    message += "\n\n" + compilerOutput;
                } else {
                    message += "\nNo compiler output was captured.";
                }
                result.diagnostics.push_back({std::move(message), {}});
                return result;
            }

            const char *noVelocityArguments[] = {
                compiler.c_str(), sourcePath.c_str(),
                "-target", "spirv",
                "-profile", "glsl_460",
                "-emit-spirv-directly",
                "-matrix-layout-row-major",
                "-I", includeDirectory.c_str(),
                "-D", "FORWARD_OUTPUT_VELOCITY=0",
                "-o", noVelocityOutputPath.c_str(),
                nullptr
            };
            const SDL_PropertiesID noVelocityProperties = SDL_CreateProperties();
            if (!noVelocityProperties) {
                throw std::runtime_error(std::string("Could not create process properties: ") + SDL_GetError());
            }
            SDL_SetPointerProperty(noVelocityProperties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
                                   static_cast<void *>(noVelocityArguments));
            SDL_SetNumberProperty(noVelocityProperties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
            SDL_SetBooleanProperty(noVelocityProperties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
            SDL_Process *noVelocityProcess = SDL_CreateProcessWithProperties(noVelocityProperties);
            SDL_DestroyProperties(noVelocityProperties);
            if (!noVelocityProcess) {
                throw std::runtime_error(std::string("Could not launch slangc: ") + SDL_GetError());
            }
            size_t noVelocityOutputSize = 0;
            int noVelocityExitCode = -1;
            void *noVelocityProcessOutput = SDL_ReadProcess(noVelocityProcess, &noVelocityOutputSize,
                                                            &noVelocityExitCode);
            std::string noVelocityCompilerOutput;
            if (noVelocityProcessOutput) {
                noVelocityCompilerOutput.assign(static_cast<const char *>(noVelocityProcessOutput),
                                                noVelocityOutputSize);
                SDL_free(noVelocityProcessOutput);
            }
            SDL_DestroyProcess(noVelocityProcess);
            if (noVelocityExitCode != 0) {
                std::string message = "slangc failed while compiling color-only generated Shader Graph module: " +
                                      slangcPath.string();
                if (!noVelocityCompilerOutput.empty()) message += "\n\n" + noVelocityCompilerOutput;
                result.diagnostics.push_back({std::move(message), {}});
            }
        } catch (const std::exception &exception) {
            result.diagnostics.push_back({exception.what(), {}});
        }
        return result;
    }

    ShaderGraphCompileResult ShaderGraphMaterialCompiler::resolve(
        Material& material, const std::filesystem::path& assetRoot,
        const std::filesystem::path& forwardTemplate,
        const std::filesystem::path& generatedDirectory) const {
        ShaderGraphCompileResult result;
        const std::filesystem::path asset = material.shaderGraphAsset.lexically_normal();
        if (asset.empty()) {
            result.diagnostics.push_back({"Shader Graph material has no source asset.", {}});
            return result;
        }
        if (asset.is_absolute() || *asset.begin() == ".." || asset.extension() != ".shadergraph") {
            result.diagnostics.push_back({"Shader Graph asset must be a relative .shadergraph path inside Assets.", {}});
            return result;
        }

        try {
            const ShaderGraphAsset graph = ShaderGraphSerializer::load(assetRoot / asset);
            ShaderGraphProgram program;
            result = ShaderGraphSlangCompiler{}.compile(graph, forwardTemplate, generatedDirectory, program);
            if (result.succeeded()) {
                material.shaderGraphAsset = asset;
                material.shaderProgram = program.id;
                material.shaderProgramSpirv = program.spirvPath;
            }
        } catch (const std::exception& exception) {
            result.diagnostics.push_back({exception.what(), {}});
        }
        return result;
    }

    void ShaderGraphPipelineCache::initialize(const VkDevice device, GraphicsPipelineOptions baseOptions) {
        if (device == VK_NULL_HANDLE) throw std::invalid_argument(
            "Shader Graph pipeline cache requires a Vulkan device");
        if (device_ != VK_NULL_HANDLE) throw std::logic_error(
            "Shader Graph pipeline cache has already been initialized");
        device_ = device;
        // Generated files are not AssetManager assets; load their just-cooked
        // SPIR-V directly instead of passing through the timestamp cache.
        baseOptions.assetManager = nullptr;
        baseOptions_ = std::move(baseOptions);
        for (auto& [program, entry] : entries_) {
            (void) program;
            createPipeline(entry);
        }
    }

    void ShaderGraphPipelineCache::destroy() noexcept {
        // ForwardPass is also torn down for swapchain/MSAA recreation while
        // the scene's GPU batches retain their shader slots. Keep the slot
        // registry so initialize() can recreate equivalent VkPipelines for
        // those batches; only device-owned resources must be released here.
        for (auto& [program, entry] : entries_) {
            (void) program;
            entry.pipeline.reset();
        }
        device_ = VK_NULL_HANDLE;
        baseOptions_ = {};
    }

    void ShaderGraphPipelineCache::createPipeline(Entry& entry) {
        GraphicsPipelineOptions options = baseOptions_;
        options.shader = entry.spirv;
        options.cullMode = entry.state.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        options.depthWriteEnable = entry.state.depthWrite ? VK_TRUE : VK_FALSE;
        options.alphaBlendEnable = entry.state.transparent ? VK_TRUE : VK_FALSE;
        entry.pipeline = std::make_unique<GraphicsPipeline>();
        entry.pipeline->create(device_, options);
    }

    std::uint32_t ShaderGraphPipelineCache::getOrCreate(const ShaderProgramId program,
                                                        const std::filesystem::path &spirv,
                                                        const MaterialRenderState &state) {
        if (program == 0 || spirv.empty()) throw std::invalid_argument(
            "Shader Graph program requires an ID and SPIR-V path");
        if (const auto existing = entries_.find(program); existing != entries_.end()) {
            if (existing->second.spirv != spirv || existing->second.state.doubleSided != state.doubleSided ||
                existing->second.state.depthWrite != state.depthWrite || existing->second.state.transparent != state.
                transparent) {
                throw std::logic_error(
                    "A ShaderProgramId may not be reused with a different SPIR-V module or render state");
            }
            if (device_ != VK_NULL_HANDLE && !existing->second.pipeline) createPipeline(existing->second);
            return existing->second.slot;
        }
        if (entries_.size() >= MaterialProgramSlotCount - MaterialShaderCount)
            throw std::runtime_error("Shader Graph pipeline slot capacity exceeded");
        const std::uint32_t slot = static_cast<std::uint32_t>(MaterialShaderCount + entries_.size());
        Entry entry{spirv, state, slot, nullptr};
        if (device_ != VK_NULL_HANDLE) createPipeline(entry);
        const auto [it, inserted] = entries_.emplace(program, std::move(entry));
        (void) inserted;
        return it->second.slot;
    }

    const GraphicsPipeline *ShaderGraphPipelineCache::find(const std::uint32_t slot) const noexcept {
        for (const auto &[id, entry]: entries_) {
            (void) id;
            if (entry.slot == slot) return entry.pipeline.get();
        }
        return nullptr;
    }
} // namespace Engine
