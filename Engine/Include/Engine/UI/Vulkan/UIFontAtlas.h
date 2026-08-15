#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine::UI
{
    /** @brief Rectangle in normalized texture coordinates. */
    struct GlyphUV final
    {
        float min[2]{};
        float max[2]{};
    };

    /**
     * @brief Rasterized metrics and atlas coordinates for one glyph.
     */
    struct Glyph final
    {
        GlyphUV uv;
        float width = 0.0f;
        float height = 0.0f;
        float bearingX = 0.0f;
        float bearingY = 0.0f;
        float advance = 0.0f;
    };

    /**
     * @brief CPU-side single-channel font atlas.
     *
     * The pixel buffer is R8_UNORM and can be uploaded through the engine's
     * existing Texture2D abstraction. The class deliberately does not own a
     * Vulkan image or sampler.
     */
    class UIFontAtlas final
    {
    public:
        /** @brief Creates an empty atlas. */
        UIFontAtlas() = default;

        /** @brief Releases atlas memory. */
        ~UIFontAtlas() = default;

        UIFontAtlas(const UIFontAtlas&) = delete;
        UIFontAtlas& operator=(const UIFontAtlas&) = delete;
        UIFontAtlas(UIFontAtlas&&) noexcept = default;
        UIFontAtlas& operator=(UIFontAtlas&&) noexcept = default;

        /**
         * @brief Builds an atlas from a TrueType-outline font.
         * @param fontPath Path to a `.ttf` file (CFF-based OpenType is not supported).
         * @param pixelSize Glyph rasterization size in pixels.
         * @param firstCodepoint First codepoint included in the atlas.
         * @param lastCodepoint Last codepoint included in the atlas.
         * @return Empty string on success, otherwise an error message.
         */
        [[nodiscard]] std::string build(const std::string& fontPath,
                                         std::uint32_t pixelSize,
                                         std::uint32_t firstCodepoint = 32,
                                         std::uint32_t lastCodepoint = 126);

        /** @return Atlas width in pixels. */
        [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }

        /** @return Atlas height in pixels. */
        [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }

        /** @return Rasterized R8_UNORM atlas pixels. */
        [[nodiscard]] const std::vector<std::uint8_t>& pixels() const noexcept
        {
            return m_pixels;
        }

        /** @return Font ascent in pixels. */
        [[nodiscard]] float ascent() const noexcept { return m_ascent; }

        /** @return Font line height in pixels. */
        [[nodiscard]] float lineHeight() const noexcept { return m_lineHeight; }

        /**
         * @brief Finds glyph metrics for a Unicode codepoint.
         * @param codepoint Unicode codepoint.
         * @return Glyph pointer, or nullptr when not present.
         */
        [[nodiscard]] const Glyph* glyph(std::uint32_t codepoint) const noexcept;

        /** @brief Returns the configured rasterization size. */
        [[nodiscard]] std::uint32_t pixelSize() const noexcept
        {
            return m_pixelSize;
        }

    private:
        std::uint32_t m_width = 0;
        std::uint32_t m_height = 0;
        std::uint32_t m_pixelSize = 0;
        float m_ascent = 0.0f;
        float m_lineHeight = 0.0f;
        std::vector<std::uint8_t> m_pixels;
        std::unordered_map<std::uint32_t, Glyph> m_glyphs;
    };
}
