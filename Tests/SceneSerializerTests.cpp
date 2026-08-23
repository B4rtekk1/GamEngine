#include <Engine/Core/Transform.h>
#include <Engine/ECS/Registry.h>
#include <Engine/ECS/Components/ScriptComponent.h>
#include <Engine/ECS/Components/ColliderComponent.h>
#include <Engine/ECS/Components/RigidbodyComponent.h>
#include <Engine/Renderer/Geometry/Cube.h>
#include <Engine/Renderer/MeshRenderer.h>
#include <Engine/Scene/Components/LightComponent.h>
#include <Engine/Scene/Components/IdentityComponents.h>
#include <Engine/Scene/SceneSerializer.h>

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool equal(const Engine::Vec3& lhs, const Engine::Vec3& rhs) {
    return lhs.x() == rhs.x() && lhs.y() == rhs.y() && lhs.z() == rhs.z();
}

bool equal(const Engine::Math::Color& lhs, const Engine::Math::Color& rhs) {
    return lhs.r() == rhs.r() && lhs.g() == rhs.g() && lhs.b() == rhs.b() &&
           lhs.a() == rhs.a();
}

} // namespace

int main() {
    using namespace Engine;

    Registry source;
    const auto sharedCube = std::make_shared<Mesh>(Cube::createMesh());
    sharedCube->vertices.front().tangent = {1.0f, 0.0f, 0.0f, -1.0f};
    sharedCube->vertices.front().materialIndex = 0;
    sharedCube->materials.push_back(PBRMaterial{
        .baseColor = {0.2f, 0.3f, 0.4f, 0.5f}, .metallic = 0.7f,
        .roughness = 0.35f, .ambientOcclusion = 0.8f, .baseColorTexture = 0,
        .metallicRoughnessTexture = 0, .normalTexture = 0, .normalScale = 0.6f,
        .alphaBlend = true, .doubleSided = true, .alphaCutoff = 0.25f,
    });
    sharedCube->images.push_back(Mesh::Image{.width = 1, .height = 1,
                                               .rgbaPixels = {10, 20, 30, 40}});

    const Entity first = source.create();
    source.add<NameComponent>(first, NameComponent{.value = "Root"});
    source.add<UUIDComponent>(first, UUIDComponent{.value = 100});
    source.add<Transform>(first, Transform{
        .position = {1.25f, -2.5f, 3.75f},
        .rotation = {10.0f, 20.0f, 30.0f},
        .scale = {2.0f, 3.0f, 4.0f},
    });
    source.add<MeshRenderer>(first, MeshRenderer{
        .mesh = sharedCube,
        .material = {
            .baseColor = {0.1f, 0.2f, 0.3f},
            .metallic = 0.8f,
            .roughness = 0.4f,
            .ambientOcclusion = 0.9f,
        },
        .castShadow = false,
    });
    source.add<LightComponent>(first, LightComponent{
        .type = LightType::Spot,
        .color = {0.7f, 0.6f, 0.5f},
        .intensity = 12.5f,
        .enabled = false,
        .castShadows = true,
    });
    source.add<ScriptComponent>(first, ScriptComponent{"PlayerController", false});
    source.add<ColliderComponent>(first, ColliderComponent{
        .shape = CapsuleCollider{0.35f, 1.8f}, .offset = {0.0f, 0.9f, 0.0f},
        .isTrigger = true, .friction = 0.25f, .restitution = 0.6f});
    source.add<RigidbodyComponent>(first, RigidbodyComponent{
        .type = RigidbodyType::Dynamic, .mass = 2.0f, .useGravity = false,
        .linearVelocity = {1.0f, 2.0f, 3.0f}});

    const Entity second = source.create();
    source.add<NameComponent>(second, NameComponent{.value = "Child"});
    source.add<UUIDComponent>(second, UUIDComponent{.value = 101});
    source.add<ParentComponent>(second, ParentComponent{.parent = 100});
    source.add<MeshRenderer>(second, MeshRenderer{.mesh = sharedCube});
    static_cast<void>(source.create()); // Entities without components are preserved.

    std::stringstream serialized;
    SceneSerializer::save(source, serialized);
    if (serialized.str().find("GAMENGINE_SCENE 6") == std::string::npos ||
        serialized.str().find("MESHES 1") == std::string::npos ||
        serialized.str().find("ENTITIES 3") == std::string::npos) {
        return 1;
    }

    Registry loaded;
    SceneSerializer::load(loaded, serialized);

    std::vector<Entity> entities;
    loaded.view<>([&entities](const Entity entity) { entities.push_back(entity); });
    if (entities.size() != 3 || !loaded.has<Transform>(1) ||
        !loaded.has<MeshRenderer>(1) || !loaded.has<LightComponent>(1) ||
        !loaded.has<ScriptComponent>(1) ||
        !loaded.has<ColliderComponent>(1) ||
        !loaded.has<RigidbodyComponent>(1) ||
        loaded.has<Transform>(2) || !loaded.has<MeshRenderer>(2) ||
        loaded.has<Transform>(3) || loaded.has<MeshRenderer>(3)) {
        return 2;
    }
    const auto& collider = loaded.get<ColliderComponent>(1);
    const auto& capsule = std::get<CapsuleCollider>(collider.shape);
    if (!collider.isTrigger || !equal(collider.offset, {0.0f, 0.9f, 0.0f}) ||
        capsule.radius != 0.35f || capsule.height != 1.8f ||
        collider.friction != 0.25f || collider.restitution != 0.6f) return 11;
    const auto& rigidbody = loaded.get<RigidbodyComponent>(1);
    if (rigidbody.type != RigidbodyType::Dynamic || rigidbody.mass != 2.0f ||
        rigidbody.useGravity || !equal(rigidbody.linearVelocity, {1.0f, 2.0f, 3.0f})) return 12;
    if (!loaded.has<NameComponent>(1) || !loaded.has<UUIDComponent>(1) ||
        loaded.get<NameComponent>(1).value != "Root" ||
        loaded.get<UUIDComponent>(1).value != 100 ||
        !loaded.has<ParentComponent>(2) ||
        loaded.get<NameComponent>(2).value != "Child" ||
        loaded.get<UUIDComponent>(2).value != 101 ||
        loaded.get<ParentComponent>(2).parent != 100) {
        return 10;
    }

    const Transform& transform = loaded.get<Transform>(1);
    if (!equal(transform.position, {1.25f, -2.5f, 3.75f}) ||
        !equal(transform.rotation, {10.0f, 20.0f, 30.0f}) ||
        !equal(transform.scale, {2.0f, 3.0f, 4.0f})) {
        return 3;
    }

    const MeshRenderer& firstRenderer = loaded.get<MeshRenderer>(1);
    const MeshRenderer& secondRenderer = loaded.get<MeshRenderer>(2);
    if (!firstRenderer.hasMesh() || firstRenderer.mesh != secondRenderer.mesh ||
        firstRenderer.mesh->vertices.size() != sharedCube->vertices.size() ||
        firstRenderer.mesh->indices != sharedCube->indices || firstRenderer.castShadow ||
        !equal(firstRenderer.material.baseColor, {0.1f, 0.2f, 0.3f}) ||
        firstRenderer.material.metallic != 0.8f ||
        firstRenderer.material.roughness != 0.4f ||
        firstRenderer.material.ambientOcclusion != 0.9f) {
        return 4;
    }
    if (firstRenderer.mesh->materials.size() != 1 || firstRenderer.mesh->images.size() != 1 ||
        firstRenderer.mesh->images.front().rgbaPixels != std::vector<std::uint8_t>{10, 20, 30, 40} ||
        firstRenderer.mesh->vertices.front().tangent.w() != -1.0f ||
        firstRenderer.mesh->vertices.front().materialIndex != 0 ||
        !firstRenderer.mesh->materials.front().alphaBlend ||
        !firstRenderer.mesh->materials.front().doubleSided ||
        firstRenderer.mesh->materials.front().baseColor.a() != 0.5f) {
        return 8;
    }

    const LightComponent& light = loaded.get<LightComponent>(1);
    if (light.type != LightType::Spot || !equal(light.color, {0.7f, 0.6f, 0.5f}) ||
        light.intensity != 12.5f || light.enabled || !light.castShadows) {
        return 5;
    }

    const ScriptComponent& script = loaded.get<ScriptComponent>(1);
    if (script.className != "PlayerController" || script.enabled) {
        return 9;
    }

    Registry unchangedAfterFailure;
    const Entity existing = unchangedAfterFailure.create();
    unchangedAfterFailure.add<Transform>(existing);
    std::stringstream invalid{
        "GAMENGINE_SCENE 1\nMESHES 0\nENTITIES 1\n"
        "ENTITY\nUNKNOWN_COMPONENT\nEND_ENTITY\nEND_SCENE\n"};
    try {
        SceneSerializer::load(unchangedAfterFailure, invalid);
        return 6;
    } catch (const std::runtime_error&) {
    }
    if (!unchangedAfterFailure.valid(existing) ||
        !unchangedAfterFailure.has<Transform>(existing)) {
        return 7;
    }

    return 0;
}
