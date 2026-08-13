#include "Engine/Input/Input.h"

#include <array>
#include <algorithm>

namespace Engine {
    namespace {

        constexpr std::size_t KEY_COUNT = static_cast<std::size_t>(KeyCode::Count);
        constexpr std::size_t MOUSE_COUNT = static_cast<std::size_t>(MouseButton::Count);

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

        Vec2 currentMousePosition{};
        Vec2 frameMouseDelta{};

        float frameMouseWheel = 0.0f;
    }

    void Input::beginFrame() {
        previousKeys = currentKeys;
        previousMouseButtons = currentMouseButtons;

        frameMouseDelta = {0.0f, 0.0f};
        frameMouseWheel = 0.0f;
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
}
