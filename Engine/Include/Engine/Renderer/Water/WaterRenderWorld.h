#pragma once

#include "Engine/ECS/Components/WaterBodyComponent.h"
#include "Engine/ECS/Components/WaterShapeComponent.h"
#include "Engine/ECS/Entity.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Renderer/Geometry/MeshGpuResource.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace Engine {
class Registry;
class SceneGpuResources;

namespace Water {
struct WaterBodyId final {
    std::uint32_t index{};
    std::uint32_t generation{};

    [[nodiscard]] constexpr bool operator==(const WaterBodyId&) const noexcept = default;
    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0U || generation != 0U; }
};

/**
 * Immutable, renderer-facing view of authored water for one scene generation.
 *
 * It deliberately copies the ECS values and draw ranges needed by Virtual
 * Water.  Once captured, a frame rebuild never follows ECS-owned pointers or
 * entity indices while it creates GPU page templates.
 */
struct WaterRenderBody final {
    WaterBodyId id{};
    WaterBodyComponent water{};
    WaterShapeComponent shape{};
    std::uint32_t instanceIndex{};
    std::uint32_t spectrumRevision{};
    std::shared_ptr<const MeshGpuResource> meshResource;
    std::vector<Mesh::DrawRange> drawRanges;
};

class WaterRenderWorld final {
public:
    [[nodiscard]] static WaterRenderWorld capture(const Registry& registry,
                                                   const SceneGpuResources& sceneGpu);

    [[nodiscard]] const std::vector<WaterRenderBody>& bodies() const noexcept { return bodies_; }
    [[nodiscard]] bool empty() const noexcept { return bodies_.empty(); }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

private:
    std::uint64_t generation_{};
    std::vector<WaterRenderBody> bodies_;
};
} // namespace Water
} // namespace Engine
