#include "PlayerController.h"

#include <Engine/Input/Input.h>
#include <Engine/Physics/PhysicsSystem.h>
#include <Engine/Scene/Scene.h>

namespace {

constexpr float Epsilon = 1.0e-5F;

}

void PlayerController::onUpdate(const float deltaTime) {
    (void)deltaTime;

    Engine::Actor body = actor();

    if (!body.hasRigidbody()) {
        for (const Engine::Actor& child : children()) {
            if (child.hasRigidbody()) {
                body = child;
                break;
            }
        }
    }

    if (!body.valid() || !body.hasRigidbody()) {
        return;
    }

    Engine::Vec2 input = Engine::Input::axis2D("Move");

    if (input.length() > 1.0F) {
        input = input.normalized();
    }

    Engine::Vec3 forward{1.0F, 0.0F, 0.0F};
    Engine::Vec3 right{0.0F, 0.0F, 1.0F};

    const Engine::Actor camera = scene().primaryCamera();

    if (camera.valid()) {
        forward = camera.forward();
        forward.setY(0.0F);

        right = camera.right();
        right.setY(0.0F);

        if (forward.length() > Epsilon) {
            forward = forward.normalized();
        }

        if (right.length() > Epsilon) {
            right = right.normalized();
        }
    }

    Engine::Vec3 movement =
        forward * input.y() +
        right * input.x();

    if (movement.length() > Epsilon) {
        movement = movement.normalized() * MovementSpeed;
    }

    Engine::Vec3 velocity = body.velocity();

    velocity.setX(movement.x());
    velocity.setZ(movement.z());

    const Engine::Vec3 groundProbeOrigin =
        body.worldPosition() + Engine::Vec3{0.0F, -0.51F, 0.0F};

    const auto groundHit = physics().raycast(
        groundProbeOrigin,
        {0.0F, -1.0F, 0.0F},
        GroundCheckDistance
    );

    const bool grounded =
        groundHit.has_value() &&
        groundHit->actor.id() != body.id() &&
        velocity.y() <= 0.5F;

    if (grounded && Engine::Input::actionPressed("Jump")) {
        velocity.setY(JumpSpeed);
    }

    body.setVelocity(velocity);
}