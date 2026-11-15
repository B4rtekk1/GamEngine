#include "Engine/Renderer/Passes/ForwardPass.h"

#include "Engine/Core/Diagnostics.h"
#include "Engine/Renderer/Culling/IndexedIndirectDrawCount.h"
#include "Engine/Renderer/Geometry/GpuVertex.h"
#include "Engine/Renderer/Vulkan/renderer_types.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace Engine {
void ForwardPass::create(VkDevice device, const VkFormat colorFormat, const VkFormat depthFormat,
                         const VkSampleCountFlagBits samples, const VkFormat depthResolveFormat,
                         const VkResolveModeFlagBits depthResolveMode, VkDescriptorSetLayout sceneLayout,
                         Assets::AssetManager& assets, const VkImageLayout colorInitialLayout,
                         const bool colorInitialLayoutExternallySynchronized, const VkFormat velocityFormat,
                         const VkFormat viewNormalFormat, const bool preserveDepth, const bool depthOnly,
                         const bool enableMeshletPipelines, const VkFormat localLightDataFormat,
                         const VkDescriptorSetLayout meshletDrawLayout) {
    reportedMissingShaderGraphSlots_.clear();
    const bool meshShaderPipelines = enableMeshletPipelines && meshletDrawLayout != VK_NULL_HANDLE;
    drawMeshTasksIndirect_ =
        meshShaderPipelines ? reinterpret_cast<PFN_vkCmdDrawMeshTasksIndirectEXT>(
                                  vkGetDeviceProcAddr(device, "vkCmdDrawMeshTasksIndirectEXT"))
                            : nullptr;
    meshShaderDrawFlags_.fill(0U);
    // Sub-pixel rejection tests pixel centres, which only matches
    // single-sample rasterization.
    meshShaderSmallPrimitiveFlag_ = samples == VK_SAMPLE_COUNT_1_BIT ? 2U : 0U;
    // Visibility-buffer prepass: colour target 0 holds triangle ids, which
    // every mesh-shader program writes (VisibilityBufferPass).
    visibilityIds_ = depthOnly && colorFormat == VK_FORMAT_R32_UINT && meshShaderPipelines;
    // Mesh-shader twin of a meshlet pipeline: same fragment program, the
    // vertex stage replaced by meshMain reading MeshletDraw.slang's set 1.
    const bool visibilityIds = visibilityIds_;
    const auto meshShaderOptions = [meshletDrawLayout, visibilityIds](GraphicsPipelineOptions pipelineOptions,
                                                                      std::filesystem::path shader) {
        if (visibilityIds) {
            // depth_velocity[_flat]_meshshader -> depth_velocity_vis[_flat]_meshshader
            std::string name = shader.filename().string();
            name.insert(std::string{"depth_velocity"}.size(), "_vis");
            shader.replace_filename(name);
            pipelineOptions.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
        }
        pipelineOptions.shader = shader;
        pipelineOptions.meshShader = true;
        pipelineOptions.vertexBindings.clear();
        pipelineOptions.vertexAttributes.clear();
        pipelineOptions.descriptorSetLayouts.push_back(meshletDrawLayout);
        pipelineOptions.pushConstantSize = sizeof(std::uint32_t) * 2;
        pipelineOptions.pushConstantStages = VK_SHADER_STAGE_MESH_BIT_EXT;
        // meshMain culls back faces per primitive (meshShaderDrawFlags_), so
        // a slot's draw can mix one-sided and two-sided meshlets.
        pipelineOptions.cullMode = VK_CULL_MODE_NONE;
        return pipelineOptions;
    };
    hasVelocityAttachment_ = velocityFormat != VK_FORMAT_UNDEFINED;
    hasViewNormalAttachment_ = viewNormalFormat != VK_FORMAT_UNDEFINED;
    // The data target takes the second slot, which the lighting pass leaves
    // free (velocity is final after the prepass).
    hasLocalLightDataAttachment_ = localLightDataFormat != VK_FORMAT_UNDEFINED && !hasVelocityAttachment_;
    preserveDepth_ = preserveDepth;
    GraphicsPipelineOptions options{};
    options.colorFormat = colorFormat;
    options.dynamicRendering = true;
    options.additionalColorFormat = hasLocalLightDataAttachment_ ? localLightDataFormat : velocityFormat;
    // The `_no_velocity_resolved` programs (FORWARD_DEFERRED_LOCAL) write the
    // local light data at location 1. A resolved lighting pass without that
    // target (Scene View) selects their colour-only `_nolocal` twins instead.
    // Only the indexed programs have such twins.
    const bool colorOnlyResolved = !depthOnly && preserveDepth && samples == VK_SAMPLE_COUNT_1_BIT &&
                                   !hasVelocityAttachment_ && !hasLocalLightDataAttachment_ &&
                                   !enableMeshletPipelines;
    const auto resolvedShader = [colorOnlyResolved](const std::filesystem::path& path) {
        return path.parent_path() /
               (path.stem().string() + (colorOnlyResolved ? "_resolved_nolocal" : "_resolved") +
                path.extension().string());
    };
    if (hasLocalLightDataAttachment_)
        options.additionalColorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    options.thirdColorFormat = viewNormalFormat;
    options.depthFormat = depthFormat;
    options.samples = samples;
    options.depthResolveFormat = depthResolveFormat;
    options.depthResolveMode = depthResolveMode;
    options.colorInitialLayout = colorInitialLayout;
    options.colorInitialLayoutExternallySynchronized = colorInitialLayoutExternallySynchronized;
    options.colorFinalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (depthOnly) {
        // The depth prepass deliberately leaves HDR untouched, but it must
        // still emit the motion vectors used by temporal GTAO/TAA.
        options.colorWriteMask = 0;
        options.additionalColorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
        options.thirdColorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    }
    if (preserveDepth) {
        options.depthLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        options.depthInitialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        options.depthFinalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        options.depthWriteEnable = VK_FALSE;
        // EQUAL needs bit-identical depth from two different shader modules.
        // slangc's direct SPIR-V path drops both `precise` and the Invariant
        // decoration, so the driver may round the forward position a few ULPs
        // differently; those pixels failed the test and left holes along
        // triangle edges. Pull the lighting depth a few steps towards the
        // camera and accept anything at or in front of the prepass depth.
        options.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        options.depthBiasConstantFactor = -4.0F;
    }
    options.shader = depthOnly ? (hasVelocityAttachment_ ? "shaders/depth_velocity.spv"
                                                          : "shaders/depth_velocity_no_velocity.spv")
                               : (hasVelocityAttachment_ ? "shaders/forward_pbr.spv"
                                                        : "shaders/forward_pbr_no_velocity.spv");
    options.assetManager = &assets;
    options.cullMode = VK_CULL_MODE_BACK_BIT;
    options.alphaBlendEnable = VK_FALSE;
    options.descriptorSetLayouts = {sceneLayout};
    options.vertexBindings = {
        {0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX},
    };
    options.vertexAttributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, px)},
        {1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(GpuVertex, color)},
        {2, 0, VK_FORMAT_R16G16_SFLOAT, offsetof(GpuVertex, texCoord)},
        {4, 0, VK_FORMAT_R16G16_SFLOAT, offsetof(GpuVertex, texCoord1)},
        {3, 0, VK_FORMAT_R32_UINT, offsetof(GpuVertex, tangentFrame)},
        {8, 0, VK_FORMAT_R32_UINT, offsetof(GpuVertex, materialIndex)},
    };
    // depth_velocity consumes position, UV0/UV1, tangent frame and material
    // index only. Supplying color causes a Vulkan validation warning at
    // location 1. Shader Graph and grass programs keep the full layout.
    const auto stripDepthOnlyAttributes = [depthOnly](GraphicsPipelineOptions& pipelineOptions) {
        if (!depthOnly)
            return;
        std::erase_if(pipelineOptions.vertexAttributes, [](const VkVertexInputAttributeDescription& attribute) {
            return attribute.location == 1;
        });
    };
    std::array<std::filesystem::path, MaterialShaderCount> shaderPaths{};
    if (depthOnly) {
        shaderPaths.fill(options.shader);
    } else if (hasVelocityAttachment_) {
        shaderPaths = {"shaders/forward_pbr.spv",         "shaders/forward_unlit.spv",
                       "shaders/forward_hologram.spv",    "shaders/forward_water.spv",
                       "shaders/forward_pbr_normal.spv",  "shaders/forward_pbr_extended.spv",
                       "shaders/forward_pbr_foliage.spv", "shaders/forward_pbr_terrain.spv"};
    } else {
        shaderPaths = {"shaders/forward_pbr_no_velocity.spv",         "shaders/forward_unlit_no_velocity.spv",
                       "shaders/forward_hologram_no_velocity.spv",    "shaders/forward_water_no_velocity.spv",
                       "shaders/forward_pbr_normal_no_velocity.spv",  "shaders/forward_pbr_extended_no_velocity.spv",
                       "shaders/forward_pbr_foliage_no_velocity.spv", "shaders/forward_pbr_terrain_no_velocity.spv"};
    }
    // Reduced variants (useReducedShaders): the prepass drops displacement,
    // the resolved lighting programs drop the DDGI probe lookup.
    const bool resolvedLighting = !depthOnly && preserveDepth && samples == VK_SAMPLE_COUNT_1_BIT;
    const char* const reducedSuffix = depthOnly ? "_flat" : "_noddgi";
    const auto reducedShader = [reducedSuffix](const std::filesystem::path& path, const char* tail = "") {
        return path.parent_path() / (path.stem().string() + reducedSuffix + tail + path.extension().string());
    };
    for (std::size_t index = 0; index < shaderPaths.size(); ++index) {
        // Water and glass have their own passes and a second descriptor set
        // for the immutable opaque scene.  Building a forward-compatible pipeline
        // here produces a validation warning (velocity target mismatch) and
        // can never be selected by a draw loop.
        if (isCompositedProgramSlot(index)) {
            continue;
        }
        GraphicsPipelineOptions materialOptions = options;
        materialOptions.shader = shaderPaths[index];
        // Only the resolved PBR programs write the deferred local light
        // inputs; the rest light themselves and leave the cleared texel.
        if (hasLocalLightDataAttachment_ &&
            !(index == materialShaderIndex(MaterialShader::StandardPBR) || index >= PbrNormalProgramSlot))
            materialOptions.additionalColorWriteMask = 0;
        stripDepthOnlyAttributes(materialOptions);
        if (preserveDepth && samples == VK_SAMPLE_COUNT_1_BIT &&
            (index == materialShaderIndex(MaterialShader::StandardPBR) || index >= PbrNormalProgramSlot)) {
            materialOptions.shader = resolvedShader(materialOptions.shader);
        }
        if (index == PbrFoliageProgramSlot)
            materialOptions.cullMode = VK_CULL_MODE_NONE;
        materialPipelines_[index].create(device, materialOptions);
        GraphicsPipelineOptions twoSidedOptions = materialOptions;
        twoSidedOptions.cullMode = VK_CULL_MODE_NONE;
        twoSidedMaterialPipelines_[index].create(device, twoSidedOptions);
        if (enableMeshletPipelines) {
            GraphicsPipelineOptions meshletOptions = materialOptions;
            const auto& path = materialOptions.shader;
            meshletOptions.shader =
                path.parent_path() / (path.stem().string() + "_meshlet" + path.extension().string());
            meshletMaterialPipelines_[index].create(device, meshletOptions);
        }
        if (drawMeshTasksIndirect_ != nullptr) {
            const auto& path = materialOptions.shader;
            meshShaderMaterialPipelines_[index].create(
                device, meshShaderOptions(materialOptions, path.parent_path() / (path.stem().string() + "_meshshader" +
                                                                                 path.extension().string())));
            if (depthOnly)
                opaqueMeshShaderMaterialPipelines_[index].create(
                    device, meshShaderOptions(materialOptions,
                                              path.parent_path() / (path.stem().string() + "_meshshader_opaque" +
                                                                    path.extension().string())));
            meshShaderDrawFlags_[index] =
                (materialOptions.cullMode == VK_CULL_MODE_BACK_BIT ? 1U : 0U) | meshShaderSmallPrimitiveFlag_;
            // MeshletDrawVisibility: the slots the visibility resolve shades.
            if (visibilityIds_ && isVisibilityResolvedSlot(index))
                meshShaderDrawFlags_[index] |= 4U;
            if (!depthOnly)
                flatMeshShaderMaterialPipelines_[index].create(
                    device, meshShaderOptions(materialOptions,
                                              path.parent_path() / (path.stem().string() + "_flat_meshshader" +
                                                                    path.extension().string())));
        }
        const bool resolvedProgram =
            index == materialShaderIndex(MaterialShader::StandardPBR) || index >= PbrNormalProgramSlot;
        if (depthOnly || (resolvedLighting && resolvedProgram)) {
            GraphicsPipelineOptions reducedOptions = materialOptions;
            reducedOptions.shader = reducedShader(materialOptions.shader);
            reducedMaterialPipelines_[index].create(device, reducedOptions);
            reducedOptions.cullMode = VK_CULL_MODE_NONE;
            reducedTwoSidedMaterialPipelines_[index].create(device, reducedOptions);
            if (enableMeshletPipelines) {
                GraphicsPipelineOptions reducedMeshletOptions = materialOptions;
                reducedMeshletOptions.shader = reducedShader(materialOptions.shader, "_meshlet");
                reducedMeshletMaterialPipelines_[index].create(device, reducedMeshletOptions);
            }
            if (drawMeshTasksIndirect_ != nullptr) {
                reducedMeshShaderMaterialPipelines_[index].create(
                    device, meshShaderOptions(materialOptions, reducedShader(materialOptions.shader, "_meshshader")));
                if (depthOnly)
                    opaqueReducedMeshShaderMaterialPipelines_[index].create(
                        device, meshShaderOptions(materialOptions,
                                                  reducedShader(materialOptions.shader, "_meshshader_opaque")));
                if (!depthOnly)
                    flatReducedMeshShaderMaterialPipelines_[index].create(
                        device,
                        meshShaderOptions(materialOptions, reducedShader(materialOptions.shader, "_flat_meshshader")));
            }
        }
        // Resolved PBR programs of the lighting pass without the local light
        // walk, for frames whose local lights the deferred compute pass adds.
        if (drawMeshTasksIndirect_ != nullptr && resolvedLighting && resolvedProgram && hasLocalLightDataAttachment_) {
            const auto& path = materialOptions.shader;
            const std::string stem = path.stem().string();
            constexpr std::array<const char*, 4> tails{"_deferred_meshshader", "_noddgi_deferred_meshshader",
                                                       "_deferred_flat_meshshader",
                                                       "_noddgi_deferred_flat_meshshader"};
            for (std::size_t set = 0; set < tails.size(); ++set)
                deferredMeshShaderMaterialPipelines_[set][index].create(
                    device, meshShaderOptions(materialOptions,
                                              path.parent_path() / (stem + tails[set] + path.extension().string())));
        }
    }
    shaderGraphPipelineOptions_ = options;
    shaderGraphPipelineOptions_.shader.clear();
    if (hasLocalLightDataAttachment_)
        shaderGraphPipelineOptions_.additionalColorWriteMask = 0;
    shaderGraphPipelines_.initialize(device, shaderGraphPipelineOptions_,
                                     drawMeshTasksIndirect_ != nullptr ? meshletDrawLayout : VK_NULL_HANDLE);

    GraphicsPipelineOptions foliageOptions = options;
    foliageOptions.shader = depthOnly                ? options.shader
                            : hasVelocityAttachment_ ? "shaders/forward_pbr.spv"
                                                     : "shaders/forward_pbr_no_velocity.spv";
    if (preserveDepth && samples == VK_SAMPLE_COUNT_1_BIT)
        foliageOptions.shader = resolvedShader(foliageOptions.shader);
    foliageOptions.cullMode = VK_CULL_MODE_NONE;
    // Vegetation cards use alpha cutout.  They must populate depth before the
    // sky draw and TAA resolve; treating them as a generic transparent stream
    // leaves their pixels unoccluding and causes background bleed/shimmer.
    // Genuine, sorted transparency belongs in a separate pipeline.
    foliageOptions.alphaBlendEnable = VK_FALSE;
    // Preserve the base pass's depth mode: lighting reads the depth prepass,
    // while the regular forward pass writes depth.
    GraphicsPipelineOptions depthFoliageOptions = foliageOptions;
    stripDepthOnlyAttributes(depthFoliageOptions);
    foliagePipeline_.create(device, depthFoliageOptions);
    if (depthOnly || resolvedLighting) {
        GraphicsPipelineOptions reducedFoliageOptions = depthFoliageOptions;
        reducedFoliageOptions.shader = reducedShader(depthFoliageOptions.shader);
        reducedFoliagePipeline_.create(device, reducedFoliageOptions);
    }
    GraphicsPipelineOptions grassOptions = foliageOptions;
    grassOptions.shader =
        hasVelocityAttachment_ ? "shaders/grass_forward.spv" : "shaders/grass_forward_no_velocity.spv";
    // Grass consumes only position, color, UV0, normal, and material index.
    // Supplying UV1/tangents causes Vulkan validation warnings at locations 4/9.
    std::erase_if(grassOptions.vertexAttributes, [](const VkVertexInputAttributeDescription& attribute) {
        return attribute.location == 4 || attribute.location == 9;
    });
    if (hasLocalLightDataAttachment_)
        grassOptions.additionalColorWriteMask = 0;
    grassPipeline_.create(device, grassOptions);

}

void ForwardPass::destroy() noexcept {
    reportedMissingShaderGraphSlots_.clear();
    hasVelocityAttachment_ = false;
    hasViewNormalAttachment_ = false;
    hasLocalLightDataAttachment_ = false;
    preserveDepth_ = false;
    shaderGraphPipelines_.destroy();
    foliagePipeline_.destroy();
    reducedFoliagePipeline_.destroy();
    grassPipeline_.destroy();
    useReduced_ = false;
    useFlatMeshShaders_ = false;
    useDeferredLocal_ = false;
    visibilityIds_ = false;
    for (auto& pipelines : deferredMeshShaderMaterialPipelines_)
        for (auto& pipeline : pipelines)
            pipeline.destroy();
    for (auto& pipeline : materialPipelines_)
        pipeline.destroy();
    for (auto& pipeline : twoSidedMaterialPipelines_)
        pipeline.destroy();
    for (auto& pipeline : meshletMaterialPipelines_)
        pipeline.destroy();
    for (auto& pipelines :
         {&reducedMaterialPipelines_, &reducedTwoSidedMaterialPipelines_, &reducedMeshletMaterialPipelines_,
          &meshShaderMaterialPipelines_, &reducedMeshShaderMaterialPipelines_, &flatMeshShaderMaterialPipelines_,
          &flatReducedMeshShaderMaterialPipelines_, &opaqueMeshShaderMaterialPipelines_,
          &opaqueReducedMeshShaderMaterialPipelines_})
        for (auto& pipeline : *pipelines)
            pipeline.destroy();
    drawMeshTasksIndirect_ = nullptr;
}

std::uint32_t ForwardPass::registerShaderGraph(const ShaderGraphProgram& program, const MaterialRenderState& state) {
    std::filesystem::path shader = program.spirvPath;
    if (!hasVelocityAttachment_) {
        shader = program.noVelocitySpirvPath;
        if (shader.empty()) {
            shader = program.spirvPath.parent_path() /
                     (program.spirvPath.stem().string() + "_no_velocity" + program.spirvPath.extension().string());
        }
    }
    return shaderGraphPipelines_.getOrCreate(program.id, shader, state);
}

bool ForwardPass::hasMeshShaderPipeline(const std::uint32_t shaderSlot) const noexcept {
    if (drawMeshTasksIndirect_ == nullptr)
        return false;
    if (shaderSlot < MaterialShaderCount)
        return meshShaderMaterialPipelines_[shaderSlot].handle() != VK_NULL_HANDLE;
    return shaderGraphPipelines_.findMesh(shaderSlot) != nullptr;
}

bool ForwardPass::hasMaterialPipeline(const std::uint32_t shaderSlot) const noexcept {
    return shaderSlot < MaterialShaderCount || shaderGraphPipelines_.find(shaderSlot) != nullptr;
}

void ForwardPass::drawShaderGraph(VkCommandBuffer commandBuffer, VkDescriptorSet sceneDescriptorSet,
                                  const std::uint32_t shaderSlot, const Culling::IndexedIndirectDrawCount& indirectDraw,
                                  const VkDeviceSize commandOffset, const VkDeviceSize countOffset) const {
    if (!indirectDraw.valid())
        return;
    const GraphicsPipeline* const pipeline = shaderGraphPipelines_.find(shaderSlot);
    if (pipeline == nullptr) {
        if (reportedMissingShaderGraphSlots_.insert(shaderSlot).second) {
            Diagnostics::instance().report(DiagnosticSeverity::Error,
                                           "[Renderer] Shader Graph pipeline missing for slot " +
                                               std::to_string(shaderSlot) + "; falling back to Standard PBR.");
        }
        const auto& fallback = materialPipelines_[materialShaderIndex(MaterialShader::StandardPBR)];
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, fallback.handle());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, fallback.layout(), 0, 1,
                                &sceneDescriptorSet, 0, nullptr);
        indirectDraw.record(commandBuffer, commandOffset, countOffset);
        return;
    }
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout(), 0, 1,
                            &sceneDescriptorSet, 0, nullptr);
    indirectDraw.record(commandBuffer, commandOffset, countOffset);
}

void ForwardPass::drawGrass(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                            const Culling::IndexedIndirectDrawCount& indirectDraw) const {
    if (!indirectDraw.valid())
        return;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, grassPipeline_.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, grassPipeline_.layout(), 0, 1,
                            &descriptorSet, 0, nullptr);
    indirectDraw.record(commandBuffer);
}

void ForwardPass::begin(VkCommandBuffer commandBuffer, const VkImageView colorView, const VkImageView depthView,
                        const VkImageView velocityView, const VkImageView viewNormalView,
                        const VkImageView colorResolveView, const VkImageView depthResolveView, const VkExtent2D extent,
                        VkDescriptorSet sceneDescriptorSet, VkBuffer vertexBuffer, VkBuffer instanceBuffer,
                        VkBuffer indexBuffer, const VkImageView localLightDataView) const {
    (void)instanceBuffer; // Instance data is fetched from descriptor binding 5.
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = colorView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.02F, 0.02F, 0.05F, 1.0F}};
    if (visibilityIds_)
        color.clearValue.color.uint32[0] = 0xffffffffU; // MeshletDraw.slang VisibilityNone
    if (colorResolveView != VK_NULL_HANDLE) {
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = colorResolveView;
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingAttachmentInfo velocity{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    velocity.imageView = velocityView;
    velocity.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    velocity.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    velocity.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    velocity.clearValue.color = {{0.0F, 0.0F, 0.0F, 0.0F}};
    VkRenderingAttachmentInfo normals = velocity;
    normals.imageView = viewNormalView;
    if (hasLocalLightDataAttachment_) {
        // Cleared to "no surface": texels nothing writes add no local light.
        velocity.imageView = localLightDataView;
        velocity.clearValue.color.uint32[0] = velocity.clearValue.color.uint32[1] = 0;
        velocity.clearValue.color.uint32[2] = velocity.clearValue.color.uint32[3] = 0;
    }
    std::array colors{color, velocity, normals};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = depthView;
    depth.imageLayout = preserveDepth_ ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                       : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = preserveDepth_ ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0F, 0};
    if (depthResolveView != VK_NULL_HANDLE) {
        depth.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        depth.resolveImageView = depthResolveView;
        depth.resolveImageLayout = depth.imageLayout;
    }
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1U + (hasVelocityAttachment_ || hasLocalLightDataAttachment_ ? 1U : 0U) +
                                     (hasViewNormalAttachment_ ? 1U : 0U);
    rendering.pColorAttachments = colors.data();
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(commandBuffer, &rendering);

    const auto& pipeline = materialPipelines_[0];
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0, 1,
                            &sceneDescriptorSet, 0, nullptr);
    constexpr VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, offsets);
    vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    const VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent.width), static_cast<float>(extent.height),
                              0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

void ForwardPass::draw(VkCommandBuffer commandBuffer, const Culling::IndexedIndirectDrawCount& indirectDraw) {
    if (indirectDraw.valid()) {
        indirectDraw.record(commandBuffer);
    }
}

void ForwardPass::drawMaterial(VkCommandBuffer commandBuffer, VkDescriptorSet sceneDescriptorSet,
                               const std::uint32_t shaderSlot, const Culling::IndexedIndirectDrawCount& indirectDraw,
                               const VkDeviceSize commandOffset, const VkDeviceSize countOffset,
                               const bool twoSided) const {
    if (!indirectDraw.valid())
        return;
    if (shaderSlot >= MaterialShaderCount) {
        drawShaderGraph(commandBuffer, sceneDescriptorSet, shaderSlot, indirectDraw, commandOffset, countOffset);
        return;
    }
    const bool reduced = useReduced_ && reducedMaterialPipelines_[shaderSlot].handle() != VK_NULL_HANDLE;
    const auto& pipeline = twoSided
                               ? (reduced ? reducedTwoSidedMaterialPipelines_ : twoSidedMaterialPipelines_)[shaderSlot]
                               : (reduced ? reducedMaterialPipelines_ : materialPipelines_)[shaderSlot];
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0, 1,
                            &sceneDescriptorSet, 0, nullptr);
    indirectDraw.record(commandBuffer, commandOffset, countOffset);
}

void ForwardPass::drawMeshletMaterial(VkCommandBuffer commandBuffer, VkDescriptorSet sceneDescriptorSet,
                                      const std::uint32_t shaderSlot,
                                      const Culling::IndexedIndirectDrawCount& indirectDraw,
                                      const VkDeviceSize commandOffset, const VkDeviceSize countOffset) const {
    if (shaderSlot >= MaterialShaderCount || !indirectDraw.valid())
        return;
    const bool reduced = useReduced_ && reducedMeshletMaterialPipelines_[shaderSlot].handle() != VK_NULL_HANDLE;
    const auto& pipeline = (reduced ? reducedMeshletMaterialPipelines_ : meshletMaterialPipelines_)[shaderSlot];
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0, 1,
                            &sceneDescriptorSet, 0, nullptr);
    indirectDraw.record(commandBuffer, commandOffset, countOffset);
}

void ForwardPass::drawMeshShaderMaterial(VkCommandBuffer commandBuffer, VkDescriptorSet sceneDescriptorSet,
                                         VkDescriptorSet meshletDrawSet, const std::uint32_t shaderSlot,
                                         const VkBuffer commands) const {
    if (drawMeshTasksIndirect_ == nullptr || commands == VK_NULL_HANDLE)
        return;
    const GraphicsPipeline* selected = nullptr;
    const GraphicsPipeline* opaqueSelected = nullptr;
    std::uint32_t flags = 0;
    if (shaderSlot < MaterialShaderCount) {
        const bool reduced =
            useReduced_ && reducedMeshShaderMaterialPipelines_[shaderSlot].handle() != VK_NULL_HANDLE;
        selected = &(reduced ? reducedMeshShaderMaterialPipelines_ : meshShaderMaterialPipelines_)[shaderSlot];
        opaqueSelected =
            &(reduced ? opaqueReducedMeshShaderMaterialPipelines_ : opaqueMeshShaderMaterialPipelines_)[shaderSlot];
        bool flatSelected = false;
        if (useFlatMeshShaders_) {
            const GraphicsPipeline& flat =
                (reduced ? flatReducedMeshShaderMaterialPipelines_ : flatMeshShaderMaterialPipelines_)[shaderSlot];
            if (flat.handle() != VK_NULL_HANDLE) {
                selected = &flat;
                flatSelected = true;
            }
        }
        if (useDeferredLocal_) {
            const GraphicsPipeline& deferred =
                deferredMeshShaderMaterialPipelines_[(reduced ? 1U : 0U) | (flatSelected ? 2U : 0U)][shaderSlot];
            if (deferred.handle() != VK_NULL_HANDLE)
                selected = &deferred;
        }
        flags = meshShaderDrawFlags_[shaderSlot];
    } else {
        bool doubleSided = false;
        selected = shaderGraphPipelines_.findMesh(shaderSlot, &doubleSided);
        flags = (doubleSided ? 0U : 1U) | meshShaderSmallPrimitiveFlag_;
    }
    if (selected == nullptr || selected->handle() == VK_NULL_HANDLE)
        return;
    // meshlet_build_mesh_commands.slang writes three commands per slot: all
    // entries, the one-sided ones and the two-sided ones.
    const auto drawWith = [&](const GraphicsPipeline& pipeline, const std::uint32_t drawFlags,
                              const std::uint32_t commandSet) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
        const VkDescriptorSet sets[]{sceneDescriptorSet, meshletDrawSet};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0, 2, sets, 0,
                                nullptr);
        // MeshletDrawConstants in MeshletDraw.slang.
        const std::uint32_t constants[]{shaderSlot, drawFlags};
        vkCmdPushConstants(commandBuffer, pipeline.layout(), VK_SHADER_STAGE_MESH_BIT_EXT, 0, sizeof(constants),
                           constants);
        drawMeshTasksIndirect_(commandBuffer, commands,
                               static_cast<VkDeviceSize>(commandSet * MaterialProgramSlotCount + shaderSlot) *
                                   sizeof(VkDrawMeshTasksIndirectCommandEXT),
                               1, sizeof(VkDrawMeshTasksIndirectCommandEXT));
    };
    // Depth prepass: any `discard` in a fragment program moves all of its
    // fragments to late depth testing. Only two-sided meshlets can carry
    // masked or blended materials (they always use the foliage pipeline),
    // and the binning keeps them at the back of the slot's range, so the
    // one-sided meshlets are drawn first with the early-depth `_opaque`
    // program and only the two-sided ones with the alpha-tested program.
    if (opaqueSelected != nullptr && opaqueSelected->handle() != VK_NULL_HANDLE) {
        drawWith(*opaqueSelected, flags | MeshShaderSkipTwoSidedFlag, 1U);
        drawWith(*selected, flags | MeshShaderSkipOneSidedFlag, 2U);
        return;
    }
    drawWith(*selected, flags, 0U);
}

void ForwardPass::drawFoliage(VkCommandBuffer commandBuffer, const VkDescriptorSet sceneDescriptorSet,
                              const Culling::IndexedIndirectDrawCount& indirectDraw) const {
    if (!indirectDraw.valid())
        return;
    const GraphicsPipeline& pipeline =
        useReduced_ && reducedFoliagePipeline_.handle() != VK_NULL_HANDLE ? reducedFoliagePipeline_ : foliagePipeline_;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0, 1,
                            &sceneDescriptorSet, 0, nullptr);
    indirectDraw.record(commandBuffer);
}

void ForwardPass::end(VkCommandBuffer commandBuffer) {
    vkCmdEndRendering(commandBuffer);
}

} // namespace Engine
