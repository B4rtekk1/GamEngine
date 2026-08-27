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

bool near(const float lhs, const float rhs, const float epsilon = 0.0001F) {
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

    actor.setPosition({1.0F, 2.0F, 3.0F});
    actor.translate({2.0F, -1.0F, 0.5F});
    actor.move({-1.0F, 1.0F, -0.5F});
    actor.setRotation({10.0F, 20.0F, 30.0F});
    actor.setScale({2.0F, 3.0F, 4.0F});
    if (!equal(actor.position(), {2.0F, 2.0F, 3.0F}) ||
        !equal(actor.rotation(), {10.0F, 20.0F, 30.0F}) ||
        !equal(actor.scale(), {2.0F, 3.0F, 4.0F})) return 2;

    actor.addRigidbody();
    actor.setBodyType(RigidbodyType::Kinematic);
    actor.setMass(4.0F);
    actor.setGravityEnabled(false);
    actor.setVelocity({3.0F, 4.0F, 5.0F});
    const auto& body = scene.find(actor.id())->rigidbody();
    if (!actor.hasRigidbody() || body.type != RigidbodyType::Kinematic ||
        !near(body.mass, 4.0F) || body.useGravity || !equal(actor.velocity(), {3.0F, 4.0F, 5.0F})) return 3;

    actor.addBoxCollider({1.0F, 2.0F, 3.0F});
    actor.setColliderTrigger(true);
    actor.setColliderMaterial(0.25F, 0.75F);
    const auto collider = [&] {
        const ColliderComponent* result = nullptr;
        scene.registryForTest().view<ColliderComponent>([&](const Entity, const ColliderComponent& value) {
            result = &value;
        });
        return result;
    }();
    const auto& box = std::get<BoxCollider>(collider->shape);
    if (!actor.hasCollider() || !equal(box.halfExtents, {1.0F, 2.0F, 3.0F}) ||
        !collider->isTrigger || !near(collider->friction, 0.25F) ||
        !near(collider->restitution, 0.75F)) return 4;

    actor.addSphereCollider(2.0F);
    const auto sphere = [&] {
        const ColliderComponent* result = nullptr;
        scene.registryForTest().view<ColliderComponent>([&](const Entity, const ColliderComponent& value) {
            result = &value;
        });
        return result;
    }();
    if (!near(std::get<SphereCollider>(sphere->shape).radius, 2.0F)) return 5;
    actor.addCapsuleCollider(0.4F, 1.8F);
    const auto capsuleComponent = [&] {
        const ColliderComponent* result = nullptr;
        scene.registryForTest().view<ColliderComponent>([&](const Entity, const ColliderComponent& value) {
            result = &value;
        });
        return result;
    }();
    const auto& capsule = std::get<CapsuleCollider>(capsuleComponent->shape);
    if (!near(capsule.radius, 0.4F) || !near(capsule.height, 1.8F)) return 6;

    auto mesh = std::make_shared<Mesh>();
    mesh->vertices = {
        {.position = {-3.0F, 0.0F, 0.0F}},
        {.position = {2.0F, 0.0F, 0.0F}},
        {.position = {0.0F, 1.0F, 0.0F}},
    };
    mesh->indices = {0, 1, 2};
    actor.setMesh(mesh);
    actor.addMeshCollider();
    const auto& meshCollider = std::get<MeshCollider>([&] {
        const ColliderComponent* result = nullptr;
        scene.registryForTest().view<ColliderComponent>([&](const Entity, const ColliderComponent& value) {
            result = &value;
        });
        return result->shape;
    }());
    if (meshCollider.mesh != mesh || meshCollider.mesh->indices.size() != 3) return 7;

    actor.setPerspectiveCamera(200.0F, -1.0F, 0.01F);
    actor.setCameraAspectRatio(1920.0F, 1080.0F);
    const auto& camera = scene.find(actor.id())->camera();
    if (!actor.hasCamera() || !camera.isPerspective() || !near(camera.fieldOfView, 179.0F) ||
        !near(camera.nearClip, 0.0001F) || camera.farClip <= camera.nearClip ||
        !near(camera.aspectRatio, 16.0F / 9.0F)) return 7;
    actor.setOrthographicCamera(-2.0F, 5.0F, 1.0F);
    actor.setPrimaryCamera(false);
    const auto& orthographic = scene.find(actor.id())->camera();
    if (!orthographic.isOrthographic() || !near(orthographic.orthographicSize, 0.0001F) ||
        !near(orthographic.nearClip, 5.0F) || orthographic.farClip <= orthographic.nearClip || orthographic.primary) return 8;

    actor.addLight();
    actor.setLightType(LightType::Spot);
    actor.setLightColor({0.1F, 0.2F, 0.3F, 1.0F});
    actor.setLightIntensity(12.0F);
    actor.setLightEnabled(false);
    actor.setLightCastShadows(false);
    const auto& light = scene.find(actor.id())->light();
    if (!actor.hasLight() || light.type != LightType::Spot || !near(light.color.r(), 0.1F) ||
        !near(light.color.g(), 0.2F) || !near(light.color.b(), 0.3F) || !near(light.intensity, 12.0F) ||
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
