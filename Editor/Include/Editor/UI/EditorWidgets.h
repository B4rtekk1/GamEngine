#pragma once

#include "Editor/UI/EditorIcons.h"
#include "imgui.h"

#include <cstddef>
#include <span>

namespace EditorUI {

bool primaryButton(const char* label, ImVec2 size = {});
void panelHeader(const char* title, const char* detail = nullptr);
bool searchBox(const char* id, const char* hint, char* value, std::size_t capacity, float width = -1.0F);
void emptyState(Icon icon, const char* title, const char* description);

/** Collapsing header styled as a neutral section bar rather than a selection. */
bool collapsingSection(const char* label, ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen);

/** Thin vertical divider between groups of toolbar items on one line. */
void toolbarSeparator();

/**
 * Mutually exclusive tab-like buttons, e.g. Scene | Game.
 * @return true when @p selected changed.
 */
bool segmentedControl(const char* id, std::span<const char* const> labels, int& selected);

/** Draws a keyboard key cap, e.g. for shortcut tables. */
void keyChip(const char* key);

/** Small rounded label with a coloured background. */
void badge(const char* text, ImVec4 background, ImVec4 foreground);

} // namespace EditorUI
