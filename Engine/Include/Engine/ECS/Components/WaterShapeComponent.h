#pragma once

#include "Engine/ECS/Components/WaterBodyComponent.h"

#include <vector>

namespace Engine {
    /**
     * Geometry authoring for a WaterBodyComponent.  This intentionally has no
     * GPU handles or MeshRenderer dependency: it is scene data consumed by the
     * water extraction step.
     */
    struct WaterShapeComponent final {
        WaterBodyType type{WaterBodyType::Lake};
        float oceanExtent{65536.0F};
        std::vector<Vec3> lakePolygon;
        std::vector<RiverSplinePoint> riverSpline;
    };
}
