#pragma once

#include <Engine/Math/Vec2.h>
#include <Engine/Math/Color.h>

namespace Engine::UI {
    struct UIVertex {
        Vec2 position;
        Vec2 uv;
        Math::Color color;
    };
}
