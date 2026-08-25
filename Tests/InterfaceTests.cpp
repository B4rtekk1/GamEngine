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
                             ElementOptions{.rect = {50.0F, 60.0F, 100.0F, 40.0F},
                                            .color = Math::Color::green()});
    auto& panel = ui.panel("Background", ElementOptions{.rect = {0.0F, 0.0F, 400.0F, 300.0F}});
    if (canvas.size() != 2 || button.children().size() != 1 ||
        button.rectTransform.calculatedRect.x != 50.0F ||
        button.rectTransform.calculatedRect.y != 60.0F ||
        button.rectTransform.calculatedRect.width != 100.0F ||
        button.rectTransform.calculatedRect.height != 40.0F ||
        panel.rectTransform.calculatedRect.width != 400.0F) return 1;

    Input::beginFrame();
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = 75.0F;
    event.motion.y = 80.0F;
    event.motion.xrel = 0.0F;
    event.motion.yrel = 0.0F;
    Engine::SDLInput::processEvent(event);
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = true;
    event.button.x = 75.0F;
    event.button.y = 80.0F;
    Engine::SDLInput::processEvent(event);
    ui.update();
    if (clicks != 1) return 2;

    Input::beginFrame();
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = 10.0F;
    event.motion.y = 10.0F;
    event.motion.xrel = 0.0F;
    event.motion.yrel = 0.0F;
    Engine::SDLInput::processEvent(event);
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.x = 10.0F;
    event.button.y = 10.0F;
    Engine::SDLInput::processEvent(event);
    ui.update();
    if (clicks != 1) return 3;

    button.visible = false;
    Input::beginFrame();
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.x = 75.0F;
    event.button.y = 80.0F;
    Engine::SDLInput::processEvent(event);
    ui.update();
    if (clicks != 1) return 4;
    return 0;
}