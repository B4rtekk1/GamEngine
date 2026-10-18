#pragma once

#include "Engine/Input/CursorMode.h"

#include <SDL3/SDL.h>

namespace Engine {

class SDLInput {
  public:
    static void processEvent(const SDL_Event& event);

    static void setWindow(SDL_Window* window);
    static bool setCursorMode(CursorMode mode);
    static CursorMode cursorMode();
};
} // namespace Engine
