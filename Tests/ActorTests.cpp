#include <Engine/ECS/Actor.h>
#include <Engine/ECS/Components/CameraComponent.h>
#include <Engine/ECS/Components/ColliderComponent.h>
#include <Engine/ECS/Components/RigidbodyComponent.h>
#include <Engine/Scene/Components/IdentityComponents.h>
#include <Engine/Scene/Components/LightComponent.h>
#include <Engine/Scene/Scene.h>

#include <cmath>
#include <stdexcept>
#include <variant>

namespace {

class TestScene final : public Engine::Scene {
public:
    Engine::Registry& registryForTest() noexcept { return registry(); }
};

bool near(const float lhs, const float rhs, const float epsilon = 0.0001f) {
    return std::abs(lhs - rhs) <= epsilon;
}

bool equal(const Engine::Vec3& lhs, const Engine::Vec3& rhs) {
    return near(lhs.x(), rhs.x()) && near(lhs.y(), rhs.y()) && near(lhs.z(), rhs.z());
}

} // namespace

int main() {
    using namespace Engine;

    TestScene scene;
    Actor actor = scene.createActor("Player");
    if (!actor.valid() || actor.name() != "Player" || scene.objectCount() != 1 ||
        !scene.find("Player") || !scene.find("Player")->isSpawned()) return 1;

    actor.setPosition({1.0f, 2.0f, 3.0f});
    actor.translate({2.0f, -1.0f, 0.5f});
    actor.move({-1.0f, 1.0f, -0.5f});
    actor.setRotation({10.0f, 20.0f, 30.0f});
    actor.setScale({2.0f, 3.0f, 4.0f});
    if (!equal(actor.position(), {2.0f, 2.0f, 3.0f}) ||
        !equal(actor.rotation(), {10.0f, 20.0f, 30.0f}) ||
        !equal(actor.scale(), {2.0f, 3.0f, 4.0f})) return 2;

    actor.addRigidbody();
    actor.setBodyType(RigidbodyType::Kinematic);
    actor.setMass(4.0f);
    actor.setGravityEnabled(false);
    actor.setVelocity({3.0f, 4.0f, 5.0f});
    const auto& body = scene.find(actor.id())->rigidbody();
    if (!actor.hasRigidbody() || body.type != RigidbodyType::Kinematic ||
        !near(body.mass, 4.0f) || body.useGravity || !equal(actor.velocity(), {3.0f, 4.0f, 5.0f})) return 3;

    actor.addBoxCollider({1.0f, 2.0f, 3.0f});
    actor.setColliderTrigger(true);
    actor.setColliderMaterial(0.25f, 0.75f);
    const auto collider = [&] {
        const ColliderComponent* result = nullptr;
        scene.registryForTest().view<ColliderComponent>([&](const Entity, const ColliderComponent& value) {
            result = &value;
        });
        return result;
    }();
    const auto& box = std::get<BoxCollider>(collider->shape);
    if (!actor.hasCollider() || !equal(box.halfExtents, {1.0f, 2.0f, 3.0f}) ||
        !collider->isTrigger || !near(collider->friction, 0.25f) ||
        !near(collider->restitution, 0.75f)) return 4;

    actor.addSphereCollider(2.0f);
    const auto sphere = [&] {
        const ColliderComponent* result = nullptr;
        scene.registryForTest().view<ColliderComponent>([&](const Entity, const ColliderComponent& value) {
            result = &value;
        });
        return result;
    }();
    if (!near(std::get<SphereCollider>(sphere->shape).radius, 2.0f)) return 5;
    actor.addCapsuleCollider(0.4f, 1.8f);
    const auto capsuleComponent = [&] {
        const ColliderComponent* result = nullptr;
        scene.registryForTest().view<ColliderComponent>([&](const Entity, const ColliderComponent& value) {
            result = &value;
        });
        return result;
    }();
    const auto& capsule = std::get<CapsuleCollider>(capsuleComponent->shape);
    if (!near(capsule.radius, 0.4f) || !near(capsule.height, 1.8f)) return 6;

    actor.setPerspectiveCamera(200.0f, -1.0f, 0.01f);
    actor.setCameraAspectRatio(1920.0f, 1080.0f);
    const auto& camera = scene.find(actor.id())->camera();
    if (!actor.hasCamera() || !camera.isPerspective() || !near(camera.fieldOfView, 179.0f) ||
        !near(camera.nearClip, 0.0001f) || camera.farClip <= camera.nearClip ||
        !near(camera.aspectRatio, 16.0f / 9.0f)) return 7;
    actor.setOrthographicCamera(-2.0f, 5.0f, 1.0f);
    actor.setPrimaryCamera(false);
    const auto& orthographic = scene.find(actor.id())->camera();
    if (!orthographic.isOrthographic() || !near(orthographic.orthographicSize, 0.0001f) ||
        !near(orthographic.nearClip, 5.0f) || orthographic.farClip <= orthographic.nearClip || orthographic.primary) return 8;

    actor.addLight();
    actor.setLightType(LightType::Spot);
    actor.setLightColor({0.1f, 0.2f, 0.3f, 1.0f});
    actor.setLightIntensity(12.0f);
    actor.setLightEnabled(false);
    actor.setLightCastShadows(false);
    const auto& light = scene.find(actor.id())->light();
    if (!actor.hasLight() || light.type != LightType::Spot || !near(light.color.r(), 0.1f) ||
        !near(light.color.g(), 0.2f) || !near(light.color.b(), 0.3f) || !near(light.intensity, 12.0f) ||
        light.enabled || light.castShadows) return 9;

    actor.setName("Hero");
    if (actor.name() != "Hero" || scene.find("Player") != nullptr || scene.find("Hero") == nullptr) return 10;
    const Actor copy = scene.duplicate(actor);
    if (!copy.valid() || copy.name() != "Hero 2" || scene.objectCount() != 2 || copy.id() == actor.id()) return 11;
    actor.destroy();
    if (actor.valid() || scene.find("Hero") != nullptr || !copy.valid() || scene.objectCount() != 1) return 12;

    try {
        static_cast<void>(scene.create(""));
        return 13;
    } catch (const std::invalid_argument&) {
    }
    return 0;
}
