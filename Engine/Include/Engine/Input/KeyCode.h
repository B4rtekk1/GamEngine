#pragma once

/**
 * @file KeyCode.h
 * @brief Defines the engine-independent keyboard key identifiers.
 */

#include <cstdint>

namespace Engine {
    /**
     * @brief Identifies a keyboard key supported by the input system.
     *
     * The values are engine-level identifiers and are independent of the
     * platform-specific key codes reported by the windowing backend.
     */
    enum class KeyCode : std::uint8_t {
        /// Invalid or unsupported key.
        Unknown = 0,

        /// Alphabetic keys.
        A, B, C, D, E, F, G,
        H, I, J, K, L, M, N,
        O, P, Q, R, S, T, U,
        V, W, X, Y, Z,

        /// Number-row keys.
        Num0, Num1, Num2, Num3, Num4,
        Num5, Num6, Num7, Num8, Num9,

        /// Common editing and control keys.
        Space,
        Escape,
        Enter,
        Tab,
        Backspace,
        Delete,

        /// Modifier keys.
        LeftShift,
        RightShift,

        LeftControl,
        RightControl,

        LeftAlt,
        RightAlt,

        /// Arrow keys.
        Up,
        Down,
        Left,
        Right,

        /// Number of defined key identifiers.
        Count,
    };
}
