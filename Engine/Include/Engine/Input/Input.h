#pragma once

/**
 * @file Input.h
 * @brief Declares the frame-based keyboard and mouse input interface.
 */

#include "Engine/Input/KeyCode.h"
#include "Engine/Input/MouseButton.h"
#include "Engine/Math/Vec2.h"

namespace Engine {
    /**
     * @brief Provides the current and per-frame input state.
     *
     * The platform-specific input backend updates this class during event
     * processing. Call @ref beginFrame once at the beginning of each frame to
     * clear transient states such as key presses, releases and mouse delta.
     */
    class Input {
    public:
        /**
         * @brief Begins a new input frame.
         *
         * Resets frame-transient state while preserving keys and buttons that
         * remain held down.
         */
        static void beginFrame();

        /** @brief Checks whether a key is currently held down. */
        static bool keyDown(KeyCode key);

        /** @brief Checks whether a key was pressed during the current frame. */
        static bool keyPressed(KeyCode key);

        /** @brief Checks whether a key was released during the current frame. */
        static bool keyReleased(KeyCode key);

        /** @brief Checks whether a mouse button is currently held down. */
        static bool mouseDown(MouseButton button);

        /** @brief Checks whether a mouse button was pressed this frame. */
        static bool mousePressed(MouseButton button);

        /** @brief Checks whether a mouse button was released this frame. */
        static bool mouseReleased(MouseButton button);

        /** @brief Returns the current mouse position in window coordinates. */
        static Vec2 mousePosition();

        /** @brief Returns the mouse movement accumulated during the current frame. */
        static Vec2 mouseDelta();

        /** @brief Returns the mouse-wheel movement accumulated during the current frame. */
        static float mouseWheel();

    private:
        friend class SDLInput;

        static void setKey(KeyCode key, bool down);

        static void setMouseButton(MouseButton button, bool down);

        static void setMousePosition(float x, float y); //NOLINT

        static void addMouseDelta(float x, float y); //NOLINT

        static void addMouseWheel(float value);
    };
}
