#pragma once

#include <Engine/Math/Math.h>
#include <Engine/Renderer/mesh.h>

namespace Engine {

// Renderable scene entity with local transform and geometry.
class GameObject {
public:
    virtual ~GameObject() = default;

    Mesh mesh;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    bool castShadow{true};

    [[nodiscard]] Mat4 modelMatrix() const noexcept {
        return Mat4::translate(position) *
               Mat4::rotate(rotation) *
               Mat4::scale(Mat4{}, scale);
    }
};

} // namespace Engine
