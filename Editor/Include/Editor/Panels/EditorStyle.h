#pragma once

#include "imgui.h"

class EditorStyle final {
public:
    static void apply();
    static void configureDockLayout(ImVec2 dockSize);
};
