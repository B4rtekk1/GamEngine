#pragma once

#include <Engine/Math/Vec2.h>
#include <Engine/Math/Color.h>

namespace Engine::UI {
    /**
     * @brief Vertex consumed by the Vulkan UI pipeline.
     */
    struct UIVertex {
        /** @brief Position in UI coordinates. */
        Vec2 position;

        /** @brief Font-atlas texture coordinates. */
        Vec2 uv;

        /** @brief Per-vertex RGBA color. */
        Math::Color color;

        /** @brief 1 for font glyphs, 0 for solid UI geometry. */
        float textSample = 0.0F;

        [[nodiscard]] static constexpr std::size_t size() noexcept {
            return sizeof(UIVertex);
        }
    };
}
