#include <Editor/EditorState.h>

#include <Engine/Assets/Content.h>

#include <filesystem>

int main() {
    Engine::ScenePreset scene{Engine::SceneType::Particles};
    Engine::Assets::Content content{
        std::filesystem::path{GAMEENGINE_SOURCE_DIR} / "Assets"};
    static_cast<void>(scene.createModel("Tree", "Models/tree.glb", content));

    const std::string serialized = Editor::serializeScene(scene);
    if (serialized.find("MESH_ASSET") == std::string::npos ||
        serialized.find("PIXELS") != std::string::npos) return 1;

    Editor::SceneHistory history;
    history.reset(scene);
    // An unchanged GLB scene must take the revision fast path and avoid
    // serializing its geometry and decoded image pixels again.
    if (history.capture(scene)) return 2;

    auto* tree = scene.find("Tree");
    if (tree == nullptr) return 3;
    tree->setPosition({1.0F, 0.0F, 0.0F});
    if (!history.capture(scene) || history.capture(scene) || !history.canUndo()) return 4;
    return 0;
}
