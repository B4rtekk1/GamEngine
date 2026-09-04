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
