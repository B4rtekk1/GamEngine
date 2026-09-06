#include "Engine/Input/InputMap.h"

#include "Engine/Input/Input.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Engine {
    namespace {
        struct KeyBinding { KeyCode key; };
        struct MouseBinding { MouseButton button; };
        struct PadBinding { GamepadButton button; };
        using ActionBinding = std::variant<KeyBinding, MouseBinding, PadBinding>;
        struct DigitalAxis { KeyCode nx, px, ny, py; };
        struct StickAxis { GamepadAxis x, y; float deadZone; };
        struct MouseAxis { Vec2 scale; };
        using AxisBinding = std::variant<DigitalAxis, StickAxis, MouseAxis>;
        std::unordered_map<std::string, std::vector<ActionBinding>> actions;
        std::unordered_map<std::string, std::vector<AxisBinding>> axes;
        bool defaultsInstalled = false;

        bool evaluate(const ActionBinding &binding, const int phase) {
            return std::visit([phase](const auto &control) {
                using T = std::decay_t<decltype(control)>;
                if constexpr (std::is_same_v<T, KeyBinding>) {
                    return phase == 0 ? Input::keyDown(control.key) : phase == 1 ? Input::keyPressed(control.key) : Input::keyReleased(control.key);
                } else if constexpr (std::is_same_v<T, MouseBinding>) {
                    return phase == 0 ? Input::mouseDown(control.button) : phase == 1 ? Input::mousePressed(control.button) : Input::mouseReleased(control.button);
                } else {
                    return phase == 0 ? Input::gamepadDown(control.button) : phase == 1 ? Input::gamepadPressed(control.button) : Input::gamepadReleased(control.button);
                }
            }, binding);
        }
        void installDefaults() {
            if (defaultsInstalled || !actions.empty() || !axes.empty()) return;
            defaultsInstalled = true;
            InputMap::bindAxis2D("Move", KeyCode::A, KeyCode::D, KeyCode::S, KeyCode::W);
            InputMap::bindAxis2D("Move", GamepadAxis::LeftX, GamepadAxis::LeftY);
            InputMap::bindMouseDelta("Look");
            InputMap::bindAxis2D("Look", GamepadAxis::RightX, GamepadAxis::RightY);
            InputMap::bindAction("Jump", KeyCode::Space);
            InputMap::bindAction("Jump", GamepadButton::South);
        }
        bool action(const std::string_view name, const int phase) {
            installDefaults(); const auto it = actions.find(std::string{name});
            return it != actions.end() && std::any_of(it->second.begin(), it->second.end(), [phase](const auto &b) { return evaluate(b, phase); });
        }
    }
    void InputMap::clear() { actions.clear(); axes.clear(); defaultsInstalled = true; }
    void InputMap::removeAction(const std::string_view name) { actions.erase(std::string{name}); axes.erase(std::string{name}); }
    void InputMap::bindAction(const std::string_view name, const KeyCode key) { actions[std::string{name}].emplace_back(KeyBinding{key}); }
    void InputMap::bindAction(const std::string_view name, const MouseButton button) { actions[std::string{name}].emplace_back(MouseBinding{button}); }
    void InputMap::bindAction(const std::string_view name, const GamepadButton button) { actions[std::string{name}].emplace_back(PadBinding{button}); }
    void InputMap::bindAxis2D(const std::string_view name, const KeyCode nx, const KeyCode px, const KeyCode ny, const KeyCode py) { axes[std::string{name}].emplace_back(DigitalAxis{nx, px, ny, py}); }
    void InputMap::bindAxis2D(const std::string_view name, const GamepadAxis x, const GamepadAxis y, const float dz) { axes[std::string{name}].emplace_back(StickAxis{x, y, std::max(0.0F, dz)}); }
    void InputMap::bindMouseDelta(const std::string_view name, const Vec2 scale) { axes[std::string{name}].emplace_back(MouseAxis{scale}); }
    bool InputMap::actionDown(const std::string_view name) { return action(name, 0); }
    bool InputMap::actionPressed(const std::string_view name) { return action(name, 1); }
    bool InputMap::actionReleased(const std::string_view name) { return action(name, 2); }
    Vec2 InputMap::axis2D(const std::string_view name) {
        installDefaults(); Vec2 value{}; const auto it = axes.find(std::string{name}); if (it == axes.end()) return value;
        for (const AxisBinding &binding : it->second) std::visit([&value](const auto &control) {
            using T = std::decay_t<decltype(control)>;
            if constexpr (std::is_same_v<T, DigitalAxis>) value += Vec2{(Input::keyDown(control.px) ? 1.0F : 0.0F) - (Input::keyDown(control.nx) ? 1.0F : 0.0F), (Input::keyDown(control.py) ? 1.0F : 0.0F) - (Input::keyDown(control.ny) ? 1.0F : 0.0F)};
            else if constexpr (std::is_same_v<T, StickAxis>) { Vec2 stick{Input::gamepadAxis(control.x), Input::gamepadAxis(control.y)}; if (stick.length() >= control.deadZone) value += stick; }
            else value += Input::mouseDelta() * control.scale;
        }, binding);
        return value;
    }
}
