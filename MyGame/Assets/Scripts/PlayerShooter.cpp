#include "PlayerShooter.h"

#include <Engine/Input/Input.h>
#include <Engine/Physics/PhysicsSystem.h>
#include <Engine/Scene/Scene.h>

#include <algorithm>

void PlayerShooter::onUpdate(const float deltaTime) {
    cooldown_ = std::max(0.0F, cooldown_ - std::max(deltaTime, 0.0F));
    // Releasing capture with Escape lets the editor controls be used without firing.
    if (Engine::Input::cursorMode() != Engine::CursorMode::Locked ||
        !Engine::Input::mouseDown(Engine::MouseButton::Left) || cooldown_ > 0.0F) return;

    const auto primary = scene().primaryCamera();
    if (!primary.valid()) return;
    const auto camera = scene().primaryViewCamera();
    if (!camera) return;
    // A camera is commonly a child of the player's capsule. Exclude that body.
    auto owner = primary;
    while (owner.parent().valid()) owner = owner.parent();
    static_cast<void>(physics().shootRay(camera->position(), camera->forward(),
                                       std::max(Range, 0.01F), owner.id()));
    cooldown_ = 1.0F / std::max(FireRate, 0.01F);
}
