#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <cstdint>
#include <chrono>
#include <functional>
#include <span>

namespace Engine {
    class Mesh;
}

namespace Engine::Assets {
    struct GltfImportTimings {
        std::chrono::nanoseconds parse{};
        std::chrono::nanoseconds imageDecode{};
        std::chrono::nanoseconds geometryImport{};
        std::chrono::nanoseconds meshletBuild{};
    };
    /**
     * Loads a glTF 2.0 mesh from either a binary `.glb` file or a JSON `.gltf`
     * file. External buffers and images referenced by `.gltf` are resolved
     * relative to `path`.
     */
    [[nodiscard]] std::shared_ptr<const Mesh> load_gltf_mesh(const std::filesystem::path &path);

    /** Imports geometry and image references without decoding textures. */
    [[nodiscard]] std::shared_ptr<const Mesh> load_gltf_mesh_for_cooking(
        const std::filesystem::path &path, GltfImportTimings *timings = nullptr);

    /** Decodes one embedded image at a time; pixel spans expire after each callback. */
    using EmbeddedImageVisitor = std::function<bool(std::uint32_t, std::span<const std::uint8_t>,
                                                    std::uint32_t, std::uint32_t)>;
    [[nodiscard]] bool visit_gltf_embedded_images_for_cooking(
        const std::filesystem::path &path, const EmbeddedImageVisitor &visitor,
        std::chrono::nanoseconds *decodeTime = nullptr);

    /** Hashes glTF/GLB bytes and every external buffer and image used by import. */
    [[nodiscard]] std::optional<std::uint64_t> gltf_source_hash(const std::filesystem::path &path);
} // namespace Engine::Assets
