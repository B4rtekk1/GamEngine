#include "Engine/UI/UIBatch.h"

#include <cstdint>

namespace Engine::UI
{
    namespace
    {
        void appendQuad(std::vector<UIVertex>& vertices,
                        std::vector<std::uint32_t>& indices,
                        float x0, float y0, float x1, float y1,
                        const Glyph& glyph,
                        const Math::Color& color,
                        const float textSample)
        {
            const auto base = static_cast<std::uint32_t>(vertices.size());
            const float u0 = glyph.uv.min[0];
            const float v0 = glyph.uv.min[1];
            const float u1 = glyph.uv.max[0];
            const float v1 = glyph.uv.max[1];

            const float positions[4][2] = {
                {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}
            };
            const float uvs[4][2] = {
                {u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}
            };

            for (int i = 0; i < 4; ++i)
            {
                UIVertex vertex{};
                vertex.position = Vec2{positions[i][0], positions[i][1]};
                vertex.uv = Vec2{uvs[i][0], uvs[i][1]};
                vertex.color = color;
                vertex.textSample = textSample;
                vertices.push_back(vertex);
            }

            indices.insert(indices.end(), {
                base, base + 1, base + 2,
                base, base + 2, base + 3
            });
        }
    }

    void UIBatch::appendText(const TextComponent& text,
                             const UIFontAtlas& atlas,
                             float originX,
                             float originY)
    {
        if (!text.isRenderable() || atlas.pixelSize() == 0)
            return;

        float penX = originX;
        float baselineY = originY;

        for (std::size_t i = 0; i < text.text.size(); ++i)
        {
            const auto codepoint = static_cast<unsigned char>(text.text[i]);
            if (codepoint == '\n')
            {
                penX = originX;
                baselineY += atlas.lineHeight() + text.lineSpacing;
                continue;
            }

            const Glyph* glyph = atlas.glyph(codepoint);
            if (glyph == nullptr)
                continue;

            const float scale = text.fontSize /
                                static_cast<float>(atlas.pixelSize());
            const float x0 = penX + glyph->bearingX * scale * text.horizontalScale;
            const float y0 = baselineY - glyph->bearingY * scale;
            const float x1 = x0 + glyph->width * scale * text.horizontalScale;
            const float y1 = y0 + glyph->height * scale;

            appendQuad(vertices, indices, x0, y0, x1, y1, *glyph, text.color, 1.0f);
            penX += glyph->advance * scale * text.horizontalScale;
        }
    }

    void UIBatch::addQuad(const Rect& rect, const Math::Color& color)
    {
        if (rect.width <= 0.0f || rect.height <= 0.0f)
            return;

        const auto base = static_cast<std::uint32_t>(vertices.size());
        const float right = rect.x + rect.width;
        const float bottom = rect.y + rect.height;
        vertices.insert(vertices.end(), {
            {{rect.x, rect.y}, {0.0f, 0.0f}, color, 0.0f},
            {{right, rect.y}, {1.0f, 0.0f}, color, 0.0f},
            {{right, bottom}, {1.0f, 1.0f}, color, 0.0f},
            {{rect.x, bottom}, {0.0f, 1.0f}, color, 0.0f},
        });
        indices.insert(indices.end(), {base, base + 1, base + 2,
                                       base, base + 2, base + 3});
    }
}
