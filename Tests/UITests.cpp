#include <Engine/UI/ButtonElement.h>
#include <Engine/UI/PanelElement.h>
#include <Engine/UI/RectTransform.h>
#include <Engine/UI/UIBatch.h>

#include <memory>

namespace {
bool equal(const Engine::UI::Rect& a, const Engine::UI::Rect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
}

int main() {
    using namespace Engine::UI;

    RectTransform transform;
    transform.anchorMin = {0.25F, 0.5F};
    transform.anchorMax = {0.75F, 1.0F};
    transform.offsetMin = {10.0F, 20.0F};
    transform.offsetMax = {-30.0F, -40.0F};
    transform.calculate({100.0F, 50.0F, 800.0F, 600.0F});
    if (!equal(transform.calculatedRect, {310.0F, 370.0F, 360.0F, 240.0F})) return 1;

    auto parent = std::make_unique<UIElement>();
    parent->rectTransform.offsetMin = {20.0F, 30.0F};
    parent->rectTransform.offsetMax = {220.0F, 230.0F};
    auto child = std::make_unique<PanelElement>(Engine::Math::Color::red());
    child->rectTransform.anchorMin = {0.5F, 0.5F};
    child->rectTransform.anchorMax = {1.0F, 1.0F};
    child->rectTransform.offsetMin = {5.0F, 6.0F};
    child->rectTransform.offsetMax = {-7.0F, -8.0F};
    PanelElement* childPointer = child.get();
    parent->addChild(std::move(child));
    parent->updateLayout({0.0F, 0.0F, 400.0F, 300.0F});
    if (!equal(parent->rectTransform.calculatedRect, {20.0F, 30.0F, 200.0F, 200.0F}) ||
        !equal(childPointer->rectTransform.calculatedRect, {125.0F, 136.0F, 88.0F, 86.0F})) return 2;

    UIBatch batch;
    childPointer->buildGeometry(batch);
    if (batch.vertices.size() != 4 || batch.indices.size() != 6 ||
        childPointer->geometryRevision() == 0) return 3;

    int clicks = 0;
    ButtonElement button;
    button.onClick = [&clicks] { ++clicks; };
    button.click();
    button.click();
    if (clicks != 2 || button.geometryRevision() == 0) return 4;
    const auto oldRevision = button.geometryRevision();
    button.color = Engine::Math::Color::blue();
    if (button.geometryRevision() == oldRevision) return 5;

    batch.clear();
    if (!batch.empty() || !batch.vertices.empty() || !batch.indices.empty()) return 6;
    return 0;
}