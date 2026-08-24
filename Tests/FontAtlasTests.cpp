#include <Engine/UI/Vulkan/UIFontAtlas.h>

#include <string>

int main() {
    Engine::UI::UIFontAtlas atlas;
    if (atlas.width() != 0 || atlas.height() != 0 || atlas.pixelSize() != 0 ||
        atlas.ascent() != 0.0f || atlas.lineHeight() != 0.0f ||
        atlas.glyph('A') != nullptr || !atlas.pixels().empty()) return 1;

    if (atlas.build("", 16) != "Invalid font atlas parameters" ||
        atlas.build("missing-font.ttf", 0) != "Invalid font atlas parameters" ||
        atlas.build("missing-font.ttf", 16, 100, 99) != "Invalid font atlas parameters") return 2;
    const std::string error = atlas.build("missing-font.ttf", 16);
    if (error.empty() || atlas.width() != 0 || atlas.height() != 0 ||
        atlas.pixelSize() != 0 || atlas.glyph('A') != nullptr) return 3;
    return 0;
}
