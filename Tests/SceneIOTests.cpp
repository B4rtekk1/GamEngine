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
    player.setPosition({4.0f, 5.0f, 6.0f});
    player.setScale({2.0f, 3.0f, 4.0f});
    player.addRigidbody(RigidbodyComponent{.mass = 3.0f, .useGravity = false});
    player.addBoxCollider({0.25f, 0.5f, 0.75f});
    player.setColliderMaterial(0.2f, 0.8f);
    player.addScript("PlayerScript", false);
    const Actor empty = source.createActor("Empty");
    source.save(scenePath);
    if (!std::filesystem::is_regular_file(scenePath) || std::filesystem::file_size(scenePath) == 0) return 1;

    Scene loaded;
    loaded.load(scenePath);
    if (loaded.objectCount() != 2 || !loaded.findActor("Player").valid() ||
        !loaded.findActor("Empty").valid() || loaded.findActor("Missing").valid()) return 2;
    const Actor loadedPlayer = loaded.findActor("Player");
    if (loadedPlayer.position().x() != 4.0f || loadedPlayer.position().y() != 5.0f ||
        loadedPlayer.position().z() != 6.0f || loadedPlayer.scale().z() != 4.0f ||
        !loadedPlayer.hasRigidbody() || !loadedPlayer.hasCollider()) return 3;

    Scene unchanged;
    const Actor existing = unchanged.createActor("Existing");
    try {
        unchanged.load(invalidPath);
        return 4;
    } catch (const std::runtime_error&) {
    }
    (void)existing;

    std::filesystem::remove_all(directory);
    return 0;
}
