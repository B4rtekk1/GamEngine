#pragma once

#include "Engine/Math/Color.h"

#include <cstdint>

namespace Engine {

// Values follow the metallic/roughness workflow used by glTF.
struct PBRMaterial {
    Math::Color baseColor = Math::Color::white();
    float metallic{0.0F};
    float roughness{0.55F};
    float ambientOcclusion{1.0F};
    std::int32_t baseColorTexture{-1};
    std::int32_t metallicRoughnessTexture{-1};
    std::int32_t normalTexture{-1};
    // glTF normalTexture.scale; zero explicitly disables normal-map detail.
    float normalScale{1.0F};
    bool alphaBlend{false};
    // glTF `doubleSided`: foliage and thin cards must remain visible from
    // either side. The fragment shader flips their shading normal per face.
    bool doubleSided{false};
    float alphaCutoff{0.5F};
};

} // namespace Engine