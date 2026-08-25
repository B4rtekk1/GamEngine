#include <Engine/UI/Canvas.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Engine::UI {
    Canvas::Canvas(const std::uint32_t width, const std::uint32_t height)
        : m_width(width),
          m_height(height)
    {
    }

    void Canvas::resize(const std::uint32_t width, const std::uint32_t height)
    {
        if (m_width == width && m_height == height) return;
        m_width = width;
        m_height = height;
        updateLayout();
    }

    void Canvas::updateLayout()
    {
        const Rect canvasRect = rect();

        for (auto& element : m_elements) {
            element->updateLayout(canvasRect);
        }
        bumpRevision();
    }

    UIElement& Canvas::addElement(std::unique_ptr<UIElement> element)
    {
        if (!element) {
            throw std::invalid_argument{"Canvas::addElement requires a non-null element"};
        }

        element->updateLayout(rect());
        UIElement& addedElement = *element;
        m_elements.emplace_back(std::move(element));
        bumpRevision();
        return addedElement;
    }

    std::unique_ptr<UIElement> Canvas::removeElement(const UIElement* element) noexcept
    {
        const auto position = std::find_if(
            m_elements.begin(),
            m_elements.end(),
            [element](const auto& candidate) { return candidate.get() == element; });

        if (position == m_elements.end()) {
            return nullptr;
        }

        auto removed = std::move(*position);
        m_elements.erase(position);
        bumpRevision();
        return removed;
    }

    void Canvas::clear() noexcept
    {
        if (m_elements.empty()) return;
        m_elements.clear();
        bumpRevision();
    }

    void Canvas::invalidate() noexcept
    {
        bumpRevision();
    }

    void Canvas::bumpRevision() noexcept
    {
        ++m_revision;
        if (m_revision == 0) ++m_revision;
    }

    Rect Canvas::rect() const noexcept
    {
        return {
            0.0F,
            0.0F,
            static_cast<float>(m_width),
            static_cast<float>(m_height)
        };
    }
} // namespace Engine::UI