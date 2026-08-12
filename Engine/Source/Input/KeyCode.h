#pragma once

#include <cstdint>

namespace Engine {

    enum class KeyCode : std::uint16_t {
        Unknown = 0,

        A, B, C, D, E, F, G,
        H, I, J, K, L, M, N,
        O, P, Q, R, S, T, U,
        V, W, X, Y, Z,

        Num0, Num1, Num2, Num3, Num4,
        Num5, Num6, Num7, Num8, Num9,

        Space,
        Escape,
        Enter,
        Tab,
        Backspace,

        LeftShift,
        RightShift,

        LeftControl,
        RightControl,

        LeftAlt,
        RightAlt,

        Up,
        Down,
        Left,
        Right,

        Count
    };

}