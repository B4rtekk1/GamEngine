#include <Engine/UI/Canvas.h>
#include <Engine/UI/PanelElement.h>
#include <Engine/UI/UIBatch.h>

#include <memory>
#include <cstdint>
#include <stdexcept>
#include <vector>

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
    using Engine::UI::PanelElement;
    using Engine::UI::Rect;
    using Engine::UI::UIBatch;
    using Engine::UI::UIElement;

    Canvas canvas{800, 600};
    if (canvas.width() != 800 || canvas.height() != 600 || !canvas.empty() ||
        !equal(canvas.rect(), Rect{0.0F, 0.0F, 800.0F, 600.0F})) {
        return 1;
    }

    auto root = std::make_unique<UIElement>();
    root->rectTransform.anchorMin = {0.0F, 0.0F};
    root->rectTransform.anchorMax = {1.0F, 1.0F};
    root->rectTransform.offsetMin = {10.0F, 20.0F};
    root->rectTransform.offsetMax = {-30.0F, -40.0F};

    UIElement& added = canvas.addElement(std::move(root));
    if (canvas.size() != 1 ||
        !equal(added.rectTransform.calculatedRect, Rect{10.0F, 20.0F, 760.0F, 540.0F})) {
        return 2;
    }

    canvas.resize(400, 300);
    if (!equal(added.rectTransform.calculatedRect, Rect{10.0F, 20.0F, 360.0F, 240.0F})) {
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
    if (!canvas.empty()) {
        return 6;
    }

    PanelElement panel{{0.1F, 0.2F, 0.3F, 0.5F}};
    panel.rectTransform.calculatedRect = {10.0F, 20.0F, 100.0F, 50.0F};
    UIBatch batch;
    panel.buildGeometry(batch);
    if (batch.vertices.size() != 4 || batch.indices.size() != 6 ||
        batch.indices != std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3}) {
        return 7;
    }

    batch.clear();
    if (!batch.empty() || !batch.vertices.empty()) {
        return 8;
    }

    return 0;
}