#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <cstdint>

namespace Engine {

class Registry;

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
    // Version 4 adds persistent object UUIDs, names and parent links.
    // Version 3 adds ScriptComponent references and their enabled state.
    // Version 2 stores complete CPU mesh data: tangent frames, materials and
    // embedded images, plus every MeshRenderer material setting.
    static constexpr unsigned FormatVersion = 4;

    /** @brief Writes a scene to a text file. */
    static void save(const Registry& registry, const std::filesystem::path& path);
    static void save(const Registry& registry, const std::filesystem::path& path,
                     std::uint32_t msaaSamples);

    /** @brief Writes a scene to an existing stream. */
    static void save(const Registry& registry, std::ostream& output);
    static void save(const Registry& registry, std::ostream& output,
                     std::uint32_t msaaSamples);

    /**
     * @brief Replaces registry with the scene read from a text file.
     *
     * The registry is changed only after the whole scene has been validated.
     */
    static void load(Registry& registry, const std::filesystem::path& path);
    static void load(Registry& registry, const std::filesystem::path& path,
                     std::optional<std::uint32_t>& msaaSamples);

    /** @brief Replaces registry with the scene read from a stream. */
    static void load(Registry& registry, std::istream& input);
    static void load(Registry& registry, std::istream& input,
                     std::optional<std::uint32_t>& msaaSamples);
};

} // namespace Engine
