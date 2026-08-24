#include <Engine/Renderer/Geometry/Cube.h>
#include <Engine/Scene/Scene.h>

#include <memory>
#include <stdexcept>

int main() {
    using namespace Engine;

    Scene scene;
    Actor first = scene.createActor("Enemy");
    Actor second = scene.createActor("Enemy");
    if (first.name() != "Enemy" || second.name() != "Enemy 2" ||
        scene.objectCount() != 2 || scene.findActor("Enemy 2").id() != second.id()) return 1;

    first.setName("Boss");
    if (scene.find("Enemy") != nullptr || scene.findActor("Boss").id() != first.id()) return 2;
    try {
        first.setName("Enemy 2");
        return 3;
    } catch (const std::invalid_argument&) {
    }

    const auto mesh = std::make_shared<Mesh>(Cube::createMesh());
    auto& meshObject = scene.createMeshObject("Mesh", mesh);
    if (!meshObject.isRenderable() || meshObject.meshRenderer().mesh != mesh ||
        scene.objectCount() != 3) return 4;

    Actor camera = scene.createCameraActor("Camera");
    if (!camera.hasCamera() || !scene.find("Camera")->camera().primary) return 5;
    camera.destroy();
    if (camera.valid() || scene.find("Camera") == nullptr || scene.objectCount() != 4) return 6;

    Actor primaryCamera = scene.createCameraActor("Primary");
    if (scene.objectCount() != 5) return 7;
    primaryCamera.setPrimaryCamera(false);
    Actor primaryHandle = scene.findActor("Primary");
    primaryHandle.destroy();
    if (primaryHandle.valid() || scene.find("Primary") != nullptr || scene.objectCount() != 4) return 8;

    Actor duplicate = scene.duplicate(first);
    if (!duplicate.valid() || duplicate.name() != "Boss 2" || duplicate.id() == first.id() ||
        scene.objectCount() != 5) return 9;
    duplicate.destroy();
    if (scene.objectCount() != 4) return 10;

    Actor foreign;
    if (scene.duplicate(foreign).valid() || scene.findActor("Missing").valid()) return 11;

    first.destroy();
    second.destroy();
    if (scene.objectCount() != 2 || scene.find("Boss") != nullptr || scene.find("Enemy 2") != nullptr) return 12;
    return 0;
}
