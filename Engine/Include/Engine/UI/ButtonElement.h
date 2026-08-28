#pragma once

#include "Engine/UI/PanelElement.h"

#include <functional>

namespace Engine::UI {
    /** A simple clickable panel widget. Input dispatch can call click(). */
    class ButtonElement final : public UIElement {
    public:
        explicit ButtonElement(Math::Color color = Math::Color::white()) : color(color) {
        }

        void buildGeometry(UIBatch &batch) const override {
            batch.addQuad(rectTransform.calculatedRect, color);
        }

        [[nodiscard]] std::uint64_t geometryRevision() const noexcept override {
            return static_cast<std::uint64_t>(color.to_a2b10g10r10());
        }

        void click() const {
            if (onClick) {
                onClick();
            }
        }

        std::function<void()> onClick;
        Math::Color color;
    };
} // namespace Engine::UI
