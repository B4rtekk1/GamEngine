#include <Engine/Math/Frustum.h>

int main() {
    using namespace Engine;

    const Frustum frustum{Mat4{}};
    if (!frustum.intersects(AABB{{-0.5F, -0.5F, 0.1F}, {0.5F, 0.5F, 0.9F}})) return 1;
    if (frustum.intersects(AABB{{2.0F, -0.5F, 0.1F}, {3.0F, 0.5F, 0.9F}}) ||
        frustum.intersects(AABB{{-0.5F, 2.0F, 0.1F}, {0.5F, 3.0F, 0.9F}}) ||
        frustum.intersects(AABB{{-0.5F, -0.5F, -2.0F}, {0.5F, 0.5F, -1.0F}})) return 2;
    if (!frustum.intersects(AABB{{0.9F, 0.9F, 0.9F}, {1.1F, 1.1F, 1.1F}})) return 3;

    const AABB unit = AABB::unitCube();
    const auto translated = unit.transformed(Mat4::translate({5.0F, 0.0F, 0.0F}).native());
    if (translated.min.x() != 4.5F || translated.max.x() != 5.5F ||
        translated.min.y() != -0.5F || translated.max.y() != 0.5F) return 4;
    return 0;
}