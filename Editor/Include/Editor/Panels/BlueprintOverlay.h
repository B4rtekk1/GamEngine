#pragma once

#include "imgui.h"

namespace Engine {
class Renderer;
}

namespace Editor {

/** Draws the world-anchored Y=0 drafting grid over the Scene viewport. */
void drawBlueprintOverlay(const Engine::Renderer& renderer, ImVec2 min, ImVec2 max);

} // namespace Editor
