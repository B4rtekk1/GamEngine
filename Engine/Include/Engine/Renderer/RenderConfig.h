#pragma once

#include <cstdint>

namespace Engine {

/** Public renderer quality and performance settings. No graphics backend types leak here. */
enum class AntialiasingLevel : std::uint32_t {
    Off,
    MSAA2x,
    MSAA4x
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

struct RenderConfig final {
    RenderFeatures features{};
    AntialiasingLevel antialiasing = AntialiasingLevel::Off;
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
