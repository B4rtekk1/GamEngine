#pragma once

#include <cstdint>

namespace Engine {

    enum class MouseButton : std::uint8_t {
        Left = 0,
        Middle,
        Right,

        X1,
        X2,

        Count
    };

}