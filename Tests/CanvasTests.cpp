#include <Engine/UI/Canvas.h>

#include <memory>
#include <stdexcept>

namespace {
    bool equal(const Engine::UI::Rect& lhs, const Engine::UI::Rect& rhs)
    {
        return lhs.x == rhs.x && lhs.y == rhs.y &&
               lhs.width == rhs.width && lhs.height == rhs.height;
    }
}

int main()
{
    using Engine::UI::Canvas;
    using Engine::UI::Rect;
    using Engine::UI::UIElement;

    Canvas canvas{800, 600};
    if (canvas.width() != 800 || canvas.height() != 600 || !canvas.empty() ||
        !equal(canvas.rect(), Rect{0.0f, 0.0f, 800.0f, 600.0f})) {
        return 1;
    }

    auto root = std::make_unique<UIElement>();
    root->rectTransform.anchorMin = {0.0f, 0.0f};
    root->rectTransform.anchorMax = {1.0f, 1.0f};
    root->rectTransform.offsetMin = {10.0f, 20.0f};
    root->rectTransform.offsetMax = {-30.0f, -40.0f};

    UIElement& added = canvas.addElement(std::move(root));
    if (canvas.size() != 1 ||
        !equal(added.rectTransform.calculatedRect, Rect{10.0f, 20.0f, 760.0f, 540.0f})) {
        return 2;
    }

    canvas.resize(400, 300);
    if (!equal(added.rectTransform.calculatedRect, Rect{10.0f, 20.0f, 360.0f, 240.0f})) {
        return 3;
    }

    auto removed = canvas.removeElement(&added);
    if (!removed || !canvas.empty() || canvas.removeElement(removed.get())) {
        return 4;
    }

    try {
        static_cast<void>(canvas.addElement(nullptr));
        return 5;
    } catch (const std::invalid_argument&) {
    }

    static_cast<void>(canvas.addElement(std::make_unique<UIElement>()));
    canvas.clear();
    return canvas.empty() ? 0 : 6;
}
