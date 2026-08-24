#include <Engine/Math/Frustum.h>

int main() {
    using namespace Engine;

    const Frustum frustum{Mat4{}};
    if (!frustum.intersects(AABB{{-0.5f, -0.5f, 0.1f}, {0.5f, 0.5f, 0.9f}})) return 1;
    if (frustum.intersects(AABB{{2.0f, -0.5f, 0.1f}, {3.0f, 0.5f, 0.9f}}) ||
        frustum.intersects(AABB{{-0.5f, 2.0f, 0.1f}, {0.5f, 3.0f, 0.9f}}) ||
        frustum.intersects(AABB{{-0.5f, -0.5f, -2.0f}, {0.5f, 0.5f, -1.0f}})) return 2;
    if (!frustum.intersects(AABB{{0.9f, 0.9f, 0.9f}, {1.1f, 1.1f, 1.1f}})) return 3;

    const AABB unit = AABB::unitCube();
    const auto translated = unit.transformed(Mat4::translate({5.0f, 0.0f, 0.0f}).native());
    if (translated.min.x() != 4.5f || translated.max.x() != 5.5f ||
        translated.min.y() != -0.5f || translated.max.y() != 0.5f) return 4;
    return 0;
}
