#pragma once

#include <Engine/UI/UIElement.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Engine::UI {
    enum class CanvasRenderMode:uint8_t {
        ScreenSpaceOverlay = 0,
        ScreenSpaceCamera = 1,
        WorldSpace = 2,

        // Kept as an alias so existing users of the original misspelling continue
        // to compile.
        ScreemSpaceOverlay = ScreenSpaceOverlay,
    };

    class Canvas {
    public:
        using ElementContainer = std::vector<std::unique_ptr<UIElement> >;

        Canvas(std::uint32_t width, std::uint32_t height);

        void resize(std::uint32_t width, std::uint32_t height);

        void updateLayout();

        [[nodiscard]] UIElement &addElement(std::unique_ptr<UIElement> element);

        [[nodiscard]] std::unique_ptr<UIElement> removeElement(const UIElement *element) noexcept;

        void clear() noexcept;

        [[nodiscard]] const ElementContainer &elements() const noexcept { return m_elements; }
        [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
        [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
        [[nodiscard]] std::size_t size() const noexcept { return m_elements.size(); }
        [[nodiscard]] bool empty() const noexcept { return m_elements.empty(); }

        [[nodiscard]] Rect rect() const noexcept;

        /**
         * Marks externally mutated element data as changed. Call this after
         * directly changing public element state such as text, colour,
         * visibility, or sorting order.
         */
        void invalidate() noexcept;

        /** Monotonically increasing revision used by the UI renderer cache. */
        [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }

        CanvasRenderMode renderMode{
            CanvasRenderMode::ScreenSpaceOverlay,
        };

        int sortingOrder{0};

    private:
        std::uint32_t m_width{};
        std::uint32_t m_height{};

        ElementContainer m_elements;
        std::uint64_t m_revision{1};

        void bumpRevision() noexcept;
    };
} // namespace Engine::UI
