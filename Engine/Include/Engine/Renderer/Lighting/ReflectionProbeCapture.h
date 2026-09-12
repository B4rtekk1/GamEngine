#pragma once

#include "Engine/Math/Mat4.h"
#include "Engine/Math/Vec3.h"

#include <array>
#include <cstdint>

namespace Engine {

/** Immutable camera data for one cubemap face.  It deliberately does not use
 * Camera: the +/-Y faces need an explicit roll and Camera clamps pitch. */
struct ReflectionProbeCaptureFace final {
    Vec3 direction;
    Vec3 up;
};

class ReflectionProbeCapture final {
public:
    static constexpr std::uint32_t FaceCount = 6;
    static constexpr std::uint32_t DefaultResolution = 256;

    [[nodiscard]] static constexpr std::array<ReflectionProbeCaptureFace, FaceCount> faces() noexcept {
        return {{{ {1.0F, 0.0F, 0.0F}, {0.0F, -1.0F, 0.0F} },
                 { {-1.0F, 0.0F, 0.0F}, {0.0F, -1.0F, 0.0F} },
                 { {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F} },
                 { {0.0F, -1.0F, 0.0F}, {0.0F, 0.0F, -1.0F} },
                 { {0.0F, 0.0F, 1.0F}, {0.0F, -1.0F, 0.0F} },
                 { {0.0F, 0.0F, -1.0F}, {0.0F, -1.0F, 0.0F} }}};
    }

    [[nodiscard]] static Mat4 viewMatrix(const Vec3& position, std::uint32_t face);
    [[nodiscard]] static Mat4 projectionMatrix(float nearPlane, float farPlane);
    [[nodiscard]] static std::uint32_t fullMipCount(std::uint32_t faceSize) noexcept;
};

} // namespace Engine
