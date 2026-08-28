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
        Vec2 anchorMin{0.0F, 0.0F};
        Vec2 anchorMax{0.0F, 0.0F};

        Vec2 pivot{0.5F, 0.5F}; //NOLINT

        Vec2 offsetMin{0.0F, 0.0F};
        Vec2 offsetMax{100.0F, 100.0F};

        Rect calculatedRect{};

        void calculate(const Rect &parentRect);
    };
}
