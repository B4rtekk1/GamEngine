#pragma once

#include <Engine/UI/UIBatch.h>
#include <Engine/UI/UIElement.h>

namespace Engine::UI {

class PanelElement final : public UIElement {
public:
    explicit PanelElement(Vec4 color = {1.0f, 1.0f, 1.0f, 1.0f})
        : color(color) {}

    void buildGeometry(UIBatch& batch) const override {
        batch.addQuad(rectTransform.calculatedRect, color);
    }

    Vec4 color;
};

} // namespace Engine::UI
