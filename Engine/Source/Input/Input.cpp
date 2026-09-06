#include "Engine/Input/Input.h"
#include "Engine/Input/InputMap.h"

#include <array>
#include <algorithm>

// NOLINTBEGIN(readability-identifier-length)

namespace Engine {
    namespace {

        constexpr std::size_t KEY_COUNT = static_cast<std::size_t>(KeyCode::Count);
        constexpr std::size_t MOUSE_COUNT = static_cast<std::size_t>(MouseButton::Count);
        constexpr std::size_t GAMEPAD_BUTTON_COUNT = static_cast<std::size_t>(GamepadButton::Count);
        constexpr std::size_t GAMEPAD_AXIS_COUNT = static_cast<std::size_t>(GamepadAxis::Count);

        constexpr bool isValidKey(const KeyCode key) {
            return static_cast<std::size_t>(key) < KEY_COUNT;
        }

        constexpr bool isValidMouseButton(const MouseButton button) {
            return static_cast<std::size_t>(button) < MOUSE_COUNT;
        }

        std::array<bool, KEY_COUNT> currentKeys{};
        std::array<bool, KEY_COUNT> previousKeys{};

        std::array<bool, MOUSE_COUNT> currentMouseButtons{};
        std::array<bool, MOUSE_COUNT> previousMouseButtons{};
        std::array<bool, GAMEPAD_BUTTON_COUNT> currentGamepadButtons{};
        std::array<bool, GAMEPAD_BUTTON_COUNT> previousGamepadButtons{};
        std::array<float, GAMEPAD_AXIS_COUNT> currentGamepadAxes{};

        Vec2 currentMousePosition{};
        Vec2 frameMouseDelta{};

        float frameMouseWheel = 0.0F;

        constexpr bool isValidGamepadButton(const GamepadButton button) { return static_cast<std::size_t>(button) < GAMEPAD_BUTTON_COUNT; }
        constexpr bool isValidGamepadAxis(const GamepadAxis axis) { return static_cast<std::size_t>(axis) < GAMEPAD_AXIS_COUNT; }
    }

    void Input::beginFrame() {
        previousKeys = currentKeys;
        previousMouseButtons = currentMouseButtons;
        previousGamepadButtons = currentGamepadButtons;

        frameMouseDelta = {0.0F, 0.0F};
        frameMouseWheel = 0.0F;
    }

    bool Input::keyDown(KeyCode key) {
        return isValidKey(key) && currentKeys[static_cast<std::size_t>(key)];
    }

    bool Input::keyPressed(KeyCode key) {
        const auto index =
            static_cast<std::size_t>(key);

        return isValidKey(key) && currentKeys[index] &&
               !previousKeys[index];
    }

    bool Input::keyReleased(KeyCode key) {
        const auto index =
            static_cast<std::size_t>(key);

        return isValidKey(key) && !currentKeys[index] &&
               previousKeys[index];
    }

    bool Input::mouseDown(MouseButton button) {
        return isValidMouseButton(button) && currentMouseButtons[static_cast<std::size_t>(button)];
    }

    bool Input::mousePressed(MouseButton button) {
        const auto index =
            static_cast<std::size_t>(button);

        return isValidMouseButton(button) && currentMouseButtons[index] &&
               !previousMouseButtons[index];
    }

    bool Input::mouseReleased(MouseButton button) {
        const auto index =
            static_cast<std::size_t>(button);

        return isValidMouseButton(button) && !currentMouseButtons[index] &&
               previousMouseButtons[index];
    }

    Vec2 Input::mousePosition() {
        return currentMousePosition;
    }

    Vec2 Input::mouseDelta() {
        return frameMouseDelta;
    }

    float Input::mouseWheel() {
        return frameMouseWheel;
    }

    bool Input::gamepadDown(const GamepadButton button) { return isValidGamepadButton(button) && currentGamepadButtons[static_cast<std::size_t>(button)]; }
    bool Input::gamepadPressed(const GamepadButton button) { const auto i = static_cast<std::size_t>(button); return isValidGamepadButton(button) && currentGamepadButtons[i] && !previousGamepadButtons[i]; }
    bool Input::gamepadReleased(const GamepadButton button) { const auto i = static_cast<std::size_t>(button); return isValidGamepadButton(button) && !currentGamepadButtons[i] && previousGamepadButtons[i]; }
    float Input::gamepadAxis(const GamepadAxis axis) { return isValidGamepadAxis(axis) ? currentGamepadAxes[static_cast<std::size_t>(axis)] : 0.0F; }
    bool Input::actionDown(const std::string_view action) { return InputMap::actionDown(action); }
    bool Input::actionPressed(const std::string_view action) { return InputMap::actionPressed(action); }
    bool Input::actionReleased(const std::string_view action) { return InputMap::actionReleased(action); }
    Vec2 Input::axis2D(const std::string_view axis) { return InputMap::axis2D(axis); }

    void Input::setKey(KeyCode key, bool down) {
        if (!isValidKey(key) || key == KeyCode::Unknown)
            return;

        currentKeys[
            static_cast<std::size_t>(key)
        ] = down;
    }

    void Input::setMouseButton(
        MouseButton button,
        bool down
    ) {
        if (!isValidMouseButton(button)) {
            return;
        }

        currentMouseButtons[
            static_cast<std::size_t>(button)
        ] = down;
    }

    void Input::setMousePosition(float x, float y) {
        currentMousePosition = {x, y};
    }

    void Input::addMouseDelta(float x, float y) {
        frameMouseDelta += Vec2{x, y};
    }

    void Input::addMouseWheel(float value) {
        frameMouseWheel += value;
    }

    void Input::setGamepadButton(const GamepadButton button, const bool down) {
        if (isValidGamepadButton(button)) currentGamepadButtons[static_cast<std::size_t>(button)] = down;
    }

    void Input::setGamepadAxis(const GamepadAxis axis, const float value) {
        if (isValidGamepadAxis(axis)) currentGamepadAxes[static_cast<std::size_t>(axis)] = value;
    }
}

// NOLINTEND(readability-identifier-length)
