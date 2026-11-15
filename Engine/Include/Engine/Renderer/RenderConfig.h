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

/** Selects whether compute render passes use the async queue or run in order on graphics. */
enum class RenderPath : std::uint8_t {
    AsyncCompute,
    Sequential,
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

enum class GtaoQuality : std::uint8_t { Low, Medium, High, Ultra, PerformanceHalf, BalancedHalf };

/** Optional short-range hardware ray queried visibility refinement. */
enum class ContactShadowMode : std::uint8_t { Off, RayTraced };

struct RtContactShadowSettings final {
    ContactShadowMode mode = ContactShadowMode::Off;
    float maxDistance = 2.0F;
    float normalBias = 0.005F;
    // Keep full resolution until the forward pass has depth/normal-aware
    // reconstruction; linear upsampling of a half-res mask creates halos.
    float resolutionScale = 1.0F;
};

/** Coarse renderer profiles intended for quick editor/runtime switching. */
enum class RenderQualityPreset : std::uint8_t { Low, Medium, High, Ultra };

struct RenderQualityPresetSettings final {
    ShadowQuality shadows;
    GtaoQuality gtao;
    AntialiasingLevel antialiasing;
};

constexpr RenderQualityPresetSettings renderQualityPresetSettings(const RenderQualityPreset preset) noexcept {
    switch (preset) {
    case RenderQualityPreset::Low:
        return {ShadowQuality::Low, GtaoQuality::Low, AntialiasingLevel::Off};
    case RenderQualityPreset::Medium:
        return {ShadowQuality::Medium, GtaoQuality::Medium, AntialiasingLevel::TAA};
    case RenderQualityPreset::High:
        return {ShadowQuality::High, GtaoQuality::High, AntialiasingLevel::TAA};
    case RenderQualityPreset::Ultra:
        return {ShadowQuality::Ultra, GtaoQuality::Ultra, AntialiasingLevel::TAA};
    }
    return renderQualityPresetSettings(RenderQualityPreset::High);
}

/** Full-screen diagnostic stage for isolating GTAO artifacts. */
enum class GtaoDebugView : std::uint8_t { Off, Raw, Filtered, Full };

/** Temporary lighting isolators for diagnosing view-dependent PBR artifacts. */
enum class PbrDebugView : std::uint8_t {
    FinalLighting,
    NoSpecularIbl,
    NoDirectSpecular,
    NoGtao,
    GeometricDirectDiffuse,
    Albedo,
    Normals,
    Roughness,
    Metallic,
    AmbientOcclusion,
    /** The specular reflection each pixel resolves (SSR -> probe -> sky), before Fresnel. */
    Reflections,
    /** SSR coverage: green hit confidence, red traced but missed, blue not traced. */
    SsrConfidence,
};

/** Tunables are deliberately data, so automated GPU benchmarks can sweep them. */
struct GtaoQualitySettings final {
    float resolutionScale;
    std::uint32_t directions;
    std::uint32_t stepsPerDirection;
    std::uint32_t depthMipCount;
    std::uint32_t denoisePassCount;
};

constexpr GtaoQualitySettings gtaoQualitySettings(const GtaoQuality quality) noexcept {
    switch (quality) {
    case GtaoQuality::Low:
        return {.resolutionScale = 1.F,
                .directions = 1,
                .stepsPerDirection = 2,
                .depthMipCount = 2,
                .denoisePassCount = 1};
    case GtaoQuality::Medium:
        return {.resolutionScale = 1.F,
                .directions = 2,
                .stepsPerDirection = 2,
                .depthMipCount = 3,
                .denoisePassCount = 1};
    case GtaoQuality::High:
        return {.resolutionScale = 1.F,
                .directions = 3,
                .stepsPerDirection = 3,
                .depthMipCount = 5,
                .denoisePassCount = 1};
    case GtaoQuality::Ultra:
        return {.resolutionScale = 1.F,
                .directions = 9,
                .stepsPerDirection = 3,
                .depthMipCount = 5,
                .denoisePassCount = 1};
    case GtaoQuality::PerformanceHalf:
        return {.resolutionScale = .5F,
                .directions = 3,
                .stepsPerDirection = 2,
                .depthMipCount = 2,
                .denoisePassCount = 1};
    case GtaoQuality::BalancedHalf:
        return {.resolutionScale = .5F,
                .directions = 4,
                .stepsPerDirection = 3,
                .depthMipCount = 3,
                .denoisePassCount = 1};
    }
    return gtaoQualitySettings(GtaoQuality::High);
}

/**
 * Screen-space diffuse global illumination. A half-resolution visibility-bitmask
 * trace reprojects the previous frame's lit colour, so on-screen surfaces bounce
 * light onto each other (one more bounce per frame) without ray tracing.
 */
enum class SsgiQuality : std::uint8_t { Off, Low, Medium, High };

struct SsgiSettings final {
    SsgiQuality quality = SsgiQuality::Medium;
    /** World-space reach of the bounce, in metres. */
    float radius = 4.0F;
    /** Assumed depth of on-screen surfaces; light passes behind thinner objects. */
    float thickness = 0.5F;
    float intensity = 1.0F;
    bool operator==(const SsgiSettings&) const noexcept = default;
};

struct SsgiQualitySettings final {
    std::uint32_t slices;
    std::uint32_t stepsPerSide;
};

constexpr SsgiQualitySettings ssgiQualitySettings(const SsgiQuality quality) noexcept {
    switch (quality) {
    case SsgiQuality::Off:
        return {0, 0};
    case SsgiQuality::Low:
        return {1, 8};
    case SsgiQuality::Medium:
        return {2, 10};
    case SsgiQuality::High:
        return {3, 14};
    }
    return ssgiQualitySettings(SsgiQuality::Medium);
}

/**
 * Ray-traced dynamic diffuse global illumination (DDGI) on a camera-centred
 * probe grid. Optional: it needs ray tracing pipelines with cluster and
 * partitioned acceleration structures (NVIDIA RTX Mega Geometry). While it
 * is active it replaces both the sky irradiance and SSGI for diffuse light.
 */
enum class DdgiQuality : std::uint8_t { Low, Medium, High };

struct DdgiSettings final {
    bool enabled = false;
    DdgiQuality quality = DdgiQuality::Medium;
    /** World-space distance between neighbouring probes, in metres. */
    float probeSpacing = 2.0F;
    float intensity = 1.0F;
    bool operator==(const DdgiSettings&) const noexcept = default;
};

struct DdgiQualitySettings final {
    std::uint32_t raysPerProbe;
    /** Every probe is refreshed once per this many frames. */
    std::uint32_t updateInterval;
};

constexpr DdgiQualitySettings ddgiQualitySettings(const DdgiQuality quality) noexcept {
    switch (quality) {
    case DdgiQuality::Low:
        return {96, 4};
    case DdgiQuality::Medium:
        return {128, 2};
    case DdgiQuality::High:
        return {192, 1};
    }
    return ddgiQualitySettings(DdgiQuality::Medium);
}

/** The diffuse global illumination technique a scene is lit with. */
enum class GlobalIlluminationMode : std::uint8_t {
    /** Sky irradiance only, occluded by GTAO. */
    Off,
    /** SSGI: one bounce per frame from on-screen surfaces, no ray tracing. */
    ScreenSpace,
    /** DDGI: ray-traced probes; needs cluster ray tracing, else falls back to Off. */
    RayTracedProbes,
};

/** Per-scene GI choice with the parameters of both techniques. */
struct GlobalIlluminationSettings final {
    GlobalIlluminationMode mode = GlobalIlluminationMode::ScreenSpace;
    SsgiSettings ssgi{};
    DdgiSettings ddgi{};
    bool operator==(const GlobalIlluminationSettings&) const noexcept = default;
};

/** Display transform applied after exposure. */
enum class TonemapOperator : std::uint8_t {
    /** AgX: bright saturated light desaturates towards white like film. */
    AgX,
    /** Fitted ACES curve on luminance; keeps hue but never reaches white. */
    AcesLuminance,
};

/**
 * Camera and colour settings of the final image: exposure, display
 * transform, colour grading and lens effects. Saved per scene.
 */
struct PostProcessSettings final {
    /** Meters the scene and adapts like an eye; otherwise exposure is fixed. */
    bool autoExposure = true;
    /** Added to the metered (or fixed) exposure, in stops. */
    float exposureCompensation = 0.0F;
    /** Limits of the metered scene luminance, in log2 units. */
    float minLuminanceEv = -5.0F;
    float maxLuminanceEv = 12.0F;
    /** Adaptation rate towards brighter and darker scenes, per second. */
    float adaptationSpeedUp = 3.0F;
    float adaptationSpeedDown = 1.0F;
    TonemapOperator tonemapper = TonemapOperator::AgX;
    /** Colour grading. Contrast pivots around middle grey. */
    float contrast = 1.0F;
    float saturation = 1.0F;
    /** White balance: -1 cool .. 1 warm, -1 magenta .. 1 green. */
    float temperature = 0.0F;
    float tint = 0.0F;
    /** Strength of the glow around bright light (lamps, sun glints, emission). */
    float bloom = 0.04F;
    /** Lens. Vignette darkens the corners; aberration splits colour radially. */
    float vignette = 0.25F;
    float chromaticAberration = 0.1F;
    /** Fraction of the frame time the virtual shutter is open; 0 disables. */
    float motionBlur = 0.5F;
    /** Depth of field (needs TAA, which provides linear depth). */
    bool depthOfField = false;
    float focusDistance = 10.0F;
    /** 0..1: how strongly the background beyond the focus blurs. */
    float aperture = 0.5F;
    float maxBlurPixels = 12.0F;
    bool operator==(const PostProcessSettings&) const noexcept = default;
};

/**
 * Glossy reflections of on-screen surfaces and rain-soaked materials.
 *
 * SSR walks each pixel's reflection ray through a min-depth pyramid built
 * from the opaque prepass (the Hi-Z trace GlassPass uses) and reflects the
 * previous frame's lit colour, blurred by roughness. It replaces the probe/sky reflection
 * where the ray finds a surface and fades back to it elsewhere. It needs
 * the prepass view normals, so it is inactive under MSAA.
 *
 * Wetness darkens porous albedo and smooths every surface; puddles collect
 * on upward-facing ground (cavities first) and become flat water mirrors.
 * Both are world-space and apply to every view.
 */
struct SurfaceSettings final {
    bool screenSpaceReflections = false;
    float ssrIntensity = 1.0F;
    /** Materials rougher than this keep the probe/sky reflection. */
    float ssrMaxRoughness = 0.6F;
    /** Assumed depth of every surface, in metres (plus 3% of view depth), when deciding a hit. */
    float ssrThickness = 0.15F;
    /** Hi-Z traversal steps per pixel for mirror-like surfaces; rough ones get half. */
    std::uint32_t ssrMaxSteps = 48;
    /** 0: dry, 1: soaked. */
    float wetness = 0.0F;
    /** Fraction of upward-facing ground covered by standing water. */
    float puddleCoverage = 0.0F;
    /** Size of a puddle, in metres. */
    float puddleScale = 2.5F;
    /** Strength of the raindrop ripples on standing water; 0 disables them. */
    float rainRipples = 0.0F;
    /**
     * Falling rain, 0 (none) to 1 (downpour): the number of lit rain streaks
     * around the Game View camera and how many drops ripple the puddles.
     */
    float rainIntensity = 0.0F;
    /** How often drops land: scales both the drop rate and the share of drop sites hit. */
    float rippleFrequency = 1.0F;
    /** Spacing of the drop sites, in metres; rings grow to about this radius. */
    float rippleSize = 0.3F;
    bool operator==(const SurfaceSettings&) const noexcept = default;
};

enum class VolumetricFogQuality : std::uint8_t { Low, Medium, High };

/** Froxel grid of the volumetric fog: screen pixels per cell edge and depth slices. */
struct VolumetricFogQualitySettings final {
    std::uint32_t tilePixels;
    std::uint32_t slices;
};
constexpr VolumetricFogQualitySettings volumetricFogQualitySettings(const VolumetricFogQuality quality) noexcept {
    switch (quality) {
    case VolumetricFogQuality::Low:
        return {16, 48};
    case VolumetricFogQuality::Medium:
        return {16, 64};
    case VolumetricFogQuality::High:
        return {8, 96};
    }
    return {16, 64};
}

/**
 * Volumetric (froxel) fog: participating media in a camera-aligned grid,
 * lit by the sun with its virtual shadow map, by every local light through
 * the clustered light lists, and by the sky irradiance. Accumulated over
 * frames with reprojection, integrated front to back and composited over the
 * lit scene before TAA (Game View only).
 */
struct FogSettings final {
    bool enabled = false;
    VolumetricFogQuality quality = VolumetricFogQuality::Medium;
    /** Extinction at and below baseHeight, per metre. */
    float density = 0.02F;
    /** Exponential falloff of density above baseHeight, per metre. */
    float heightFalloff = 0.15F;
    float baseHeight = 0.0F;
    /** Scattering colour (single-scattering albedo). */
    float albedoR = 0.9F;
    float albedoG = 0.92F;
    float albedoB = 1.0F;
    /** Henyey-Greenstein g: 0 isotropic, towards 1 strong glow around lights. */
    float anisotropy = 0.55F;
    float localLightScattering = 1.0F;
    float directionalScattering = 1.0F;
    float ambientScattering = 1.0F;
    /** Animated density variation; 0 keeps the fog uniform. */
    float noise = 0.4F;
    /** Depth the froxel grid covers, in metres; the fog stops beyond it. */
    float distance = 64.0F;
    /** Share of the reprojected history kept per frame. */
    float temporalBlend = 0.9F;
    bool operator==(const FogSettings&) const noexcept = default;
};

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
    case IblQuality::Low:
        return {128, 64, 128, 1, false};
    case IblQuality::Medium:
        return {256, 128, 256, 1, false};
    case IblQuality::High:
        return {512, 256, 512, 2, false};
    case IblQuality::Ultra:
        return {1024, 512, 1024, 4, false};
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
    /** Green: desired page, yellow: fallback page, red: no resident page. */
    Residency,
};

struct RenderFeatures final {
    bool shadows = false;
    bool instancedRendering = true;
    bool meshDeduplication = true;
    bool transformCaching = true;
    bool materialCaching = true;
    bool gpuCulling = true;
    // Hi-Z occlusion culling reuses the hierarchy that virtual shadow maps
    // already build every frame, so enabling it costs no extra GPU work.
    // The cull test keeps expanding bounds and disables itself on camera
    // cuts; MSAA paths without conservative depth resolve fall back to
    // frustum-only culling in canUseHiZOcclusionCulling().
    bool occlusionCulling = true;
    // Rasterize GPU-culled meshlets with VK_EXT_mesh_shader (one workgroup
    // per meshlet, per-triangle culling) instead of one indexed draw per
    // meshlet run. Ignored on devices without mesh shaders.
    bool meshShaders = true;
};

/** Distance limits for the GPU-driven grass visibility streams, in metres. */
struct GrassRenderSettings final {
    float renderDistance = 250.0F;
    float shadowDistance = 300.0F;
    float velocityDistance = 250.0F;
};

struct RenderConfig final {
    RenderFeatures features{};
    RenderPath renderPath = RenderPath::AsyncCompute;
    AntialiasingLevel antialiasing = AntialiasingLevel::TAA;
    GrassRenderSettings grass{};
    ShadowQuality shadowQuality = ShadowQuality::High;
    RtContactShadowSettings rtContactShadows{};
    GtaoQuality gtaoQuality = GtaoQuality::High;
    GtaoDebugView gtaoDebugView = GtaoDebugView::Off;
    PbrDebugView pbrDebugView = PbrDebugView::FinalLighting;
    IblQuality iblQuality = IblQuality::High;
    ShadowDebugView shadowDebugView = ShadowDebugView::Off;
    SsgiSettings ssgi{};
    DdgiSettings ddgi{};
    PostProcessSettings postProcess{};
    SurfaceSettings surface{};
    FogSettings fog{};
};

/** Opaque viewport texture handle used by editor integrations. */
struct ViewportHandle final {
    std::uintptr_t value{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return value != 0;
    }
};

struct EditorEventState final {
    bool quitRequested = false;
    bool togglePlay = false;
    bool togglePause = false;
};
} // namespace Engine
