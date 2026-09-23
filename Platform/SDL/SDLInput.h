#pragma once

#include <SDL3/SDL.h>
#include "Engine/Input/CursorMode.h"

namespace Engine {

    class SDLInput {
    public:
        static void processEvent(const SDL_Event& event);

        static void setWindow(SDL_Window* window);
        static bool setCursorMode(CursorMode mode);
        static CursorMode cursorMode();
    };
}
