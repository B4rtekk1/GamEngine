#pragma once

#include "imgui.h"

#include <algorithm>

/** Numeric editor controls that open a text field after a double-click. */
namespace Editor::Controls {

inline ImGuiID& textEditId() {
    static ImGuiID id{};
    return id;
}

inline bool& textEditNeedsFocus() {
    static bool needsFocus{};
    return needsFocus;
}

inline bool shouldUseTextEdit(const char* label) {
    const ImGuiID id = ImGui::GetID(label);
    if (textEditId() == id) {
        if (textEditNeedsFocus()) {
            ImGui::SetKeyboardFocusHere();
            textEditNeedsFocus() = false;
        }
        return true;
    }
    return false;
}

inline void finishTextEdit(const char* label, const bool submitted) {
    if (submitted || ImGui::IsItemDeactivated()) {
        const ImGuiID id = ImGui::GetID(label);
        if (textEditId() == id) {
            textEditId() = 0;
            textEditNeedsFocus() = false;
        }
    }
}

inline void enableTextEditOnDoubleClick(const char* label) {
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        textEditId() = ImGui::GetID(label);
        textEditNeedsFocus() = true;
    }
}

inline bool sliderFloat(const char* label, float* value, const float minimum, const float maximum,
                        const char* format = "%.3f", const ImGuiSliderFlags flags = 0) {
    if (shouldUseTextEdit(label)) {
        const bool changed = ImGui::InputFloat(label, value, 0.0F, 0.0F, format,
                                               ImGuiInputTextFlags_EnterReturnsTrue);
        if (changed) *value = std::clamp(*value, minimum, maximum);
        finishTextEdit(label, changed);
        return changed;
    }
    const bool changed = ImGui::SliderFloat(label, value, minimum, maximum, format, flags);
    enableTextEditOnDoubleClick(label);
    return changed;
}

inline bool sliderInt(const char* label, int* value, const int minimum, const int maximum,
                      const char* format = "%d", const ImGuiSliderFlags flags = 0) {
    if (shouldUseTextEdit(label)) {
        const bool changed = ImGui::InputInt(label, value, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue);
        if (changed) *value = std::clamp(*value, minimum, maximum);
        finishTextEdit(label, changed);
        return changed;
    }
    const bool changed = ImGui::SliderInt(label, value, minimum, maximum, format, flags);
    enableTextEditOnDoubleClick(label);
    return changed;
}

inline bool dragFloat(const char* label, float* value, const float speed = 1.0F,
                      const float minimum = 0.0F, const float maximum = 0.0F,
                      const char* format = "%.3f", const ImGuiSliderFlags flags = 0) {
    if (shouldUseTextEdit(label)) {
        const bool changed = ImGui::InputFloat(label, value, 0.0F, 0.0F, format,
                                               ImGuiInputTextFlags_EnterReturnsTrue);
        if (changed && minimum < maximum) *value = std::clamp(*value, minimum, maximum);
        finishTextEdit(label, changed);
        return changed;
    }
    const bool changed = ImGui::DragFloat(label, value, speed, minimum, maximum, format, flags);
    enableTextEditOnDoubleClick(label);
    return changed;
}

inline bool dragInt(const char* label, int* value, const float speed = 1.0F,
                    const int minimum = 0, const int maximum = 0,
                    const char* format = "%d", const ImGuiSliderFlags flags = 0) {
    if (shouldUseTextEdit(label)) {
        const bool changed = ImGui::InputInt(label, value, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue);
        if (changed && minimum < maximum) *value = std::clamp(*value, minimum, maximum);
        finishTextEdit(label, changed);
        return changed;
    }
    const bool changed = ImGui::DragInt(label, value, speed, minimum, maximum, format, flags);
    enableTextEditOnDoubleClick(label);
    return changed;
}

inline bool dragFloat3(const char* label, float values[3], const float speed = 1.0F,
                       const float minimum = 0.0F, const float maximum = 0.0F,
                       const char* format = "%.3f", const ImGuiSliderFlags flags = 0) {
    if (shouldUseTextEdit(label)) {
        const bool changed = ImGui::InputFloat3(label, values, format, ImGuiInputTextFlags_EnterReturnsTrue);
        if (changed && minimum < maximum) {
            for (int index = 0; index < 3; ++index) values[index] = std::clamp(values[index], minimum, maximum);
        }
        finishTextEdit(label, changed);
        return changed;
    }
    const bool changed = ImGui::DragFloat3(label, values, speed, minimum, maximum, format, flags);
    enableTextEditOnDoubleClick(label);
    return changed;
}

} // namespace Editor::Controls
