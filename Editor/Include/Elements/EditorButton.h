#pragma once

#include "imgui.h"

/**
 * @brief Lightweight wrapper around an ImGui button.
 *
 * Stores the button label and optional size, and provides methods for drawing
 * either a regular or a small ImGui button.
 *
 * @note The label string is not copied. It must remain valid for the entire
 *       lifetime of the EditorButton instance.
 */
class EditorButton final {
public:
    /**
     * @brief Constructs an editor button.
     *
     * @param label Null-terminated text displayed on the button.
     * @param size Desired button size in pixels. A zero component lets ImGui
     *             calculate that dimension automatically.
     */
    explicit EditorButton(const char* label, const ImVec2 size = {}) noexcept
        : label_(label), size_(size) {}

    /**
     * @brief Draws a regular ImGui button.
     *
     * @return true if the button was activated during the current frame;
     *         otherwise false.
     */
    [[nodiscard]] bool draw() const {
        return ImGui::Button(label_, size_);
    }

    /**
     * @brief Draws a compact ImGui button.
     *
     * @return true if the button was activated during the current frame;
     *         otherwise false.
     */
    [[nodiscard]] bool drawSmall() const {
        return ImGui::SmallButton(label_);
    }

private:
    const char* label_; ///< Null-terminated label displayed by the button.
    ImVec2 size_;       ///< Requested size of the regular button in pixels.
};