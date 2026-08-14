#pragma once

#include <Engine/UI/RectTransform.h>

#include <memory>
#include <vector>

namespace Engine::UI {
    class UIBatch;

    class UIElement {
        public:
        virtual ~UIElement() = default;

        void addChild(std::unique_ptr<UIElement> child) {
            child->m_parent = this;
            m_children.emplace_back(std::move(child));
        }

        virtual void update(float deltaTime) {}
        virtual void buildGeometry(UIBatch& batch) const {}

        void updateLayout(const Rect& parentRect) {
            rectTransform.calculate(parentRect);

            for (auto& child : m_children) {
                child->updateLayout(rectTransform.calculatedRect);
            }
        }

        [[nodiscard]] const auto& children() const { return m_children; }

        RectTransform rectTransform;
        bool visible{true};
        int sortingOrder{0};

    protected:
        UIElement* m_parent{};
        std::vector<std::unique_ptr<UIElement>> m_children;
    };
}
