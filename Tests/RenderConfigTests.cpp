#include <Engine/Renderer/RenderConfig.h>

#include <cstdint>

int main() {
    using namespace Engine;

    const RenderConfig defaults;
    if (defaults.antialiasing != AntialiasingLevel::Off ||
        defaults.features.shadows || !defaults.features.instancedRendering ||
        !defaults.features.meshDeduplication || !defaults.features.transformCaching ||
        !defaults.features.materialCaching || !defaults.features.gpuCulling ||
        defaults.features.occlusionCulling) return 1;

    RenderConfig configured;
    configured.antialiasing = AntialiasingLevel::MSAA4x;
    configured.features.shadows = true;
    configured.features.occlusionCulling = true;
    configured.features.gpuCulling = false;
    if (configured.antialiasing != AntialiasingLevel::MSAA4x ||
        !configured.features.shadows || !configured.features.occlusionCulling ||
        configured.features.gpuCulling) return 2;

    const ViewportHandle empty;
    const ViewportHandle valid{static_cast<std::uintptr_t>(42)};
    if (static_cast<bool>(empty) || !static_cast<bool>(valid) || valid.value != 42) return 3;

    EditorEventState events;
    if (events.quitRequested || events.togglePlay || events.togglePause) return 4;
    events.togglePlay = true;
    events.togglePause = true;
    if (!events.togglePlay || !events.togglePause || events.quitRequested) return 5;

    return 0;
}
