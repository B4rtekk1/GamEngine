#pragma once

#include "Engine/Math/Color.h"

namespace Engine {

// Values follow the metallic/roughness workflow used by glTF.
struct PBRMaterial {
    Math::Color baseColor = Math::Color::white();
    float metallic{0.0f};
    float roughness{0.55f};
    float ambientOcclusion{1.0f};
};

} // namespace Engine
