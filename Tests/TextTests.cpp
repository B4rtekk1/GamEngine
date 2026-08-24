#include <Engine/UI/Components/TextComponent.h>
#include <Engine/UI/TextElement.h>

int main() {
    using namespace Engine;
    using namespace Engine::UI;

    TextComponent text;
    if (text.isRenderable() || text.fontSize != 16.0f || !text.visible ||
        text.horizontalScale != 1.0f) return 1;
    text.text = "Hello";
    if (!text.isRenderable()) return 2;
    text.visible = false;
    if (text.isRenderable()) return 3;
    text.visible = true;
    text.fontSize = 0.0f;
    if (text.isRenderable()) return 4;
    text.fontSize = 16.0f;
    text.horizontalScale = -1.0f;
    if (text.isRenderable()) return 5;

    UIFontAtlas atlas;
    TextComponent drawable{.text = "Hello", .fontSize = 18.0f};
    TextElement element{drawable, atlas};
    const auto initialRevision = element.geometryRevision();
    element.text.text = "World";
    if (element.geometryRevision() == initialRevision) return 6;
    const auto changedTextRevision = element.geometryRevision();
    element.text.color = Math::Color::red();
    if (element.geometryRevision() == changedTextRevision) return 7;
    const auto changedColorRevision = element.geometryRevision();
    element.text.visible = false;
    if (element.geometryRevision() == changedColorRevision) return 8;
    return 0;
}
