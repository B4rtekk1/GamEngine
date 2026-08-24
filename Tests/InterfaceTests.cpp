#include <Engine/Input/Input.h>
#include <Engine/UI/Interface.h>

#include "../Platform/SDL/SDLInput.h"

int main() {
    using namespace Engine;
    using namespace Engine::UI;

    Canvas canvas{400, 300};
    UIFontAtlas atlas;
    Interface ui{canvas, atlas};
    int clicks = 0;
    auto& button = ui.button("Play", [&clicks] { ++clicks; },
                             ElementOptions{.rect = {50.0f, 60.0f, 100.0f, 40.0f},
                                            .color = Math::Color::green()});
    auto& panel = ui.panel("Background", ElementOptions{.rect = {0.0f, 0.0f, 400.0f, 300.0f}});
    if (canvas.size() != 2 || button.children().size() != 1 ||
        button.rectTransform.calculatedRect.x != 50.0f ||
        button.rectTransform.calculatedRect.y != 60.0f ||
        button.rectTransform.calculatedRect.width != 100.0f ||
        button.rectTransform.calculatedRect.height != 40.0f ||
        panel.rectTransform.calculatedRect.width != 400.0f) return 1;

    Input::beginFrame();
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = 75.0f;
    event.motion.y = 80.0f;
    event.motion.xrel = 0.0f;
    event.motion.yrel = 0.0f;
    Engine::SDLInput::processEvent(event);
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = true;
    event.button.x = 75.0f;
    event.button.y = 80.0f;
    Engine::SDLInput::processEvent(event);
    ui.update();
    if (clicks != 1) return 2;

    Input::beginFrame();
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = 10.0f;
    event.motion.y = 10.0f;
    event.motion.xrel = 0.0f;
    event.motion.yrel = 0.0f;
    Engine::SDLInput::processEvent(event);
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.x = 10.0f;
    event.button.y = 10.0f;
    Engine::SDLInput::processEvent(event);
    ui.update();
    if (clicks != 1) return 3;

    button.visible = false;
    Input::beginFrame();
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.x = 75.0f;
    event.button.y = 80.0f;
    Engine::SDLInput::processEvent(event);
    ui.update();
    if (clicks != 1) return 4;
    return 0;
}
