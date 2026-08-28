#pragma once

/**
 * @file MouseButton.h
 * @brief Defines the engine-independent mouse button identifiers.
 */

#include <cstdint>

namespace Engine {
    /**
     * @brief Identifies a mouse button supported by the input system.
     *
     * The values are independent of platform-specific mouse button codes.
     */
    enum class MouseButton : std::uint8_t {
        /// Primary mouse button.
        Left = 0,
        /// Middle mouse button, commonly the wheel button.
        Middle,
        /// Secondary mouse button.
        Right,

        /// First auxiliary mouse button.
        X1,
        /// Second auxiliary mouse button.
        X2,

        /// Number of defined mouse button identifiers.
        Count,
    };
}
