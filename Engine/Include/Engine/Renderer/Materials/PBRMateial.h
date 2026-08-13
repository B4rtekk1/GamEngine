#pragma once

#include "Engine/Math/Vec3.h"

namespace Engine {

// Values follow the metallic/roughness workflow used by glTF.
struct PBRMaterial {
    Vec3 baseColor{1.0f, 1.0f, 1.0f};
    float metallic{0.0f};
    float roughness{0.55f};
    float ambientOcclusion{1.0f};
};

} // namespace Engine
