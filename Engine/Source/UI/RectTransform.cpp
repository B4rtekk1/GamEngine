#include <Engine/UI/RectTransform.h>

namespace Engine::UI {
    void RectTransform::calculate(const Rect &parentRect) {
        const float anchorLeft = parentRect.x + parentRect.width * anchorMin.x();
        const float anchorTop = parentRect.y + parentRect.height * anchorMin.y();
        const float anchorRight = parentRect.x + parentRect.width * anchorMax.x();
        const float anchorBottom = parentRect.y + parentRect.height * anchorMax.y();

        calculatedRect.x = anchorLeft + offsetMin.x();
        calculatedRect.y = anchorTop + offsetMin.y();

        calculatedRect.width = anchorRight - anchorLeft + offsetMax.x() - offsetMin.x();
        calculatedRect.height = anchorBottom - anchorTop + offsetMax.y() - offsetMin.y();
    }
}
