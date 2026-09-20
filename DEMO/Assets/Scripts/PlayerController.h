#pragma once

#include <Engine/Scripting/Script.h>
#include <Engine/Scripting/ScriptAttributes.h>

class PlayerController final : public Engine::Script {
public:
    void onUpdate(float deltaTime) override;

    GE_PROPERTY(
        Header("Movement"),
        Range(0.0F, 50.0F),
        Tooltip("Player movement speed.")
    )
    float MovementSpeed = 6.0F;

    GE_PROPERTY(
        Header("Jump"),
        Range(0.0F, 30.0F),
        Tooltip("Player jump velocity.")
    )
    float JumpSpeed = 8.0F;

    GE_PROPERTY(
        Range(0.01F, 1.0F),
        Step(0.01F),
        Tooltip("Distance used to detect the ground.")
    )
    float GroundCheckDistance = 0.15F;
};