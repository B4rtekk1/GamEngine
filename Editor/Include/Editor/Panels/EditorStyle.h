#pragma once

#include "imgui.h"

class EditorStyle final {
public:
    static void apply();
    /** Builds the initial layout only when no persisted ImGui layout exists. */
    static void configureDockLayout(ImVec2 dockSize, bool restorePersistedLayout);
};
