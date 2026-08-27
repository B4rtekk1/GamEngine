#pragma once

#include "imgui.h"

void drawPanelHeader(const char *title, const char *subtitle = nullptr);
bool drawToolbarToggle(const char *label, bool active);
void drawSearchIcon(ImVec2 min, ImVec2 max);
bool containsCaseInsensitive(const char *text, const char *query);
