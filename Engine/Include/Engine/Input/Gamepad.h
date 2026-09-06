#pragma once

#include <cstdint>

namespace Engine {
    /** Engine-independent gamepad controls. Player zero is the primary pad. */
    enum class GamepadButton : std::uint8_t {
        South, East, West, North, Back, Guide, Start, LeftStick, RightStick,
        LeftShoulder, RightShoulder, DPadUp, DPadDown, DPadLeft, DPadRight, Count,
    };
    enum class GamepadAxis : std::uint8_t {
        LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count,
    };
}
