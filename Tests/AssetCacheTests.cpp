#include <Engine/Assets/AssetManager.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <typeindex>
#include <vector>

int main() {
    using namespace Engine::Assets;

    const auto root = std::filesystem::temp_directory_path() / "gamengine_asset_cache_tests";
    std::filesystem::create_directories(root);
    const auto textPath = root / "message.txt";
    {
        std::ofstream file(textPath, std::ios::binary);
        file << "hello engine";
    }

    AssetManager manager(root);
    std::vector<std::string> errors;
    manager.set_error_handler([&](const std::string& message) { errors.push_back(message); });

    const auto first = manager.loadText("message.txt");
    const auto second = manager.load<TextAsset>(root / "message.txt", AssetType::Text);
    if (!first || !second || first.id() != second.id() || first.get() != second.get() ||
        first->text != "hello engine" || manager.size() != 1 ||
        !manager.contains(first.id(), std::type_index(typeid(TextAsset)))) return 1;

    const auto unchanged = manager.load_if_changed<TextAsset>("message.txt", AssetType::Text);
    if (!unchanged || unchanged.get() != first.get()) return 2;

    const auto empty = manager.loadText("missing.txt");
    if (empty || errors.empty()) return 3;

    const auto id = first.id();
    manager.unload<TextAsset>(id);
    if (manager.size() != 0 || manager.contains(id, std::type_index(typeid(TextAsset)))) return 4;

    const auto reloaded = manager.loadText("message.txt");
    if (!reloaded || reloaded.id() != id || reloaded->text != "hello engine") return 5;
    manager.clear();
    if (manager.size() != 0) return 6;

    AssetHandle<TextAsset> handle{id, std::make_shared<const TextAsset>(TextAsset{"owned"})};
    if (!handle || handle.id() != id || handle->text != "owned" || handle.get() != &*handle) return 7;
    handle.reset();
    if (handle || handle.id() != 0 || handle.get() != nullptr) return 8;

    if (AssetManager::normalize_path("A/../B.TXT") != "b.txt" ||
        AssetManager::make_id("same") != AssetManager::make_id("same") ||
        AssetManager::make_id("same") == AssetManager::make_id("different")) return 9;

    std::filesystem::remove_all(root);
    return 0;
}
