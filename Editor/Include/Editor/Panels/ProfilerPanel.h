#pragma once

namespace Engine { class Renderer; }

namespace Editor {
    /** Interactive CPU/GPU frame history, timelines and aggregated hotspots. */
    void drawProfilerPanel(const Engine::Renderer& renderer, bool& isOpen);
}
