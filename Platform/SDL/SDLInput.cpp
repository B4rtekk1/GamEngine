#include "SDLInput.h"

#include "Input/Input.h"

namespace Engine {
    namespace {

    KeyCode toKeyCode(const SDL_Scancode key) {
        switch (key) {
            case SDL_SCANCODE_A: return KeyCode::A;
            case SDL_SCANCODE_B: return KeyCode::B;
            case SDL_SCANCODE_C: return KeyCode::C;
            case SDL_SCANCODE_D: return KeyCode::D;
            case SDL_SCANCODE_E: return KeyCode::E;
            case SDL_SCANCODE_F: return KeyCode::F;
            case SDL_SCANCODE_G: return KeyCode::G;
            case SDL_SCANCODE_H: return KeyCode::H;
            case SDL_SCANCODE_I: return KeyCode::I;
            case SDL_SCANCODE_J: return KeyCode::J;
            case SDL_SCANCODE_K: return KeyCode::K;
            case SDL_SCANCODE_L: return KeyCode::L;
            case SDL_SCANCODE_M: return KeyCode::M;
            case SDL_SCANCODE_N: return KeyCode::N;
            case SDL_SCANCODE_O: return KeyCode::O;
            case SDL_SCANCODE_P: return KeyCode::P;
            case SDL_SCANCODE_W: return KeyCode::W;
            case SDL_SCANCODE_S: return KeyCode::S;
            case SDL_SCANCODE_Q: return KeyCode::Q;
            case SDL_SCANCODE_R: return KeyCode::R;
            case SDL_SCANCODE_T: return KeyCode::T;
            case SDL_SCANCODE_U: return KeyCode::U;
            case SDL_SCANCODE_V: return KeyCode::V;
            case SDL_SCANCODE_X: return KeyCode::X;
            case SDL_SCANCODE_Y: return KeyCode::Y;
            case SDL_SCANCODE_Z: return KeyCode::Z;

            case SDL_SCANCODE_0: return KeyCode::Num0;
            case SDL_SCANCODE_1: return KeyCode::Num1;
            case SDL_SCANCODE_2: return KeyCode::Num2;
            case SDL_SCANCODE_3: return KeyCode::Num3;
            case SDL_SCANCODE_4: return KeyCode::Num4;
            case SDL_SCANCODE_5: return KeyCode::Num5;
            case SDL_SCANCODE_6: return KeyCode::Num6;
            case SDL_SCANCODE_7: return KeyCode::Num7;
            case SDL_SCANCODE_8: return KeyCode::Num8;
            case SDL_SCANCODE_9: return KeyCode::Num9;

            case SDL_SCANCODE_SPACE:
                return KeyCode::Space;

            case SDL_SCANCODE_ESCAPE:
                return KeyCode::Escape;

            case SDL_SCANCODE_RETURN:
                return KeyCode::Enter;

            case SDL_SCANCODE_TAB:
                return KeyCode::Tab;

            case SDL_SCANCODE_BACKSPACE:
                return KeyCode::Backspace;

            case SDL_SCANCODE_LSHIFT:
                return KeyCode::LeftShift;

            case SDL_SCANCODE_RSHIFT:
                return KeyCode::RightShift;

            case SDL_SCANCODE_LCTRL:
                return KeyCode::LeftControl;

            case SDL_SCANCODE_RCTRL:
                return KeyCode::RightControl;

            case SDL_SCANCODE_LALT:
                return KeyCode::LeftAlt;

            case SDL_SCANCODE_RALT:
                return KeyCode::RightAlt;

            case SDL_SCANCODE_UP:
                return KeyCode::Up;

            case SDL_SCANCODE_DOWN:
                return KeyCode::Down;

            case SDL_SCANCODE_LEFT:
                return KeyCode::Left;

            case SDL_SCANCODE_RIGHT:
                return KeyCode::Right;

            default:
                return KeyCode::Unknown;
        }
    }

    bool toMouseButton(Uint8 button, MouseButton& result) {
        switch (button) {
            case SDL_BUTTON_LEFT:   result = MouseButton::Left; return true;
            case SDL_BUTTON_MIDDLE: result = MouseButton::Middle; return true;
            case SDL_BUTTON_RIGHT:  result = MouseButton::Right; return true;
            case SDL_BUTTON_X1:     result = MouseButton::X1; return true;
            case SDL_BUTTON_X2:     result = MouseButton::X2; return true;
            default: return false;
        }
    }

    } // namespace

    void SDLInput::processEvent(const SDL_Event& event) {
        switch (event.type) {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                Input::setKey(toKeyCode(event.key.scancode), event.key.down);
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                MouseButton button{};
                if (toMouseButton(event.button.button, button)) {
                    Input::setMouseButton(button, event.button.down);
                }
                Input::setMousePosition(event.button.x, event.button.y);
                break;
            }

            case SDL_EVENT_MOUSE_MOTION:
                Input::setMousePosition(event.motion.x, event.motion.y);
                Input::addMouseDelta(event.motion.xrel, event.motion.yrel);
                break;

            case SDL_EVENT_MOUSE_WHEEL: {
                const float direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
                Input::addMouseWheel(event.wheel.y * direction);
                break;
            }

            default:
                break;
        }
    }

    void SDLInput::setRelativeMouseMode(SDL_Window* window, bool enabled) {
        SDL_SetWindowRelativeMouseMode(window, enabled);
    }
}
