#include "Engine/UI/Vulkan/UIFontAtlas.h"

namespace Engine::UI {

const Glyph* UIFontAtlas::glyph(const std::uint32_t codepoint) const noexcept
{
    const auto iterator = m_glyphs.find(codepoint);
    return iterator == m_glyphs.end() ? nullptr : &iterator->second;
}

} // namespace Engine::UI
