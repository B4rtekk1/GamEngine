#include <Engine/ECS/Components/TransformComponent.h>

#include <cmath>

namespace {
bool near(const float a, const float b, const float epsilon = 0.001f) {
    return std::abs(a - b) <= epsilon;
}
}

int main() {
    using namespace Engine;

    TransformComponent identity;
    const Vec4 origin = identity.matrix() * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    const Vec4 xAxis = identity.matrix() * Vec4{1.0f, 0.0f, 0.0f, 1.0f};
    if (!near(origin.x(), 0.0f) || !near(origin.y(), 0.0f) || !near(origin.z(), 0.0f) ||
        !near(xAxis.x(), 1.0f) || !near(xAxis.y(), 0.0f) || !near(xAxis.z(), 0.0f) ||
        identity.scale.x() != 1.0f || identity.scale.y() != 1.0f || identity.scale.z() != 1.0f) return 1;

    TransformComponent transform{
        .position = {10.0f, 20.0f, 30.0f},
        .rotation = {0.0f, 0.0f, 90.0f},
        .scale = {2.0f, 3.0f, 4.0f},
    };
    const Mat4 matrix = transform.matrix();
    const Vec4 transformedOrigin = matrix * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    const Vec4 transformedX = matrix * Vec4{1.0f, 0.0f, 0.0f, 1.0f};
    const Vec4 transformedY = matrix * Vec4{0.0f, 1.0f, 0.0f, 1.0f};
    if (!near(transformedOrigin.x(), 10.0f) || !near(transformedOrigin.y(), 20.0f) ||
        !near(transformedOrigin.z(), 30.0f) || !near(transformedX.x(), 10.0f) ||
        !near(transformedX.y(), 22.0f) || !near(transformedY.x(), 7.0f) ||
        !near(transformedY.y(), 20.0f)) return 2;

    transform.position = {-1.0f, -2.0f, -3.0f};
    transform.rotation = {};
    transform.scale = {0.5f, 0.25f, 2.0f};
    const Vec4 scaled = transform.matrix() * Vec4{4.0f, 8.0f, 1.5f, 1.0f};
    if (!near(scaled.x(), 1.0f) || !near(scaled.y(), 0.0f) || !near(scaled.z(), 0.0f)) return 3;
    return 0;
}
