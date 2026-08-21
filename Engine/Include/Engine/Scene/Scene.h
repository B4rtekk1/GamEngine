#pragma once

#include "Engine/ECS/Registry.h"
#include "Engine/Renderer/Particles/ParticleSystem.h"
#include "Engine/UI/Canvas.h"
#include "Engine/UI/Vulkan/UIFontAtlas.h"

namespace Engine {

// Runtime scene data. Content creation belongs to ScenePresets (or to the
// application), rather than to this data container.
class Scene {
public:
    Registry registry;

    [[nodiscard]] UI::Canvas& uiCanvas() noexcept { return canvas_; }
    [[nodiscard]] const UI::Canvas& uiCanvas() const noexcept { return canvas_; }
    [[nodiscard]] const UI::UIFontAtlas& uiFontAtlas() const noexcept { return fontAtlas_; }
    [[nodiscard]] UI::UIFontAtlas& uiFontAtlas() noexcept { return fontAtlas_; }

    [[nodiscard]] bool isParticleScene() const noexcept { return particleScene_; }
    [[nodiscard]] const Particles::ParticleEmitter& particleEmitter() const noexcept {
        return particleEmitter_;
    }

protected:
    void setParticleEmitter(Particles::ParticleEmitter emitter) noexcept {
        particleEmitter_ = emitter;
        particleScene_ = true;
    }

private:
    UI::Canvas canvas_{800, 600};
    UI::UIFontAtlas fontAtlas_{};
    Particles::ParticleEmitter particleEmitter_{};
    bool particleScene_ = false;
};

} // namespace Engine
