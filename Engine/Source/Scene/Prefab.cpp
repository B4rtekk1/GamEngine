#include "Engine/Scene/Prefab.h"

#include "Engine/Assets/Content.h"
#include "Engine/Renderer/Geometry/Cube.h"

#include <stdexcept>

namespace Engine {
    Prefab Prefab::model(Assets::Content &content, std::filesystem::path path,
                         PBRMaterial material) {
        auto mesh = content.mesh(std::move(path));
        if (!mesh) throw std::runtime_error("Could not create prefab: model could not be loaded");
        return Prefab{std::move(mesh), material};
    }

    Prefab Prefab::cube(PBRMaterial material) {
        return Prefab{std::make_shared<Mesh>(Cube::createMesh()), material};
    }
} // namespace Engine
