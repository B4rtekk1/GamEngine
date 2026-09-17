#pragma once

#include <cstdint>

namespace Engine {
    /** Public renderer quality and performance settings. No graphics backend types leak here. */
    enum class AntialiasingLevel : std::uint8_t {
        Off,
        MSAA2x,
        MSAA4x,
        /** Temporal anti-aliasing with HDR history accumulation. */
        TAA,
    };

    /**
     * Filter budget for directional virtual shadow maps.
     * Low uses one comparison per level. High never falls back to a single
     * comparison: it uses 3x3 near and 2x2 mid/far range. Ultra uses a
     * stable Poisson kernel of 12/8/4 taps; it deliberately does not rely
     * on temporal accumulation to hide a rotating stochastic pattern.
     */
    enum class ShadowQuality : std::uint8_t {
        Low,
        Medium,
        High,
        Ultra,
    };

    enum class GtaoQuality : std::uint8_t { Low, Medium, High, Ultra };

    /** Full-screen diagnostic stage for isolating GTAO artifacts. */
    enum class GtaoDebugView : std::uint8_t { Off, Raw, Filtered, Full };

    /** Temporary lighting isolators for diagnosing view-dependent PBR artifacts. */
    enum class PbrDebugView : std::uint8_t {
        FinalLighting,
        NoSpecularIbl,
        NoDirectSpecular,
        NoGtao,
    };

    /** Tunables are deliberately data, so automated GPU benchmarks can sweep them. */
    struct GtaoQualitySettings final {
        float resolutionScale;
        std::uint32_t directions;
        std::uint32_t stepsPerDirection;
        std::uint32_t depthMipCount;
        std::uint32_t denoisePassCount;
        bool temporal;
        float historyWeight;
        bool specularOcclusion;
        bool bentNormals;
    };

    constexpr GtaoQualitySettings gtaoQualitySettings(const GtaoQuality quality) noexcept {
        switch (quality) {
        case GtaoQuality::Low:    return {.resolutionScale=.5F, .directions=3, .stepsPerDirection=2, .depthMipCount=2, .denoisePassCount=1, .temporal=false, .historyWeight=0.F, .specularOcclusion=false, .bentNormals=false};
        case GtaoQuality::Medium: return {.resolutionScale=.5F, .directions=4, .stepsPerDirection=3, .depthMipCount=3, .denoisePassCount=1, .temporal=false, .historyWeight=0.F, .specularOcclusion=false, .bentNormals=false};
        // Match Intel XeGTAO High: 3 slices, 3 samples per side, at native
        // resolution. Half-resolution AO remains available in Low/Medium.
        case GtaoQuality::High:   return {.resolutionScale=1.F, .directions=3, .stepsPerDirection=3, .depthMipCount=5, .denoisePassCount=1, .temporal=false, .historyWeight=0.F, .specularOcclusion=false, .bentNormals=false};
        case GtaoQuality::Ultra:  return {.resolutionScale=1.F, .directions=8, .stepsPerDirection=4, .depthMipCount=5, .denoisePassCount=1, .temporal=false, .historyWeight=0.F, .specularOcclusion=false, .bentNormals=false};
        }
        return gtaoQualitySettings(GtaoQuality::High);
    }

    enum class IblQuality : std::uint8_t { Low, Medium, High, Ultra };
    struct IblQualitySettings final {
        std::uint32_t environmentResolution;
        std::uint32_t prefilterResolution;
        std::uint32_t reflectionProbeResolution;
        std::uint32_t probeUpdateBudget;
        bool useBc6h;
    };
    constexpr IblQualitySettings iblQualitySettings(const IblQuality quality) noexcept {
        switch (quality) {
        case IblQuality::Low: return {128, 64, 128, 1, false};
        case IblQuality::Medium: return {256, 128, 256, 1, false};
        case IblQuality::High: return {512, 256, 512, 2, false};
        case IblQuality::Ultra: return {1024, 512, 1024, 4, false};
        }
        return iblQualitySettings(IblQuality::High);
    }

    /** Diagnostic visualization of directional virtual-shadow sampling. */
    enum class ShadowDebugView : std::uint8_t {
        Off,
        BlockerCount,
        PenumbraRadius,
        /** The level selected from receiver geometry before cache fallback. */
        DesiredClipLevel,
        /** The resident level that supplied the shadow result. */
        ResolvedClipLevel,
        /** Cache-fallback distance: green=0, yellow=1, orange=2, red=3+. */
        FallbackDelta,
        VirtualPage,
    };

    struct RenderFeatures final {
        bool shadows = false;
        bool instancedRendering = true;
        bool meshDeduplication = true;
        bool transformCaching = true;
        bool materialCaching = true;
        bool gpuCulling = true;
        bool occlusionCulling = false;
    };

    /** Distance limits for the GPU-driven grass visibility streams, in metres. */
    struct GrassRenderSettings final {
        float renderDistance = 250.0F;
        float shadowDistance = 300.0F;
        float velocityDistance = 250.0F;
    };

    struct RenderConfig final {
        RenderFeatures features{};
        AntialiasingLevel antialiasing = AntialiasingLevel::Off;
        GrassRenderSettings grass{};
        ShadowQuality shadowQuality = ShadowQuality::High;
        GtaoQuality gtaoQuality = GtaoQuality::High;
        GtaoDebugView gtaoDebugView = GtaoDebugView::Off;
        PbrDebugView pbrDebugView = PbrDebugView::FinalLighting;
        IblQuality iblQuality = IblQuality::High;
        ShadowDebugView shadowDebugView = ShadowDebugView::Off;
    };

    /** Opaque viewport texture handle used by editor integrations. */
    struct ViewportHandle final {
        std::uintptr_t value{};
        [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    };

    struct EditorEventState final {
        bool quitRequested = false;
        bool togglePlay = false;
        bool togglePause = false;
    };
} // namespace Engine
