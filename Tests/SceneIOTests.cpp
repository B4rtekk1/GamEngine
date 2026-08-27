#include <Engine/Scene/Scene.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>

int main() {
    using namespace Engine;

    const auto directory = std::filesystem::temp_directory_path() / "gamengine_scene_io_tests";
    std::filesystem::create_directories(directory);
    const auto scenePath = directory / "roundtrip.scene";
    const auto invalidPath = directory / "missing" / "scene.scene";

    Scene source;
    Actor player = source.createActor("Player");
    player.setPosition({4.0F, 5.0F, 6.0F});
    player.setScale({2.0F, 3.0F, 4.0F});
    player.addRigidbody(RigidbodyComponent{.mass = 3.0F, .useGravity = false});
    player.addBoxCollider({0.25F, 0.5F, 0.75F});
    player.setColliderMaterial(0.2F, 0.8F);
    player.addScript("PlayerScript", false);
    const Actor empty = source.createActor("Empty");
    source.save(scenePath);
    if (!std::filesystem::is_regular_file(scenePath) || std::filesystem::file_size(scenePath) == 0) return 1;

    Scene loaded;
    loaded.load(scenePath);
    if (loaded.objectCount() != 2 || !loaded.findActor("Player").valid() ||
        !loaded.findActor("Empty").valid() || loaded.findActor("Missing").valid()) return 2;
    const Actor loadedPlayer = loaded.findActor("Player");
    if (loadedPlayer.position().x() != 4.0F || loadedPlayer.position().y() != 5.0F ||
        loadedPlayer.position().z() != 6.0F || loadedPlayer.scale().z() != 4.0F ||
        !loadedPlayer.hasRigidbody() || !loadedPlayer.hasCollider()) return 3;

    Scene unchanged;
    const Actor existing = unchanged.createActor("Existing");
    try {
        unchanged.load(invalidPath);
        return 4;
    } catch (const std::runtime_error&) {
    }
    // A failed load must leave the previously usable scene connected to its
    // GameObject wrappers; otherwise the editor crashes on its next UI frame.
    if (!unchanged.findActor("Existing").valid() ||
        unchanged.findActor("Existing").name() != "Existing") return 5;

    Scene editorScene;
    editorScene.load(std::filesystem::path{GAMEENGINE_SOURCE_DIR} / "Assets/Scenes/Editor.scene");
    if (editorScene.objectCount() != 8 || !editorScene.findActor("Tree").valid()) return 6;

    std::filesystem::remove_all(directory);
    return 0;
}
