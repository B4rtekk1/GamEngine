#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "Engine/Math/Color.h"

namespace Engine {
    /**
     * @brief Text rendered by the UI text pipeline.
     */
    struct TextComponent final {
        /** @brief UTF-8 text content. */
        std::string text;

        /** @brief Font atlas identifier resolved by the UI renderer. */
        std::uint64_t fontAtlasId = 0;

        /** @brief Requested font size in pixels. */
        float fontSize = 16;

        /** @brief RGBA text color. */
        Math::Color color{1.0f, 1.0f, 1.0f, 1.0f};

        /** @brief Additional line spacing in pixels. */
        float lineSpacing = 0.0f;

        /** @brief Horizontal scale applied while laying out glyphs. */
        float horizontalScale = 1.0f;

        /** @brief Enables or disables text rendering. */
        bool visible = true;

        /**
         * @brief Checks whether this component contains drawable text.
         * @return True if the component is visible and contains text.
         */
        [[nodiscard]] bool isRenderable() const noexcept {
            return visible && !text.empty() && fontSize > 0.0f && horizontalScale > 0.0f;
        }
    };
}
