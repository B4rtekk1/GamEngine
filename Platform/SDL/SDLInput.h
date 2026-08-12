#pragma once

#include <SDL3/SDL.h>

namespace Engine {

    class SDLInput {
    public:
        static void processEvent(const SDL_Event& event);

        static void setRelativeMouseMode(SDL_Window* window, bool enabled);
    };
}