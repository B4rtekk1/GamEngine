#pragma once

#include "Engine/Assets/Content.h"
#include "Engine/Core/Diagnostics.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/ScenePresets.h"
#include "imgui.h"

#include "Engine/Scene/Components/IdentityComponents.h"
#include "Engine/Scene/SceneEditor.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <vector>

namespace Editor::AssetDragDrop {
inline constexpr const char* modelPayload = "EDITOR_MODEL_ASSET";
inline constexpr const char* texturePayload = "EDITOR_TEXTURE_ASSET";
inline constexpr const char* shaderGraphPayload = "EDITOR_SHADER_GRAPH_ASSET";
inline constexpr const char* environmentPayload = "EDITOR_ENVIRONMENT_ASSET";

inline void setModelPayload(const std::filesystem::path& relativePath) {
    const std::string value = relativePath.generic_string();
    ImGui::SetDragDropPayload(modelPayload, value.c_str(), value.size() + 1);
}

inline void setTexturePayload(const std::filesystem::path& relativePath) {
    const std::string value = relativePath.generic_string();
    ImGui::SetDragDropPayload(texturePayload, value.c_str(), value.size() + 1);
}

inline void setShaderGraphPayload(const std::filesystem::path& relativePath) {
    const std::string value = relativePath.generic_string();
    ImGui::SetDragDropPayload(shaderGraphPayload, value.c_str(), value.size() + 1);
}

inline void setEnvironmentPayload(const std::filesystem::path& relativePath) {
    const std::string value = relativePath.generic_string();
    ImGui::SetDragDropPayload(environmentPayload, value.c_str(), value.size() + 1);
}

inline std::filesystem::path modelPath(const ImGuiPayload& payload) {
    if (payload.Data == nullptr || payload.DataSize <= 1)
        return {};
    return std::filesystem::path{static_cast<const char*>(payload.Data)};
}

inline std::filesystem::path texturePath(const ImGuiPayload& payload) {
    return modelPath(payload);
}
inline std::filesystem::path shaderGraphPath(const ImGuiPayload& payload) {
    return modelPath(payload);
}
inline std::filesystem::path environmentPath(const ImGuiPayload& payload) {
    return modelPath(payload);
}

inline Engine::Entity instantiateModel(Engine::ScenePreset& scene, Engine::Assets::Content& content,
                                       const std::filesystem::path& relativePath,
                                       const std::optional<Engine::Vec3>& position = std::nullopt) {
    if (relativePath.empty())
        return Engine::NullEntity;
    const auto prefab = Engine::Prefab::model(content, relativePath);
    const auto actor = scene.createPrefab(relativePath.stem().string(), prefab);
    if (position)
        actor.setPosition(*position);
    return scene.findEntity(actor.id());
}

/**
 * A model added to the scene while its mesh loads on a worker thread. A large
 * cooked model takes seconds to read and decode; on the UI thread that froze
 * the editor for the whole time. AssetManager is thread-safe, and the entity
 * is only created back on the UI thread by finishModelLoads().
 */
struct PendingModelLoad final {
    std::filesystem::path path;
    std::optional<Engine::Vec3> position;
    std::optional<Engine::UUID> parentUuid;
    std::future<Engine::Prefab> prefab;
};

inline std::vector<PendingModelLoad>& pendingModelLoads() {
    static std::vector<PendingModelLoad> loads;
    return loads;
}

/** Starts loading @p relativePath; the entity appears when finishModelLoads() sees it ready. */
inline void requestModel(Engine::Assets::Content& content, const std::filesystem::path& relativePath,
                         const std::optional<Engine::Vec3>& position = std::nullopt,
                         const std::optional<Engine::UUID>& parentUuid = std::nullopt) {
    if (relativePath.empty())
        return;
    Engine::Diagnostics::instance().report(Engine::DiagnosticSeverity::Info,
                                           "Loading model " + relativePath.generic_string() + "...",
                                           {.subsystem = "Editor"});
    pendingModelLoads().push_back(
        {relativePath, position, parentUuid,
         std::async(std::launch::async, [&content, relativePath] { return Engine::Prefab::model(content, relativePath); })});
}

[[nodiscard]] inline bool modelLoadsPending() noexcept {
    return !pendingModelLoads().empty();
}

struct FinishedModelLoad final {
    std::filesystem::path path;
    Engine::Entity entity = Engine::NullEntity;
    std::string error;
};

/** Creates the entities of loads that completed; never waits for one that has not. */
inline std::vector<FinishedModelLoad> finishModelLoads(Engine::ScenePreset& scene) {
    std::vector<FinishedModelLoad> finished;
    auto& loads = pendingModelLoads();
    for (auto load = loads.begin(); load != loads.end();) {
        if (load->prefab.wait_for(std::chrono::seconds::zero()) != std::future_status::ready) {
            ++load;
            continue;
        }
        FinishedModelLoad result{.path = load->path};
        try {
            const auto actor = scene.createPrefab(load->path.stem().string(), load->prefab.get());
            if (load->position)
                actor.setPosition(*load->position);
            result.entity = scene.findEntity(actor.id());
            if (load->parentUuid && result.entity != Engine::NullEntity) {
                const Engine::ParentComponent link{.parentUuid = *load->parentUuid};
                if (scene.editor().has<Engine::ParentComponent>(result.entity))
                    scene.editor().patch<Engine::ParentComponent>(result.entity,
                                                                  [&](auto& component) { component = link; });
                else
                    scene.editor().add<Engine::ParentComponent>(result.entity, link);
            }
        } catch (const std::exception& exception) {
            result.error = exception.what();
        }
        finished.push_back(std::move(result));
        load = loads.erase(load);
    }
    return finished;
}
} // namespace Editor::AssetDragDrop
