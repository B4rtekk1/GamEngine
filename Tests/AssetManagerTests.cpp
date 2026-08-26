#include <Engine/Assets/AssetManager.h>
#include <Engine/Renderer/Geometry/Mesh.h>

#include <cassert>
#include <cstdint>
#include <fstream>
#include <vector>

int main() {
    const auto path = std::filesystem::temp_directory_path() / "gamengine_blender_object.obj";
    {
        std::ofstream file(path);
        file << "# Blender OBJ export\n"
             << "v 0 0 0\n"
             << "v 1 0 0\n"
             << "v 1 1 0\n"
             << "v 0 1 0\n"
             << "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
             << "f 1/1 2/2 3/3 4/4\n";
    }

    Engine::Assets::AssetManager assets;
    Engine::Assets::register_default_asset_loaders(assets);
    const auto mesh = assets.load<Engine::Mesh>(path, Engine::Assets::AssetType::Mesh);
    assert(mesh);
    assert(mesh->vertices.size() == 4);
    assert(mesh->indices.size() == 6);
    assert(mesh->vertices.front().normal.z() > 0.99F);

    std::filesystem::remove(path);
}
