#include <Engine/ECS/Components/TransformComponent.h>

#include <cmath>

namespace {
bool near(const float a, const float b, const float epsilon = 0.001F) {
    return std::abs(a - b) <= epsilon;
}
}

int main() {
    using namespace Engine;

    TransformComponent identity;
    const Vec4 origin = identity.matrix() * Vec4{0.0F, 0.0F, 0.0F, 1.0F};
    const Vec4 xAxis = identity.matrix() * Vec4{1.0F, 0.0F, 0.0F, 1.0F};
    if (!near(origin.x(), 0.0F) || !near(origin.y(), 0.0F) || !near(origin.z(), 0.0F) ||
        !near(xAxis.x(), 1.0F) || !near(xAxis.y(), 0.0F) || !near(xAxis.z(), 0.0F) ||
        identity.scale.x() != 1.0F || identity.scale.y() != 1.0F || identity.scale.z() != 1.0F) return 1;

    TransformComponent transform{
        .position = {10.0F, 20.0F, 30.0F},
        .rotation = {0.0F, 0.0F, 90.0F},
        .scale = {2.0F, 3.0F, 4.0F},
    };
    const Mat4 matrix = transform.matrix();
    const Vec4 transformedOrigin = matrix * Vec4{0.0F, 0.0F, 0.0F, 1.0F};
    const Vec4 transformedX = matrix * Vec4{1.0F, 0.0F, 0.0F, 1.0F};
    const Vec4 transformedY = matrix * Vec4{0.0F, 1.0F, 0.0F, 1.0F};
    if (!near(transformedOrigin.x(), 10.0F) || !near(transformedOrigin.y(), 20.0F) ||
        !near(transformedOrigin.z(), 30.0F) || !near(transformedX.x(), 10.0F) ||
        !near(transformedX.y(), 22.0F) || !near(transformedY.x(), 7.0F) ||
        !near(transformedY.y(), 20.0F)) return 2;

    transform.position = {-1.0F, -2.0F, -3.0F};
    transform.rotation = {};
    transform.scale = {0.5F, 0.25F, 2.0F};
    const Vec4 scaled = transform.matrix() * Vec4{4.0F, 8.0F, 1.5F, 1.0F};
    if (!near(scaled.x(), 1.0F) || !near(scaled.y(), 0.0F) || !near(scaled.z(), 0.0F)) return 3;
    return 0;
}