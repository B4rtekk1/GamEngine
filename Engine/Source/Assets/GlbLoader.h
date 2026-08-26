#pragma once

#include <filesystem>
#include <memory>

namespace Engine { class Mesh; }

namespace Engine::Assets {

/**
 * Loads a glTF 2.0 mesh from either a binary `.glb` file or a JSON `.gltf`
 * file. External buffers and images referenced by `.gltf` are resolved
 * relative to `path`.
 */
[[nodiscard]] std::shared_ptr<const Mesh> load_gltf_mesh(const std::filesystem::path& path);

} // namespace Engine::Assets
