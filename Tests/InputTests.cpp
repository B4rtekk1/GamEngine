#include <Engine/Input/Input.h>

#include "../Platform/SDL/SDLInput.h"

#include <cmath>

int main() {
    using namespace Engine;

    Input::beginFrame();
    if (Input::keyDown(KeyCode::A) || Input::keyPressed(KeyCode::A) ||
        Input::mouseDown(MouseButton::Left) || Input::mouseWheel() != 0.0f) return 1;

    SDL_Event key{};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.scancode = SDL_SCANCODE_A;
    key.key.down = true;
    SDLInput::processEvent(key);
    if (!Input::keyDown(KeyCode::A) || !Input::keyPressed(KeyCode::A) ||
        Input::keyReleased(KeyCode::A)) return 2;

    Input::beginFrame();
    if (!Input::keyDown(KeyCode::A) || Input::keyPressed(KeyCode::A) ||
        Input::keyReleased(KeyCode::A)) return 3;
    key.type = SDL_EVENT_KEY_UP;
    key.key.down = false;
    SDLInput::processEvent(key);
    if (Input::keyDown(KeyCode::A) || !Input::keyReleased(KeyCode::A)) return 4;

    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.x = 100.0f;
    motion.motion.y = 50.0f;
    motion.motion.xrel = 7.0f;
    motion.motion.yrel = -3.0f;
    SDLInput::processEvent(motion);
    if (Input::mousePosition().x() != 100.0f || Input::mousePosition().y() != 50.0f ||
        Input::mouseDelta().x() != 7.0f || Input::mouseDelta().y() != -3.0f) return 5;

    SDL_Event button{};
    button.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    button.button.button = SDL_BUTTON_LEFT;
    button.button.down = true;
    button.button.x = 120.0f;
    button.button.y = 80.0f;
    SDLInput::processEvent(button);
    if (!Input::mouseDown(MouseButton::Left) || !Input::mousePressed(MouseButton::Left) ||
        Input::mousePosition().x() != 120.0f || Input::mousePosition().y() != 80.0f) return 6;

    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.y = 2.0f;
    wheel.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    SDLInput::processEvent(wheel);
    wheel.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    wheel.wheel.y = 1.0f;
    SDLInput::processEvent(wheel);
    if (Input::mouseWheel() != 1.0f) return 7;

    Input::beginFrame();
    if (Input::keyReleased(KeyCode::A) || Input::mouseDelta().x() != 0.0f ||
        Input::mouseDelta().y() != 0.0f || Input::mouseWheel() != 0.0f ||
        !Input::mouseDown(MouseButton::Left) || Input::mousePressed(MouseButton::Left)) return 8;
    return 0;
}
