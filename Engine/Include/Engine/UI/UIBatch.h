#pragma once

#include "Engine/UI/UIVertex.h"
#include "Engine/UI/RectTransform.h"
#include "Engine/UI/Components/TextComponent.h"
#include "Engine/UI/Vulkan/UIFontAtlas.h"

#include <cstdint>
#include <vector>

namespace Engine::UI
{
    /**
     * @brief CPU-side batch of vertices and indices for UI text.
     */
    class UIBatch final
    {
    public:
        /** @brief Removes all previously generated geometry. */
        void clear() noexcept
        {
            vertices.clear();
            indices.clear();
        }

        /**
         * @brief Appends one text component as indexed quads.
         * @param text Text component to append.
         * @param atlas Font atlas containing glyph metrics.
         * @param originX Baseline origin in UI coordinates.
         * @param originY Baseline origin in UI coordinates.
         */
        void appendText(const TextComponent& text,
                        const UIFontAtlas& atlas,
                        float originX,
                        float originY);

        void addQuad(const Rect& rect, const Math::Color& color);

        [[nodiscard]] bool empty() const noexcept { return indices.empty(); }

        // Kept public for the existing renderer and callers that upload the batch directly.
        std::vector<UIVertex> vertices;
        std::vector<std::uint32_t> indices;

    private:
    };
}
