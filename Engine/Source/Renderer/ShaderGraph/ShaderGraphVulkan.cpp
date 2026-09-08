#include "Engine/Renderer/ShaderGraph/ShaderGraphVulkan.h"
#include "Engine/Renderer/ShaderGraph/ShaderGraphSerializer.h"

#include <cstdlib>
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

        [[nodiscard]] std::string quote(const std::filesystem::path &path) {
            const std::string value = path.string();
            if (value.find('"') != std::string::npos) throw std::invalid_argument(
                "Shader paths may not contain quotation marks");
            return '"' + value + '"';
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
            writeText(program.slangPath, source);

            // The editor invokes this after debounce on its worker thread; Vulkan
            // sees only the finished SPIR-V via ShaderGraphPipelineCache.
            const std::filesystem::path slangcPath{GAMEENGINE_SLANGC_PATH};
            const std::string command = quote(slangcPath) + " " + quote(program.slangPath) +
                                        " -target spirv -profile glsl_460 -emit-spirv-directly -matrix-layout-row-major -I "
                                        +
                                        quote(forwardTemplate.parent_path().parent_path()) + " -o " + quote(
                                            program.spirvPath);
            if (std::system(command.c_str()) != 0) {
                result.diagnostics.push_back({"slangc failed while compiling generated Shader Graph module: " +
                                              slangcPath.string(), {}});
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
        destroy();
        if (device == VK_NULL_HANDLE) throw std::invalid_argument(
            "Shader Graph pipeline cache requires a Vulkan device");
        device_ = device;
        // Generated files are not AssetManager assets; load their just-cooked
        // SPIR-V directly instead of passing through the timestamp cache.
        baseOptions.assetManager = nullptr;
        baseOptions_ = std::move(baseOptions);
    }

    void ShaderGraphPipelineCache::destroy() noexcept {
        entries_.clear();
        device_ = VK_NULL_HANDLE;
        baseOptions_ = {};
    }

    std::uint32_t ShaderGraphPipelineCache::getOrCreate(const ShaderProgramId program,
                                                        const std::filesystem::path &spirv,
                                                        const MaterialRenderState &state) {
        if (device_ == VK_NULL_HANDLE) throw std::logic_error("Shader Graph pipeline cache has not been initialized");
        if (program == 0 || spirv.empty()) throw std::invalid_argument(
            "Shader Graph program requires an ID and SPIR-V path");
        if (const auto existing = entries_.find(program); existing != entries_.end()) {
            if (existing->second.spirv != spirv || existing->second.state.doubleSided != state.doubleSided ||
                existing->second.state.depthWrite != state.depthWrite || existing->second.state.transparent != state.
                transparent) {
                throw std::logic_error(
                    "A ShaderProgramId may not be reused with a different SPIR-V module or render state");
            }
            return existing->second.slot;
        }
        if (entries_.size() >= MaterialProgramSlotCount - MaterialShaderCount)
            throw std::runtime_error("Shader Graph pipeline slot capacity exceeded");
        GraphicsPipelineOptions options = baseOptions_;
        options.shader = spirv;
        options.cullMode = state.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        options.depthWriteEnable = state.depthWrite ? VK_TRUE : VK_FALSE;
        options.alphaBlendEnable = state.transparent ? VK_TRUE : VK_FALSE;
        auto pipeline = std::make_unique<GraphicsPipeline>();
        pipeline->create(device_, options);
        const std::uint32_t slot = static_cast<std::uint32_t>(MaterialShaderCount + entries_.size());
        const auto [it, inserted] = entries_.emplace(program, Entry{spirv, state, slot, std::move(pipeline)});
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
