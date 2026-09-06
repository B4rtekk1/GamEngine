#pragma once


namespace Engine {
    enum class RigidbodyType:uint8_t {
        Static,
        Dynamic,
        Kinematic,
    };

    struct RigidbodyComponent {
        RigidbodyType type = RigidbodyType::Dynamic;

        float mass = 1.0F;
        float linearDamping = 0.05F; //NOLINT
        float angularDamping = 0.05F; //NOLINT

        bool useGravity = true;
        bool fixedRotation = false;

    };
}
