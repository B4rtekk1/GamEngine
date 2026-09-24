#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <cstdint>

namespace Engine {
    class Mesh;
}

namespace Engine::Assets {
    struct GltfPhaseTimings;
    struct TextureCookSummary;
    /** Loads a cooked mesh and its referenced .gtex textures. */
    [[nodiscard]] std::shared_ptr<const Mesh> load_gmesh(const std::filesystem::path &path);

    /** Loads only geometry and meshlet payload; materials, sections, and image references are skipped. */
    [[nodiscard]] std::shared_ptr<Mesh> load_gmesh_geometry(const std::filesystem::path &path);

    /** Writes geometry, materials, and relative .gtex references to a .gmesh file. */
    [[nodiscard]] bool save_gmesh(const std::filesystem::path &path, const Mesh &mesh,
                                  GltfPhaseTimings *timings = nullptr);

    /** Whether a cooked mesh uses the current texture-reference layout. */
    [[nodiscard]] bool current_gmesh_version(const std::filesystem::path &path);

    /** Content key for a glTF and all source files it depends on. */
    [[nodiscard]] std::optional<std::uint64_t> gltf_cook_key(const std::filesystem::path &source);

    /** Checks the mesh key and validates its referenced cooked textures. */
    [[nodiscard]] bool current_gltf_mesh(const std::filesystem::path &source,
                                         const std::filesystem::path &cooked);

    /** Imports one glTF source, cooks its images, and writes a sibling .gmesh. */
    [[nodiscard]] bool cook_gltf_mesh(const std::filesystem::path &source,
                                      const std::filesystem::path &cacheRoot = {},
                                      TextureCookSummary *summary = nullptr);
} // namespace Engine::Assets
