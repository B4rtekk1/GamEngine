#pragma once

#include "Engine/Input/Gamepad.h"
#include "Engine/Input/KeyCode.h"
#include "Engine/Input/MouseButton.h"
#include "Engine/Math/Vec2.h"

#include <string_view>

namespace Engine {
    /** Configurable mapping from physical controls to gameplay actions. */
    class InputMap final {
    public:
        static void clear();
        static void removeAction(std::string_view action);
        static void bindAction(std::string_view action, KeyCode key);
        static void bindAction(std::string_view action, MouseButton button);
        static void bindAction(std::string_view action, GamepadButton button);
        static void bindAxis2D(std::string_view axis, KeyCode negativeX, KeyCode positiveX,
                               KeyCode negativeY, KeyCode positiveY);
        static void bindAxis2D(std::string_view axis, GamepadAxis x, GamepadAxis y,
                               float deadZone = 0.15F);
        static void bindMouseDelta(std::string_view axis, Vec2 scale = {1.0F, 1.0F});
        [[nodiscard]] static bool actionDown(std::string_view action);
        [[nodiscard]] static bool actionPressed(std::string_view action);
        [[nodiscard]] static bool actionReleased(std::string_view action);
        [[nodiscard]] static Vec2 axis2D(std::string_view axis);
    };
}
