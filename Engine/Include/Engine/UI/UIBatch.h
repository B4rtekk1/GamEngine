#pragma once

#include <Engine/UI/RectTransform.h>
#include <Engine/UI/UIVertex.h>

#include <cstdint>
#include <vector>

namespace Engine::UI {

class UIBatch final {
public:
    void clear() noexcept {
        vertices.clear();
        indices.clear();
    }

    void addQuad(const Rect& rect, const Vec4& color) {
        if (rect.width <= 0.0f || rect.height <= 0.0f) {
            return;
        }

        const auto first = static_cast<std::uint32_t>(vertices.size());
        const float right = rect.x + rect.width;
        const float bottom = rect.y + rect.height;
        vertices.insert(vertices.end(), {
            {{rect.x, rect.y}, {0.0f, 0.0f}, color},
            {{right, rect.y}, {1.0f, 0.0f}, color},
            {{right, bottom}, {1.0f, 1.0f}, color},
            {{rect.x, bottom}, {0.0f, 1.0f}, color},
        });
        indices.insert(indices.end(), {
            first, first + 1, first + 2,
            first, first + 2, first + 3,
        });
    }

    [[nodiscard]] bool empty() const noexcept { return indices.empty(); }

    std::vector<UIVertex> vertices;
    std::vector<std::uint32_t> indices;
};

} // namespace Engine::UI
