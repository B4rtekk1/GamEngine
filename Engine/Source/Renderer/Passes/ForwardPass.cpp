#include "Engine/Renderer/Passes/ForwardPass.h"

#include "Engine/Core/Diagnostics.h"
#include "Engine/Renderer/Culling/IndexedIndirectDrawCount.h"
#include "Engine/Renderer/Geometry/GpuVertex.h"
#include "Engine/Renderer/Vulkan/renderer_types.h"
#include <algorithm>
#include <cstddef>
#include <string>

namespace Engine {
    void ForwardPass::create(VkDevice device, const VkFormat colorFormat,
                             const VkFormat depthFormat,
                             const VkSampleCountFlagBits samples,
                             const VkFormat depthResolveFormat,
                             const VkResolveModeFlagBits depthResolveMode,
                             VkDescriptorSetLayout sceneLayout,
                             Assets::AssetManager &assets,
                             const VkImageLayout colorInitialLayout,
                             const bool colorInitialLayoutExternallySynchronized,
                             const VkFormat velocityFormat, const VkFormat viewNormalFormat, const bool preserveDepth,
                             const bool depthOnly) {
        reportedMissingShaderGraphSlots_.clear();
        hasVelocityAttachment_ = velocityFormat != VK_FORMAT_UNDEFINED;
        hasViewNormalAttachment_ = viewNormalFormat != VK_FORMAT_UNDEFINED;
        preserveDepth_ = preserveDepth;
        GraphicsPipelineOptions options{};
        options.colorFormat = colorFormat;
        options.dynamicRendering = true;
        options.additionalColorFormat = velocityFormat;
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
            options.additionalColorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                               VK_COLOR_COMPONENT_G_BIT;
            options.thirdColorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                          VK_COLOR_COMPONENT_G_BIT;
        }
        if (preserveDepth) {
            options.depthLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            options.depthInitialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            options.depthFinalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            options.depthWriteEnable = VK_FALSE;
            options.depthCompareOp = VK_COMPARE_OP_EQUAL;
        }
        options.shader = depthOnly
                             ? (hasVelocityAttachment_
                                    ? "shaders/depth_velocity.spv"
                                    : "shaders/depth_velocity_no_velocity.spv")
                             : "shaders/forward_pbr.spv";
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
            {3, 0, VK_FORMAT_A2B10G10R10_SNORM_PACK32, offsetof(GpuVertex, normal)},
            {8, 0, VK_FORMAT_R32_UINT, offsetof(GpuVertex, materialIndex)},
            {9, 0, VK_FORMAT_A2B10G10R10_SNORM_PACK32, offsetof(GpuVertex, tangent)},
        };
        std::array<std::filesystem::path, MaterialShaderCount> shaderPaths{};
        if (depthOnly) {
            shaderPaths.fill(options.shader);
        } else if (hasVelocityAttachment_) {
            shaderPaths = {
                "shaders/forward_pbr.spv", "shaders/forward_unlit.spv",
                "shaders/forward_hologram.spv", "shaders/forward_water.spv"
            };
        } else {
            shaderPaths = {
                "shaders/forward_pbr_no_velocity.spv", "shaders/forward_unlit_no_velocity.spv",
                "shaders/forward_hologram_no_velocity.spv", "shaders/forward_water_no_velocity.spv"
            };
        }
        for (std::size_t index = 0; index < shaderPaths.size(); ++index) {
            // Water has its own render pass and a second descriptor set for the
            // immutable opaque scene.  Building a forward-compatible pipeline
            // here produces a validation warning (velocity target mismatch) and
            // can never be selected by a draw loop.
            if (index == materialShaderIndex(MaterialShader::Water)) {
                continue;
            }
            GraphicsPipelineOptions materialOptions = options;
            materialOptions.shader = shaderPaths[index];
            materialPipelines_[index].create(device, materialOptions);
        }
        shaderGraphPipelineOptions_ = options;
        shaderGraphPipelineOptions_.shader.clear();
        shaderGraphPipelines_.initialize(device, shaderGraphPipelineOptions_);

        GraphicsPipelineOptions foliageOptions = options;
        foliageOptions.shader = depthOnly
                                    ? options.shader
                                    : hasVelocityAttachment_
                                          ? "shaders/forward_pbr.spv"
                                          : "shaders/forward_pbr_no_velocity.spv";
        foliageOptions.cullMode = VK_CULL_MODE_NONE;
        // Vegetation cards use alpha cutout.  They must populate depth before the
        // sky draw and TAA resolve; treating them as a generic transparent stream
        // leaves their pixels unoccluding and causes background bleed/shimmer.
        // Genuine, sorted transparency belongs in a separate pipeline.
        foliageOptions.alphaBlendEnable = VK_FALSE;
        // Preserve the base pass's depth mode: lighting reads the depth prepass,
        // while the regular forward pass writes depth.
        foliagePipeline_.create(device, foliageOptions);
        GraphicsPipelineOptions grassOptions = foliageOptions;
        grassOptions.shader = hasVelocityAttachment_
                                  ? "shaders/grass_forward.spv"
                                  : "shaders/grass_forward_no_velocity.spv";
        // Grass consumes only position, color, UV0, normal, and material index.
        // Supplying UV1/tangents causes Vulkan validation warnings at locations 4/9.
        std::erase_if(grassOptions.vertexAttributes,
                      [](const VkVertexInputAttributeDescription &attribute) {
                          return attribute.location == 4 || attribute.location == 9;
                      });
        grassPipeline_.create(device, grassOptions);

        GraphicsPipelineOptions outlineOptions = options;
        outlineOptions.shader = hasVelocityAttachment_
                                    ? "shaders/selection_outline.spv"
                                    : "shaders/selection_outline_no_velocity.spv";
        outlineOptions.cullMode = VK_CULL_MODE_FRONT_BIT;
        outlineOptions.depthWriteEnable = VK_FALSE;
        outlineOptions.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        outlineOptions.vertexAttributes.erase(
            std::remove_if(outlineOptions.vertexAttributes.begin(),
                           outlineOptions.vertexAttributes.end(),
                           [](const VkVertexInputAttributeDescription &attribute) {
                               // The outline only needs position (0), normal (3),
                               // and the instance model columns (4-7).
                               return attribute.location == 1 ||
                                      attribute.location == 2 ||
                                      attribute.location == 8 ||
                                      attribute.location >= 9;
                           }),
            outlineOptions.vertexAttributes.end());
        outlinePipeline_.create(device, outlineOptions);
    }

    void ForwardPass::destroy() noexcept {
        reportedMissingShaderGraphSlots_.clear();
        hasVelocityAttachment_ = false;
        hasViewNormalAttachment_ = false;
        preserveDepth_ = false;
        shaderGraphPipelines_.destroy();
        outlinePipeline_.destroy();
        foliagePipeline_.destroy();
        grassPipeline_.destroy();
        for (auto &pipeline: materialPipelines_) pipeline.destroy();
    }

    std::uint32_t
    ForwardPass::registerShaderGraph(const ShaderGraphProgram &program, const MaterialRenderState &state) {
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

    bool ForwardPass::hasMaterialPipeline(const std::uint32_t shaderSlot) const noexcept {
        return shaderSlot < MaterialShaderCount || shaderGraphPipelines_.find(shaderSlot) != nullptr;
    }

    void ForwardPass::drawShaderGraph(VkCommandBuffer commandBuffer, VkDescriptorSet sceneDescriptorSet,
                                      const std::uint32_t shaderSlot,
                                      const Culling::IndexedIndirectDrawCount &indirectDraw,
                                      const VkDeviceSize commandOffset, const VkDeviceSize countOffset) const {
        if (!indirectDraw.valid()) return;
        const GraphicsPipeline *const pipeline = shaderGraphPipelines_.find(shaderSlot);
        if (pipeline == nullptr) {
            if (reportedMissingShaderGraphSlots_.insert(shaderSlot).second) {
                Diagnostics::instance().report(
                    DiagnosticSeverity::Error,
                    "[Renderer] Shader Graph pipeline missing for slot " +
                    std::to_string(shaderSlot) + "; falling back to Standard PBR.");
            }
            const auto &fallback = materialPipelines_[materialShaderIndex(MaterialShader::StandardPBR)];
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, fallback.handle());
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, fallback.layout(),
                                    0, 1, &sceneDescriptorSet, 0, nullptr);
            indirectDraw.record(commandBuffer, commandOffset, countOffset);
            return;
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->handle());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout(),
                                0, 1, &sceneDescriptorSet, 0, nullptr);
        indirectDraw.record(commandBuffer, commandOffset, countOffset);
    }

    void ForwardPass::drawGrass(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                                const Culling::IndexedIndirectDrawCount &indirectDraw) const {
        if (!indirectDraw.valid()) return;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, grassPipeline_.handle());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, grassPipeline_.layout(),
                                0, 1, &descriptorSet, 0, nullptr);
        indirectDraw.record(commandBuffer);
    }

    void ForwardPass::begin(VkCommandBuffer commandBuffer,
                            const VkImageView colorView, const VkImageView depthView,
                            const VkImageView velocityView, const VkImageView viewNormalView,
                            const VkImageView colorResolveView, const VkImageView depthResolveView,
                            const VkExtent2D extent,
                            VkDescriptorSet sceneDescriptorSet,
                            VkBuffer vertexBuffer, VkBuffer instanceBuffer,
                            VkBuffer indexBuffer) const {
        (void) instanceBuffer; // Instance data is fetched from descriptor binding 5.
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = colorView;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.02F, 0.02F, 0.05F, 1.0F}};
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
        std::array colors{color, velocity, normals};
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depthView;
        depth.imageLayout = preserveDepth_
                                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
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
        rendering.colorAttachmentCount = 1U + (hasVelocityAttachment_ ? 1U : 0U) +
                                         (hasViewNormalAttachment_ ? 1U : 0U);
        rendering.pColorAttachments = colors.data();
        rendering.pDepthAttachment = &depth;
        vkCmdBeginRendering(commandBuffer, &rendering);

        const auto &pipeline = materialPipelines_[0];
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline.layout(), 0, 1, &sceneDescriptorSet, 0, nullptr);
        constexpr VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, offsets);
        vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        const VkViewport viewport{
            0.0F, 0.0F, static_cast<float>(extent.width),
            static_cast<float>(extent.height), 0.0F, 1.0F
        };
        const VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    }

    void ForwardPass::draw(VkCommandBuffer commandBuffer,
                           const Culling::IndexedIndirectDrawCount &indirectDraw) {
        if (indirectDraw.valid()) {
            indirectDraw.record(commandBuffer);
        }
    }

    void ForwardPass::drawMaterial(VkCommandBuffer commandBuffer, VkDescriptorSet sceneDescriptorSet,
                                   const std::uint32_t shaderSlot,
                                   const Culling::IndexedIndirectDrawCount &indirectDraw,
                                   const VkDeviceSize commandOffset,
                                   const VkDeviceSize countOffset) const {
        if (!indirectDraw.valid()) return;
        if (shaderSlot >= MaterialShaderCount) {
            drawShaderGraph(commandBuffer, sceneDescriptorSet, shaderSlot, indirectDraw, commandOffset, countOffset);
            return;
        }
        const auto &pipeline = materialPipelines_[shaderSlot];
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                                0, 1, &sceneDescriptorSet, 0, nullptr);
        indirectDraw.record(commandBuffer, commandOffset, countOffset);
    }

    void ForwardPass::drawFoliage(
        VkCommandBuffer commandBuffer, const VkDescriptorSet sceneDescriptorSet,
        const Culling::IndexedIndirectDrawCount &indirectDraw) const {
        if (!indirectDraw.valid()) return;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, foliagePipeline_.handle());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                foliagePipeline_.layout(), 0, 1, &sceneDescriptorSet, 0, nullptr);
        indirectDraw.record(commandBuffer);
    }

    void ForwardPass::end(VkCommandBuffer commandBuffer) {
        vkCmdEndRendering(commandBuffer);
    }

    void ForwardPass::drawOutline(
        VkCommandBuffer commandBuffer,
        const VkDescriptorSet sceneDescriptorSet,
        const Culling::IndexedIndirectDrawCount &indirectDraw) const {
        if (!indirectDraw.valid()) return;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          outlinePipeline_.handle());
        // Sky and particle draws bind their own descriptor sets. Bind the scene
        // set again because the outline pipeline uses the forward-pass layout.
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                outlinePipeline_.layout(), 0, 1,
                                &sceneDescriptorSet, 0, nullptr);
        indirectDraw.record(commandBuffer);
    }
} // namespace Engine
