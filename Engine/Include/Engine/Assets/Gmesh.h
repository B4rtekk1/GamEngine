#pragma once

#include <filesystem>
#include <memory>

namespace Engine { class Mesh; }

namespace Engine::Assets {
/** Loads a cooked mesh and its referenced .gtex textures. */
[[nodiscard]] std::shared_ptr<const Mesh> load_gmesh(const std::filesystem::path& path);

/** Writes geometry, materials, and relative .gtex references to a .gmesh file. */
[[nodiscard]] bool save_gmesh(const std::filesystem::path& path, const Mesh& mesh);

/** Imports one glTF source, cooks its images, and writes a sibling .gmesh. */
[[nodiscard]] bool cook_gltf_mesh(const std::filesystem::path& source);
} // namespace Engine::Assets
