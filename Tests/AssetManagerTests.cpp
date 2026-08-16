#include <Engine/Assets/AssetManager.h>
#include <Engine/Renderer/Geometry/Mesh.h>

#include <cassert>
#include <cstdint>
#include <cstring>
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
    assert(mesh->vertices.front().normal.z() > 0.99f);

    const auto glb_path = std::filesystem::temp_directory_path() / "gamengine_triangle.glb";
    // A Blender export places geometry through scene nodes; the second node is
    // deliberately not reachable from the active scene and must not be loaded.
    const std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0,"translation":[3,2,1],"scale":[2,1,1]},{"mesh":0,"translation":[100,0,0]}],"buffers":[{"byteLength":44}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}]})";
    std::vector<std::uint8_t> bin(44);
    const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::uint16_t indices[] = {0, 1, 2};
    std::memcpy(bin.data(), positions, sizeof(positions));
    std::memcpy(bin.data() + 36, indices, sizeof(indices));
    const auto pad4 = [](std::size_t value) { return (value + 3u) & ~3u; };
    std::vector<std::uint8_t> glb;
    const auto append_u32 = [&glb](std::uint32_t value) { for (int i = 0; i < 4; ++i) glb.push_back(static_cast<std::uint8_t>(value >> (i * 8))); };
    const auto json_size = pad4(json.size());
    append_u32(0x46546C67); append_u32(2); append_u32(static_cast<std::uint32_t>(12 + 8 + json_size + 8 + bin.size()));
    append_u32(static_cast<std::uint32_t>(json_size)); append_u32(0x4E4F534A); glb.insert(glb.end(), json.begin(), json.end()); glb.resize(glb.size() + json_size - json.size(), ' ');
    append_u32(static_cast<std::uint32_t>(bin.size())); append_u32(0x004E4942); glb.insert(glb.end(), bin.begin(), bin.end());
    { std::ofstream file(glb_path, std::ios::binary); file.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size())); }
    const auto glb_mesh = assets.load<Engine::Mesh>(glb_path, Engine::Assets::AssetType::Mesh);
    assert(glb_mesh && glb_mesh->vertices.size() == 3 && glb_mesh->indices.size() == 3);
    assert(glb_mesh->vertices[0].position.x() == 3.0f && glb_mesh->vertices[0].position.y() == 2.0f);
    assert(glb_mesh->vertices[1].position.x() == 5.0f);

    const auto treePath = std::filesystem::path{__FILE__}.parent_path().parent_path() / "Assets/Models/tree.glb";
    const auto tree = assets.load<Engine::Mesh>(treePath, Engine::Assets::AssetType::Mesh);
    assert(tree && tree->materials.size() == 2);
    assert(tree->vertices.size() > 3716);
    assert(tree->vertices.front().materialIndex == 0);
    assert(tree->vertices.front().tangent.length() > 0.99f);
    assert(tree->vertices[3716].materialIndex == 1);
    assert(tree->images.size() == 3);
    assert(tree->images.front().width > 0 && !tree->images.front().rgbaPixels.empty());
    assert(tree->materials[1].baseColorTexture == 2);
    assert(tree->materials[1].normalTexture == 1);
    assert(tree->materials[1].normalScale == 0.0f);
    assert(tree->materials[1].alphaBlend);
    assert(tree->materials[1].doubleSided);

    std::filesystem::remove(path);
    std::filesystem::remove(glb_path);
}
