#include <Engine/Physics/PhysicsSystem.h>
#include <Engine/Renderer/Geometry/Mesh.h>
#include <Engine/Scene/Scene.h>

#include <memory>

int main() {
    using namespace Engine;

    // A glTF MeshCollider is made of triangles. Boxes must use those
    // triangles too; treating only spheres as mesh-contact candidates lets a
    // dynamic cube pass straight through vertical parts such as a tree trunk.
    Scene scene;
    auto wallMesh = std::make_shared<Mesh>();
    wallMesh->vertices = {
        Vertex{.position = {0.0F, -2.0F, -2.0F}},
        Vertex{.position = {0.0F,  2.0F, -2.0F}},
        Vertex{.position = {0.0F,  2.0F,  2.0F}},
        Vertex{.position = {0.0F, -2.0F,  2.0F}}
    };
    wallMesh->indices = {0, 1, 2, 0, 2, 3};
    auto meshWall = scene.createActor("Triangle wall");
    meshWall.setMesh(wallMesh);
    meshWall.addMeshCollider();

    auto strikingBox = scene.createActor("Mesh striking box");
    strikingBox.setPosition({-1.0F, 0.0F, 0.0F});
    strikingBox.addRigidbody(RigidbodyComponent{
        .linearDamping = 0.0F, .angularDamping = 0.0F, .useGravity = false,
        .linearVelocity = {5.0F, 0.0F, 0.0F}
    });
    strikingBox.addBoxCollider({0.5F, 0.5F, 0.5F});

    PhysicsSystem physics;
    physics.update(scene, 0.1F);
    if (strikingBox.position().x() > -0.499F ||
        strikingBox.velocity().x() > 0.001F) {
        return 1;
    }

    // A small tangential velocity left by a mesh impact must not make a box
    // creep forever once it is resting on flat ground. Slopes deliberately
    // retain their tangential component so gravity can keep them moving.
    Scene restingScene;
    auto ground = restingScene.createActor("Ground");
    ground.addBoxCollider({10.0F, 0.05F, 10.0F});
    auto restingBox = restingScene.createActor("Resting box");
    restingBox.setPosition({0.0F, 0.549F, 0.0F});
    restingBox.addRigidbody(RigidbodyComponent{
        .useGravity = false, .linearVelocity = {0.04F, 0.0F, -0.05F}
    });
    restingBox.addBoxCollider();
    physics.update(restingScene, 1.0F / 120.0F);
    if (restingBox.velocity().length() > 0.001F) return 2;

    return 0;
}
