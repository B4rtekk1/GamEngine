#pragma once

#include <Engine/Math/Vec2.h>
#include <Engine/Math/Vec4.h>

namespace Engine::UI {
    struct UIVertex {
        Vec2 position;
        Vec2 uv;
        Vec4 color;
    };
}