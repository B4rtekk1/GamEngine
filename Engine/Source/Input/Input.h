#pragma once

#include "KeyCode.h"
#include "MouseButton.h"
#include "Engine/Math/Vec2.h"

namespace Engine {

    class Input {
    public:
        static void beginFrame();

        static bool keyDown(KeyCode key);
        static bool keyPressed(KeyCode key);
        static bool keyReleased(KeyCode key);

        static bool mouseDown(MouseButton button);
        static bool mousePressed(MouseButton button);
        static bool mouseReleased(MouseButton button);

        static Vec2 mousePosition();
        static Vec2 mouseDelta();

        static float mouseWheel();

    private:
        friend class SDLInput;

        static void setKey(KeyCode key, bool down);
        static void setMouseButton(MouseButton button, bool down);

        static void setMousePosition(float x, float y);
        static void addMouseDelta(float x, float y);
        static void addMouseWheel(float value);
    };

}