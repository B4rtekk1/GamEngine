#include "Engine/Renderer/Passes/RainPass.h"

#include "Engine/Assets/AssetManager.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Engine {
namespace {
// Must match RainParams in Weather/rain.slang.
struct PushConstants {
    float box[4];
    float look[4];
};
static_assert(sizeof(PushConstants) == 32, "Rain push constants must fit 128 bytes");

// Box around the camera that holds the drops: further rain is too thin to
// resolve and the volumetric fog stands in for it.
constexpr float BoxHalfWidth = 12.0F;
constexpr float BoxHalfHeight = 7.0F;
constexpr float FallSpeed = 9.0F;     // m/s, a typical raindrop terminal velocity
constexpr float StreakLength = 0.45F; // m, the motion blur of a ~1/20 s exposure
constexpr float StreakWidth = 0.004F; // m
constexpr float Opacity = 0.16F;
constexpr float Brightness = 3.0F;

[[nodiscard]] VkImageMemoryBarrier2 colorBarrier(const VkImage image, const VkPipelineStageFlags2 srcStage,
                                                 const VkAccessFlags2 srcAccess, const VkPipelineStageFlags2 dstStage,
                                                 const VkAccessFlags2 dstAccess, const VkImageLayout oldLayout,
                                                 const VkImageLayout newLayout) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return barrier;
}

void submit(const VkCommandBuffer commandBuffer, const VkImageMemoryBarrier2& barrier) {
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}
} // namespace

RainPass::~RainPass() {
    destroy();
}

void RainPass::create(const CreateInfo& info) {
    destroy();
    if (info.device == VK_NULL_HANDLE || info.assets == nullptr || info.sceneLayout == VK_NULL_HANDLE ||
        info.colorFormat == VK_FORMAT_UNDEFINED || info.depthFormat == VK_FORMAT_UNDEFINED)
        throw std::invalid_argument("RainPass requires a device, assets, the scene layout and target formats");
    info_ = info;
    device_ = info.device;
    GraphicsPipelineOptions options{};
    options.colorFormat = info.colorFormat;
    options.depthFormat = info.depthFormat;
    options.samples = VK_SAMPLE_COUNT_1_BIT;
    options.colorLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    options.depthLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    options.colorInitialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    options.colorFinalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    options.depthInitialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    options.depthFinalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    options.shader = "shaders/rain.spv";
    options.assetManager = info.assets;
    options.pushConstantSize = sizeof(PushConstants);
    options.pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    options.cullMode = VK_CULL_MODE_NONE;
    options.depthWriteEnable = VK_FALSE;
    options.depthTestEnable = VK_TRUE;
    options.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    options.alphaBlendEnable = VK_TRUE;
    options.descriptorSetLayouts = {info.sceneLayout};
    try {
        pipeline_.create(device_, options);
    } catch (...) {
        destroy();
        throw;
    }
}

void RainPass::record(const VkCommandBuffer commandBuffer, const RecordInfo& info) {
    if (!created() || info.sceneSet == VK_NULL_HANDLE || info.colorView == VK_NULL_HANDLE ||
        info.depthView == VK_NULL_HANDLE || info.intensity <= 0.0F)
        return;
    const auto drops = static_cast<std::uint32_t>(std::lround(static_cast<double>(MaxDrops) *
                                                              std::clamp(info.intensity, 0.0F, 1.0F)));
    if (drops == 0)
        return;
    // The lit colour was written as an attachment or from compute (fog) and
    // is SHADER_READ_ONLY; blend the streaks into it as an attachment.
    submit(commandBuffer,
           colorBarrier(info.color,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = info.colorView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = info.depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, info.extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(commandBuffer, &rendering);
    const VkViewport viewport{0.0F, 0.0F, static_cast<float>(info.extent.width),
                              static_cast<float>(info.extent.height), 0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, info.extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(), 0, 1, &info.sceneSet,
                            0, nullptr);
    // Light rain is sparser and fainter, not just fewer drops.
    const float visibility = 0.5F + 0.5F * std::clamp(info.intensity, 0.0F, 1.0F);
    const PushConstants push{{BoxHalfWidth, BoxHalfHeight, FallSpeed, StreakLength},
                             {StreakWidth, Opacity * visibility, info.time, Brightness}};
    vkCmdPushConstants(commandBuffer, pipeline_.layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);
    vkCmdDraw(commandBuffer, 6, drops, 0, 0);
    vkCmdEndRendering(commandBuffer);

    submit(commandBuffer,
           colorBarrier(info.color, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
}

void RainPass::destroy() noexcept {
    pipeline_.destroy();
    info_ = {};
    device_ = VK_NULL_HANDLE;
}
} // namespace Engine
