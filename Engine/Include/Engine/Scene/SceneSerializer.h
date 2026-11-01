#pragma once

#include "Engine/Renderer/RenderConfig.h"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>

namespace Engine {
class Registry;
class Scene;

/**
 * @brief Saves and loads the serializable portion of an ECS scene.
 *
 * The current format stores identity, hierarchy, Transform, CameraComponent,
 * MeshRenderer, LightComponent and ScriptComponent.
 * Runtime-only renderer state, such as uploaded index offsets and occlusion
 * query slots, is intentionally rebuilt instead of serialized.
 */
class SceneSerializer final {
  public:
    // This serializer intentionally supports only the current on-disk
    // format. Files with any other version are rejected while loading.
    /**
     * Version 23 persists WaterBodyComponent geometry, optical parameters and Gerstner waves. Version 21 adds local
     * ReflectionProbeComponent volumes. Version 19 persists a material's relative Shader Graph source asset; cooked
     * program IDs and SPIR-V paths are rebuilt at load time. Version 15 persists the directional Main Light selection.
     * Version 13 adds PBR emission while keeping the scene graph readable and storing terrain sample buffers in a
     * lossless binary sidecar (<scene>.terrain).  This avoids formatting and parsing every height/colour as decimal
     * text.
     */
    static constexpr std::uint32_t FormatVersion = 33;

    /** @brief Writes a scene to a text file. */
    static void save(const Registry& registry, const std::filesystem::path& path);

    static void save(const Registry& registry, const std::filesystem::path& path, AntialiasingLevel antialiasing);
    /** @deprecated Use the AntialiasingLevel overload. */
    static void save(const Registry& registry, const std::filesystem::path& path, std::uint32_t msaaSamples);

    /** @brief Writes a scene to an existing stream. */
    static void save(const Registry& registry, std::ostream& output);

    static void save(const Registry& registry, std::ostream& output, AntialiasingLevel antialiasing);
    /** @deprecated Use the AntialiasingLevel overload. */
    static void save(const Registry& registry, std::ostream& output, std::uint32_t msaaSamples);

    // High-level scene API. These overloads keep Registry out of application code.
    static void save(const Scene& scene, const std::filesystem::path& path);

    static void save(const Scene& scene, const std::filesystem::path& path, AntialiasingLevel antialiasing);
    /** @deprecated Use the AntialiasingLevel overload. */
    static void save(const Scene& scene, const std::filesystem::path& path, std::uint32_t msaaSamples);

    static void save(const Scene& scene, std::ostream& output);

    static void save(const Scene& scene, std::ostream& output, AntialiasingLevel antialiasing);
    /** @deprecated Use the AntialiasingLevel overload. */
    static void save(const Scene& scene, std::ostream& output, std::uint32_t msaaSamples);

    /**
     * @brief Complete serialized scene held in memory.
     *
     * `scene` stores the regular text format and `data` the lossless binary
     * sidecar (terrain samples and embedded RGBA images). Keeping both
     * together avoids formatting large buffers as decimal text when the
     * editor captures an undo snapshot.
     */
    struct Snapshot final {
        std::string scene;
        std::string data;
        /**
         * Content hash of both payloads, filled in by saveSnapshot().
         *
         * The editor rejects no-op undo captures by comparing this value, so a
         * frame that changed only runtime state does not walk hundreds of
         * megabytes of scene text. operator== still verifies the bytes to keep
         * equality exact.
         */
        std::uint64_t hash{};

        [[nodiscard]] bool operator==(const Snapshot& other) const noexcept;
    };

    /** @brief Serializes a scene and its binary sidecar into memory. */
    static Snapshot saveSnapshot(const Registry& registry);
    static Snapshot saveSnapshot(const Registry& registry, AntialiasingLevel antialiasing);
    static Snapshot saveSnapshot(const Scene& scene);
    static Snapshot saveSnapshot(const Scene& scene, AntialiasingLevel antialiasing);

    /** @brief Replaces registry with an in-memory snapshot. */
    static void loadSnapshot(Registry& registry, const Snapshot& snapshot);
    static void loadSnapshot(Registry& registry, const Snapshot& snapshot,
                             std::optional<AntialiasingLevel>& antialiasing);
    static void loadSnapshot(Scene& scene, const Snapshot& snapshot);
    static void loadSnapshot(Scene& scene, const Snapshot& snapshot, std::optional<AntialiasingLevel>& antialiasing);

    /**
     * @brief Replaces registry with the scene read from a text file.
     *
     * The registry is changed only after the whole scene has been validated.
     */
    static void load(Registry& registry, const std::filesystem::path& path);

    static void load(Registry& registry, const std::filesystem::path& path,
                     std::optional<AntialiasingLevel>& antialiasing);
    /** @deprecated Use the AntialiasingLevel overload. */
    static void load(Registry& registry, const std::filesystem::path& path, std::optional<std::uint32_t>& msaaSamples);

    /** @brief Replaces registry with the scene read from a stream. */
    static void load(Registry& registry, std::istream& input);

    static void load(Registry& registry, std::istream& input, std::optional<AntialiasingLevel>& antialiasing);
    /** @deprecated Use the AntialiasingLevel overload. */
    static void load(Registry& registry, std::istream& input, std::optional<std::uint32_t>& msaaSamples);

    static void load(Scene& scene, const std::filesystem::path& path);

    static void load(Scene& scene, const std::filesystem::path& path, std::optional<AntialiasingLevel>& antialiasing);
    /** @deprecated Use the AntialiasingLevel overload. */
    static void load(Scene& scene, const std::filesystem::path& path, std::optional<std::uint32_t>& msaaSamples);

    static void load(Scene& scene, std::istream& input);

    static void load(Scene& scene, std::istream& input, std::optional<AntialiasingLevel>& antialiasing);
    /** @deprecated Use the AntialiasingLevel overload. */
    static void load(Scene& scene, std::istream& input, std::optional<std::uint32_t>& msaaSamples);

    /**
     * Adopts an already decoded scene without parsing or copying its registry.
     *
     * Both scenes must be quiescent. This is intended for handing a scene
     * decoded by a worker to the owner/render thread.
     */
    static void replace(Scene& destination, Scene& source);
};
} // namespace Engine
