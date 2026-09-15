#pragma once

#include "Engine/ECS/Components/WaterBodyComponent.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Renderer/Geometry/MeshGpuResource.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace Engine {
class Registry;
class SceneGpuResources;

namespace Water {
/**
 * Immutable, renderer-facing view of authored water for one scene generation.
 *
 * It deliberately copies the ECS values and draw ranges needed by Virtual
 * Water.  Once captured, a frame rebuild never follows ECS-owned pointers or
 * entity indices while it creates GPU page templates.
 */
struct WaterRenderBody final {
    WaterBodyComponent water{};
    std::uint32_t instanceIndex{};
    std::shared_ptr<const MeshGpuResource> meshResource;
    std::vector<Mesh::DrawRange> drawRanges;
};

class WaterRenderWorld final {
public:
    [[nodiscard]] static WaterRenderWorld capture(const Registry& registry,
                                                   const SceneGpuResources& sceneGpu);

    [[nodiscard]] const std::vector<WaterRenderBody>& bodies() const noexcept { return bodies_; }
    [[nodiscard]] bool empty() const noexcept { return bodies_.empty(); }

private:
    std::vector<WaterRenderBody> bodies_;
};
} // namespace Water
} // namespace Engine
