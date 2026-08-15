#pragma once

#include <Engine/UI/UIBatch.h>
#include <Engine/UI/UIElement.h>
#include <Engine/Math/Color.h>

#include <bit>

namespace Engine::UI {

class PanelElement final : public UIElement {
public:
    explicit PanelElement(Math::Color color = Math::Color::white())
        : color(color) {}

    void buildGeometry(UIBatch& batch) const override {
        batch.addQuad(rectTransform.calculatedRect, color);
    }

    [[nodiscard]] std::uint64_t geometryRevision() const noexcept override {
        const auto mix = [](std::uint64_t hash, const float value) {
            return (hash ^ std::bit_cast<std::uint32_t>(value)) * 1099511628211ull;
        };
        std::uint64_t hash = 14695981039346656037ull;
        hash = mix(hash, color.r());
        hash = mix(hash, color.g());
        hash = mix(hash, color.b());
        return mix(hash, color.a());
    }

    Math::Color color;
};

} // namespace Engine::UI
