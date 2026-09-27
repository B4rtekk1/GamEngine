#include <FidelityFX/host/backends/vk/ffx_vk.h>

// FidelityFX SDK 1.1.4 links the Vulkan backend to Frame Interpolation's
// swapchain hook even when only Brixelizer is enabled. GamEngine does not use
// that effect. Its swapchain implementation is excluded from this build.
FFX_API FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(
    const FfxFrameGenerationConfig* /*config*/) {
    return FFX_ERROR_INVALID_ARGUMENT;
}
