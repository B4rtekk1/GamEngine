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
    transform.anchorMin = {0.25f, 0.5f};
    transform.anchorMax = {0.75f, 1.0f};
    transform.offsetMin = {10.0f, 20.0f};
    transform.offsetMax = {-30.0f, -40.0f};
    transform.calculate({100.0f, 50.0f, 800.0f, 600.0f});
    if (!equal(transform.calculatedRect, {310.0f, 370.0f, 360.0f, 240.0f})) return 1;

    auto parent = std::make_unique<UIElement>();
    parent->rectTransform.offsetMin = {20.0f, 30.0f};
    parent->rectTransform.offsetMax = {220.0f, 230.0f};
    auto child = std::make_unique<PanelElement>(Engine::Math::Color::red());
    child->rectTransform.anchorMin = {0.5f, 0.5f};
    child->rectTransform.anchorMax = {1.0f, 1.0f};
    child->rectTransform.offsetMin = {5.0f, 6.0f};
    child->rectTransform.offsetMax = {-7.0f, -8.0f};
    PanelElement* childPointer = child.get();
    parent->addChild(std::move(child));
    parent->updateLayout({0.0f, 0.0f, 400.0f, 300.0f});
    if (!equal(parent->rectTransform.calculatedRect, {20.0f, 30.0f, 200.0f, 200.0f}) ||
        !equal(childPointer->rectTransform.calculatedRect, {125.0f, 136.0f, 88.0f, 86.0f})) return 2;

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
