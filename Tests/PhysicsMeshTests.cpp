#include <gtest/gtest.h>

#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Scene/Scene.h"

#include <cmath>
#include <memory>

namespace {

std::shared_ptr<const Engine::Mesh> offsetTallMesh() {
    auto mesh = std::make_shared<Engine::Mesh>();
    for (const Engine::Vec3 position : {
             Engine::Vec3{1.0F, -0.25F, -0.5F}, Engine::Vec3{3.0F, -0.25F, -0.5F},
             Engine::Vec3{3.0F,  7.40F, -0.5F}, Engine::Vec3{1.0F,  7.40F, -0.5F},
             Engine::Vec3{1.0F, -0.25F,  0.5F}, Engine::Vec3{3.0F, -0.25F,  0.5F},
             Engine::Vec3{3.0F,  7.40F,  0.5F}, Engine::Vec3{1.0F,  7.40F,  0.5F}}) {
        mesh->vertices.push_back(Engine::Vertex{.position = position});
    }
    mesh->indices = {
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
        0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2,
        0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5
    };
    return mesh;
}

TEST(PhysicsMesh, DynamicMeshUsesTightBoundsAndPreservesCenterOfMass) {
    Engine::Scene scene;
    const Engine::Actor ground = scene.createActor("Ground");
    ground.setPosition({2.0F, 0.0F, 0.0F});
    ground.addBoxCollider({5.0F, 0.5F, 5.0F});

    const Engine::Actor object = scene.createActor("Dynamic mesh");
    object.setMesh(offsetTallMesh());
    object.setPosition({0.0F, 3.0F, 0.0F});
    object.addMeshCollider();
    object.addRigidbody();

    Engine::PhysicsSystem physics;
    for (int step = 0; step < 240; ++step) physics.update(scene, 1.0F / 60.0F);

    EXPECT_LT(std::abs(object.position().x()), 0.1F);
    EXPECT_GT(object.position().y(), 0.70F);
    EXPECT_LT(object.position().y(), 2.0F);
    EXPECT_LT(object.velocity().length(), 1e-2F);
    const Engine::Vec3 rotation = object.rotation();
    EXPECT_LT(std::abs(std::remainder(rotation.x(), 360.0F)), 10.0F);
    EXPECT_LT(std::abs(std::remainder(rotation.y(), 360.0F)), 10.0F);
    EXPECT_LT(std::abs(std::remainder(rotation.z(), 360.0F)), 10.0F);
}

} // namespace
