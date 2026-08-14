#pragma once

#include <Engine/UI/UIBatch.h>
#include <Engine/UI/UIElement.h>
#include <Engine/Math/Color.h>

namespace Engine::UI {

class PanelElement final : public UIElement {
public:
    explicit PanelElement(Math::Color color = Math::Color::white())
        : color(color) {}

    void buildGeometry(UIBatch& batch) const override {
        batch.addQuad(rectTransform.calculatedRect, color);
    }

    Math::Color color;
};

} // namespace Engine::UI
