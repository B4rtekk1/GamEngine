#include <Engine/Core/Transform.h>
#include <Engine/ECS/Registry.h>
#include <Engine/ECS/Components/ScriptComponent.h>
#include <Engine/ECS/Components/ColliderComponent.h>
#include <Engine/ECS/Components/RigidbodyComponent.h>
#include <Engine/ECS/Components/SmokeEmitterComponent.h>
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
    sharedCube->vertices.front().tangent = {1.0F, 0.0F, 0.0F, -1.0F};
    sharedCube->vertices.front().materialIndex = 0;
    sharedCube->materials.push_back(PBRMaterial{
        .baseColor = {0.2F, 0.3F, 0.4F, 0.5F}, .metallic = 0.7F,
        .roughness = 0.35F, .ambientOcclusion = 0.8F, .baseColorTexture = 0,
        .metallicRoughnessTexture = 0, .normalTexture = 0, .normalScale = 0.6F,
        .alphaBlend = true, .doubleSided = true, .alphaCutoff = 0.25F,
    });
    sharedCube->images.push_back(Mesh::Image{.width = 1, .height = 1,
                                               .rgbaPixels = {10, 20, 30, 40}});

    const Entity first = source.create();
    source.add<NameComponent>(first, NameComponent{.value = "Root"});
    source.add<UUIDComponent>(first, UUIDComponent{.value = 100});
    source.add<Transform>(first, Transform{
        .position = {1.25F, -2.5F, 3.75F},
        .rotation = {10.0F, 20.0F, 30.0F},
        .scale = {2.0F, 3.0F, 4.0F},
    });
    source.add<MeshRenderer>(first, MeshRenderer{
        .mesh = sharedCube,
        .material = {
            .baseColor = {0.1F, 0.2F, 0.3F},
            .metallic = 0.8F,
            .roughness = 0.4F,
            .ambientOcclusion = 0.9F,
        },
        .castShadow = false,
    });
    source.add<LightComponent>(first, LightComponent{
        .type = LightType::Spot,
        .color = {0.7F, 0.6F, 0.5F},
        .intensity = 12.5F,
        .enabled = false,
        .castShadows = true,
    });
    source.add<ScriptComponent>(first, ScriptComponent{"PlayerController", false});
    source.add<ColliderComponent>(first, ColliderComponent{
        .shape = CapsuleCollider{0.35F, 1.8F}, .offset = {0.0F, 0.9F, 0.0F},
        .isTrigger = true, .friction = 0.25F, .restitution = 0.6F});
    source.add<RigidbodyComponent>(first, RigidbodyComponent{
        .type = RigidbodyType::Dynamic, .mass = 2.0F, .useGravity = false,
        .linearVelocity = {1.0F, 2.0F, 3.0F}});

    const Entity second = source.create();
    source.add<NameComponent>(second, NameComponent{.value = "Child"});
    source.add<UUIDComponent>(second, UUIDComponent{.value = 101});
    source.add<ParentComponent>(second, ParentComponent{.parent = 100});
    source.add<MeshRenderer>(second, MeshRenderer{.mesh = sharedCube});
    Particles::SmokeEmitter smoke;
    smoke.buoyancy = 7.25F;
    smoke.drag = 0.8F;
    smoke.turbulence = 1.6F;
    smoke.collisionRadius = 0.18F;
    source.add<SmokeEmitterComponent>(second, SmokeEmitterComponent{.emitter = smoke});
    static_cast<void>(source.create()); // Entities without components are preserved.

    std::stringstream serialized;
    SceneSerializer::save(source, serialized);
    if (serialized.str().find("GAMENGINE_SCENE 7") == std::string::npos ||
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
        !loaded.has<SmokeEmitterComponent>(2) ||
        loaded.has<Transform>(2) || !loaded.has<MeshRenderer>(2) ||
        loaded.has<Transform>(3) || loaded.has<MeshRenderer>(3)) {
        return 2;
    }
    const auto& collider = loaded.get<ColliderComponent>(1);
    const auto& capsule = std::get<CapsuleCollider>(collider.shape);
    if (!collider.isTrigger || !equal(collider.offset, {0.0F, 0.9F, 0.0F}) ||
        capsule.radius != 0.35F || capsule.height != 1.8F ||
        collider.friction != 0.25F || collider.restitution != 0.6F) return 11;
    const auto& rigidbody = loaded.get<RigidbodyComponent>(1);
    if (rigidbody.type != RigidbodyType::Dynamic || rigidbody.mass != 2.0F ||
        rigidbody.useGravity || !equal(rigidbody.linearVelocity, {1.0F, 2.0F, 3.0F})) return 12;
    if (!loaded.has<NameComponent>(1) || !loaded.has<UUIDComponent>(1) ||
        loaded.get<NameComponent>(1).value != "Root" ||
        loaded.get<UUIDComponent>(1).value != 100 ||
        !loaded.has<ParentComponent>(2) ||
        loaded.get<NameComponent>(2).value != "Child" ||
        loaded.get<UUIDComponent>(2).value != 101 ||
        loaded.get<ParentComponent>(2).parent != 100) {
        return 10;
    }
    const auto& loadedSmoke = loaded.get<SmokeEmitterComponent>(2).emitter;
    if (loadedSmoke.buoyancy != 7.25F || loadedSmoke.drag != 0.8F ||
        loadedSmoke.turbulence != 1.6F || loadedSmoke.collisionRadius != 0.18F) {
        return 13;
    }

    const Transform& transform = loaded.get<Transform>(1);
    if (!equal(transform.position, {1.25F, -2.5F, 3.75F}) ||
        !equal(transform.rotation, {10.0F, 20.0F, 30.0F}) ||
        !equal(transform.scale, {2.0F, 3.0F, 4.0F})) {
        return 3;
    }

    const MeshRenderer& firstRenderer = loaded.get<MeshRenderer>(1);
    const MeshRenderer& secondRenderer = loaded.get<MeshRenderer>(2);
    if (!firstRenderer.hasMesh() || firstRenderer.mesh != secondRenderer.mesh ||
        firstRenderer.mesh->vertices.size() != sharedCube->vertices.size() ||
        firstRenderer.mesh->indices != sharedCube->indices || firstRenderer.castShadow ||
        !equal(firstRenderer.material.baseColor, {0.1F, 0.2F, 0.3F}) ||
        firstRenderer.material.metallic != 0.8F ||
        firstRenderer.material.roughness != 0.4F ||
        firstRenderer.material.ambientOcclusion != 0.9F) {
        return 4;
    }
    if (firstRenderer.mesh->materials.size() != 1 || firstRenderer.mesh->images.size() != 1 ||
        firstRenderer.mesh->images.front().rgbaPixels != std::vector<std::uint8_t>{10, 20, 30, 40} ||
        firstRenderer.mesh->vertices.front().tangent.w() != -1.0F ||
        firstRenderer.mesh->vertices.front().materialIndex != 0 ||
        !firstRenderer.mesh->materials.front().alphaBlend ||
        !firstRenderer.mesh->materials.front().doubleSided ||
        firstRenderer.mesh->materials.front().baseColor.a() != 0.5F) {
        return 8;
    }

    const LightComponent& light = loaded.get<LightComponent>(1);
    if (light.type != LightType::Spot || !equal(light.color, {0.7F, 0.6F, 0.5F}) ||
        light.intensity != 12.5F || light.enabled || !light.castShadows) {
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