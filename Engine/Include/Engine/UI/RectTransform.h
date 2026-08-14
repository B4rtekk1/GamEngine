#pragma once

#include <Engine/Math/Vec2.h>

namespace Engine::UI {

    struct Rect {
        float x{};
        float y{};
        float width{};
        float height{};
    };

    struct RectTransform {
        Vec2 anchorMin{0.0f, 0.0f};
        Vec2 anchorMax{0.0f, 0.0f};

        Vec2 pivot{0.5f, 0.5f};

        Vec2 offsetMin{0.0f, 0.0f};
        Vec2 offsetMax{100.0f, 100.0f};

        Rect calculatedRect{};

        void calculate(const Rect& parentRect);
    };

}