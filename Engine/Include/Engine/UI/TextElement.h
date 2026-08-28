#pragma once

#include <Engine/UI/UIBatch.h>
#include <Engine/UI/UIElement.h>

#include <bit>

namespace Engine::UI {
    /** UI element that lays out text from a shared font atlas at its top-left corner. */
    class TextElement final : public UIElement {
    public:
        TextElement(const TextComponent &text, const UIFontAtlas &atlas)
            : text(text), atlas_(&atlas) {
        }

        void buildGeometry(UIBatch &batch) const override {
            const Rect &rect = rectTransform.calculatedRect;
            batch.appendText(text, *atlas_, rect.x, rect.y + atlas_->ascent());
        }

        [[nodiscard]] std::uint64_t geometryRevision() const noexcept override {
            std::uint64_t hash = 14695981039346656037ULL; //NOLINT
            const auto mix = [&hash](const std::uint64_t value) {
                hash = (hash ^ value) * 1099511628211ULL; //NOLINT
            };
            mix(std::bit_cast<std::uint32_t>(text.fontSize));
            mix(std::bit_cast<std::uint32_t>(text.lineSpacing));
            mix(std::bit_cast<std::uint32_t>(text.horizontalScale));
            mix(static_cast<std::uint64_t>(text.visible));
            for (const unsigned char character: text.text) {
                mix(character);
}
            mix(std::bit_cast<std::uint32_t>(text.color.r()));
            mix(std::bit_cast<std::uint32_t>(text.color.g()));
            mix(std::bit_cast<std::uint32_t>(text.color.b()));
            mix(std::bit_cast<std::uint32_t>(text.color.a()));
            return hash;
        }

        TextComponent text;

    private:
        const UIFontAtlas *atlas_;
    };
} // namespace Engine::UI
