#pragma once

#include "Engine/ECS/Components/ProceduralCloudComponent.h"
#include "Engine/Renderer/Geometry/Mesh.h"

namespace Engine {
    /** Builds a low-poly cloud from overlapping, randomly distributed ellipsoidal puffs. */
    class ProceduralCloud final {
    public:
        [[nodiscard]] static Mesh createMesh(const ProceduralCloudComponent& settings);
    };
} // namespace Engine
