#pragma once

#include <Engine/Scripting/Script.h>
#include <Engine/Scripting/ScriptAttributes.h>

/** Attach to a GameObject or camera; shots follow the primary game camera. */
class PlayerShooter final : public Engine::Script {
  public:
    void onUpdate(float deltaTime) override;

    GE_PROPERTY(Header("Shooting"), Range(1.0F, 1000.0F), Tooltip("Maximum hitscan distance in metres."))
    float Range = 100.0F;

    GE_PROPERTY(Range(1.0F, 30.0F), Tooltip("Shots per second while holding the left mouse button."))
    float FireRate = 5.0F;

  private:
    float cooldown_{};
};
