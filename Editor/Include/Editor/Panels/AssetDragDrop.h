#pragma once

#include "Engine/Assets/Content.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/ScenePresets.h"
#include "imgui.h"

#include <filesystem>
#include <optional>
#include <string>

namespace Editor::AssetDragDrop {

inline constexpr const char* modelPayload = "EDITOR_MODEL_ASSET";

inline void setModelPayload(const std::filesystem::path& relativePath) {
    const std::string value = relativePath.generic_string();
    ImGui::SetDragDropPayload(modelPayload, value.c_str(), value.size() + 1);
}

inline std::filesystem::path modelPath(const ImGuiPayload& payload) {
    if (payload.Data == nullptr || payload.DataSize <= 1)
        return {};
    return std::filesystem::path{static_cast<const char*>(payload.Data)};
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

} // namespace Editor::AssetDragDrop
