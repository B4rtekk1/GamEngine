#include "Engine/Renderer/Lighting/ReflectionProbeCapture.h"

#include "Engine/Math/Math.h"

#include <bit>
#include <stdexcept>

namespace Engine {

Mat4 ReflectionProbeCapture::viewMatrix(const Vec3& position, const std::uint32_t face) {
    if (face >= FaceCount) throw std::out_of_range("Reflection-probe cubemap face is out of range");
    const ReflectionProbeCaptureFace camera = faces()[face];
    return Mat4::lookAt(position, position + camera.direction, camera.up);
}

Mat4 ReflectionProbeCapture::projectionMatrix(const float nearPlane, const float farPlane) {
    if (nearPlane <= 0.0F || farPlane <= nearPlane)
        throw std::invalid_argument("Reflection-probe capture clip planes are invalid");
    Mat4 projection = Mat4::perspective(Radians{Degrees{90.0F}}, 1.0F, nearPlane, farPlane);
    projection.native()[1][1] *= -1.0F;
    return projection;
}

std::uint32_t ReflectionProbeCapture::fullMipCount(const std::uint32_t faceSize) noexcept {
    return faceSize == 0 ? 0U : std::bit_width(faceSize);
}

} // namespace Engine
