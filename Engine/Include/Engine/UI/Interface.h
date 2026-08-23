#pragma once

#include "Engine/UI/ButtonElement.h"
#include "Engine/UI/Canvas.h"
#include "Engine/UI/TextElement.h"
#include "Engine/Input/Input.h"

#include <functional>
#include <memory>
#include <string_view>

namespace Engine::UI {

struct ElementOptions {
    Rect rect{0.0f, 0.0f, 100.0f, 100.0f};
    Math::Color color = Math::Color::white();
};

/** High-level widget factory over Canvas and its concrete UI elements. */
class Interface final {
public:
    Interface(Canvas& canvas, UIFontAtlas& atlas) noexcept
        : canvas_(&canvas), atlas_(&atlas) {}

    [[nodiscard]] PanelElement& panel(std::string_view name = {},
                                       ElementOptions options = {});
    [[nodiscard]] TextElement& label(std::string_view text,
                                     ElementOptions options = {});
    [[nodiscard]] ButtonElement& button(std::string_view text,
                                        std::function<void()> onClick,
                                        ElementOptions options = {});
    void update();

private:
    template<typename T>
    T& add(std::unique_ptr<T> element, const ElementOptions& options) {
        element->rectTransform.offsetMin = {options.rect.x, options.rect.y};
        element->rectTransform.offsetMax = {
            options.rect.x + options.rect.width,
            options.rect.y + options.rect.height};
        return static_cast<T&>(canvas_->addElement(std::move(element)));
    }

    Canvas* canvas_;
    UIFontAtlas* atlas_;

    static void dispatch(UIElement& element, const Vec2& mousePosition);
};

inline PanelElement& Interface::panel(std::string_view, ElementOptions options) {
    return add(std::make_unique<PanelElement>(options.color), options);
}

inline TextElement& Interface::label(std::string_view text, ElementOptions options) {
    TextComponent component;
    component.text = text;
    component.color = options.color;
    return add(std::make_unique<TextElement>(component, *atlas_), options);
}

inline ButtonElement& Interface::button(std::string_view text,
                                        std::function<void()> onClick,
                                        ElementOptions options) {
    auto element = std::make_unique<ButtonElement>(options.color);
    element->onClick = std::move(onClick);
    auto& result = add(std::move(element), options);
    TextComponent caption;
    caption.text = text;
    caption.color = Math::Color::white();
    auto textElement = std::make_unique<TextElement>(caption, *atlas_);
    textElement->rectTransform.offsetMin = {0.0f, 0.0f};
    textElement->rectTransform.offsetMax = {options.rect.width, options.rect.height};
    result.addChild(std::move(textElement));
    return result;
}

inline void Interface::dispatch(UIElement& element, const Vec2& mousePosition) {
    if (element.visible) {
        const auto& rect = element.rectTransform.calculatedRect;
        const bool inside = mousePosition.x() >= rect.x &&
            mousePosition.x() <= rect.x + rect.width &&
            mousePosition.y() >= rect.y &&
            mousePosition.y() <= rect.y + rect.height;
        if (inside) {
            if (auto* button = dynamic_cast<ButtonElement*>(&element)) button->click();
        }
    }
    for (const auto& child : element.children()) dispatch(*child, mousePosition);
}

inline void Interface::update() {
    if (!Input::mousePressed(MouseButton::Left)) return;
    const Vec2 position = Input::mousePosition();
    for (const auto& element : canvas_->elements()) dispatch(*element, position);
}

} // namespace Engine::UI
