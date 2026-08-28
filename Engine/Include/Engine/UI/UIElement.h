#pragma once

#include <Engine/UI/RectTransform.h>

#include <memory>
#include <cstdint>
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

        virtual void update(float deltaTime) {
        }

        virtual void buildGeometry(UIBatch &batch) const {
        }

        /// Returns a value that changes whenever subclass-owned visual data changes.
        /// The renderer combines it with hierarchy, visibility and layout state to
        /// decide whether the cached UI batch can be reused.
        [[nodiscard]] virtual std::uint64_t geometryRevision() const noexcept { return 0; }

        void updateLayout(const Rect &parentRect) {
            rectTransform.calculate(parentRect);

            for (auto &child: m_children) {
                child->updateLayout(rectTransform.calculatedRect);
            }
        }

        [[nodiscard]] const auto &children() const { return m_children; }

        RectTransform rectTransform;
        bool visible{true};
        int sortingOrder{0};

    protected:
        UIElement *m_parent{};
        std::vector<std::unique_ptr<UIElement> > m_children;
    };
}
