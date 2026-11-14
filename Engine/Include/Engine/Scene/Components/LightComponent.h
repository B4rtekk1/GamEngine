#pragma once

#include "Engine/Math/Math.h"

#include <cstdint>
#include <filesystem>

namespace Engine {
enum class LightType : uint8_t {
    Directional,
    Point,
    Spot,
};

/**
 * Photometric profile of a point or spot light: how its intensity varies with
 * the angle from the light's forward axis (local -Z). Built-in shapes model
 * common fixtures; Ies reads an IESNA LM-63 file. Profiles are authored for a
 * lamp aimed down (IES nadir = forward), so rotate such lights to point down.
 */
enum class LightProfile : uint8_t {
    None,
    Downlight,
    Lampshade,
    Sconce,
    Lantern,
    Globe,
    Ies,
};
inline constexpr int LightProfileCount = 7;

struct LightComponent {
    LightType type = LightType::Directional;

    Math::Color color{1.0F, 1.0F, 1.0F};
    float intensity = 1.0F;

    // Local lights fade smoothly to zero at this distance. It is ignored
    // by directional lights.
    float range = 10.0F;
    // Cone angles in degrees, used by spot lights only. The inner cone is
    // fully lit; the outer cone is the soft edge of the beam.
    float innerConeAngle = 20.0F;
    float outerConeAngle = 30.0F;

    bool enabled = true;
    bool castShadows = true;
    // Selects the sole directional light currently consumed by the forward
    // renderer. It does not control whether this light itself is enabled.
    bool mainLight = false;

    // Point and spot lights only; see LightProfile.
    LightProfile profile = LightProfile::None;
    /// IESNA LM-63 file for LightProfile::Ies, relative to the asset root.
    std::filesystem::path iesProfile{};
};
} // namespace Engine
