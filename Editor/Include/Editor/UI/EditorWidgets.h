#pragma once

#include "imgui.h"

#include <cstddef>

namespace EditorUI {

bool primaryButton(const char* label, ImVec2 size = {});
void panelHeader(const char* title, const char* detail = nullptr);
bool searchBox(const char* id, const char* hint, char* value, std::size_t capacity);
void emptyState(const char* icon, const char* title, const char* description);

} // namespace EditorUI
