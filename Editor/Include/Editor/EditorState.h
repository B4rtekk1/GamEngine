#pragma once

#include "Engine/Scene/ScenePresets.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/ECS/Components/TerrainComponent.h"
#include "Engine/Scene/Components/IdentityComponents.h"

#include <optional>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <variant>

namespace Editor {

inline std::string serializeScene(const Engine::ScenePreset &scene) {
    std::ostringstream output;
    Engine::SceneSerializer::save(scene, output);
    return output.str();
}

class SceneHistory final {
public:
    void reset(const Engine::ScenePreset &scene) {
        baseline_ = serializeScene(scene);
        observedRevision_ = scene.editor().mutationRevision();
        undo_.clear();
        redo_.clear();
    }

    [[nodiscard]] bool capture(const Engine::ScenePreset &scene) {
        const std::uint64_t revision = scene.editor().mutationRevision();
        if (revision == observedRevision_) return false;
        const std::string current = serializeScene(scene);
        observedRevision_ = revision;
        if (current == baseline_) return false;
        undo_.emplace_back(std::move(baseline_));
        baseline_ = current;
        redo_.clear();
        return true;
    }

    [[nodiscard]] bool captureTerrainStroke(const Engine::ScenePreset& scene,
                                            const Engine::Entity entity,
                                            const std::vector<float>& before,
                                            const Engine::TerrainRegion& region) {
        if (!region.valid || !scene.editor().valid(entity) ||
            !scene.editor().has<Engine::TerrainComponent>(entity) ||
            !scene.editor().has<Engine::UUIDComponent>(entity)) return false;
        const auto& terrain = scene.editor().get<Engine::TerrainComponent>(entity);
        if (before.size() != terrain.heights.size()) return false;
        TerrainEdit edit;
        edit.entity = scene.editor().get<Engine::UUIDComponent>(entity).value;
        edit.resolution = terrain.resolution;
        edit.region = region;
        const std::size_t width = region.maximumX - region.minimumX + 1;
        const std::size_t height = region.maximumZ - region.minimumZ + 1;
        edit.before.reserve(width * height);
        edit.after.reserve(width * height);
        for (std::uint32_t z = region.minimumZ; z <= region.maximumZ; ++z) {
            for (std::uint32_t x = region.minimumX; x <= region.maximumX; ++x) {
                const std::size_t index = static_cast<std::size_t>(z) * terrain.resolution + x;
                edit.before.push_back(before[index]);
                edit.after.push_back(terrain.heights[index]);
            }
        }
        if (edit.before == edit.after) return false;
        undo_.emplace_back(std::move(edit));
        baseline_ = serializeScene(scene);
        observedRevision_ = scene.editor().mutationRevision();
        redo_.clear();
        return true;
    }

    [[nodiscard]] bool canUndo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !redo_.empty(); }

    bool undo(Engine::ScenePreset &scene) { return restore(scene, undo_, redo_, false); }
    bool redo(Engine::ScenePreset &scene) { return restore(scene, redo_, undo_, true); }

private:
    struct TerrainEdit final {
        Engine::UUID entity;
        std::uint32_t resolution{};
        Engine::TerrainRegion region{};
        std::vector<float> before;
        std::vector<float> after;
    };
    using Entry = std::variant<std::string, TerrainEdit>;

    bool restore(Engine::ScenePreset &scene, std::vector<Entry> &from,
                 std::vector<Entry> &to, const bool forward) {
        if (from.empty()) return false;
        Entry entry = std::move(from.back());
        from.pop_back();
        if (auto* snapshot = std::get_if<std::string>(&entry)) {
            to.emplace_back(std::move(baseline_));
            baseline_ = std::move(*snapshot);
            std::istringstream input{baseline_};
            Engine::SceneSerializer::load(scene, input);
        } else {
            const TerrainEdit& edit = std::get<TerrainEdit>(entry);
            Engine::Entity found = Engine::NullEntity;
            scene.editor().view<>([&](const Engine::Entity candidate) {
                if (found == Engine::NullEntity && scene.editor().has<Engine::UUIDComponent>(candidate) &&
                    scene.editor().get<Engine::UUIDComponent>(candidate).value == edit.entity) found = candidate;
            });
            if (found == Engine::NullEntity || !scene.editor().has<Engine::TerrainComponent>(found)) {
                from.push_back(std::move(entry));
                return false;
            }
            const std::vector<float>& values = forward ? edit.after : edit.before;
            scene.editor().modify<Engine::TerrainComponent>(found, [&](auto& terrain) {
                if (terrain.resolution != edit.resolution) return;
                std::size_t source = 0;
                for (std::uint32_t z = edit.region.minimumZ; z <= edit.region.maximumZ; ++z) {
                    for (std::uint32_t x = edit.region.minimumX; x <= edit.region.maximumX; ++x) {
                        terrain.heights[static_cast<std::size_t>(z) * terrain.resolution + x] = values[source++];
                    }
                }
            });
            const auto& terrain = scene.editor().get<Engine::TerrainComponent>(found);
            auto mesh = std::make_shared<Engine::Mesh>(terrain.createMesh());
            if (scene.editor().has<Engine::MeshRendererComponent>(found)) {
                scene.editor().modify<Engine::MeshRendererComponent>(found,
                    [&](auto& renderer) { renderer.mesh = mesh; });
            }
            if (scene.editor().has<Engine::ColliderComponent>(found)) {
                scene.editor().modify<Engine::ColliderComponent>(found, [&](auto& collider) {
                    if (auto* shape = std::get_if<Engine::MeshCollider>(&collider.shape)) shape->mesh = mesh;
                });
            }
            to.push_back(std::move(entry));
            baseline_ = serializeScene(scene);
        }
        observedRevision_ = scene.editor().mutationRevision();
        return true;
    }

    std::string baseline_;
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
    std::uint64_t observedRevision_{};
};

class EntityClipboard final {
public:
    void copy(const Engine::ScenePreset &scene, const Engine::Entity entity) {
        if (!scene.editor().valid(entity) ||
            !scene.editor().has<Engine::UUIDComponent>(entity)) return;
        source_ = scene.editor().get<Engine::UUIDComponent>(entity).value;
    }

    [[nodiscard]] bool canPaste(const Engine::ScenePreset &scene) const {
        return findSource(scene) != Engine::NullEntity;
    }

    [[nodiscard]] Engine::Entity paste(Engine::ScenePreset &scene) const {
        const Engine::Entity source = findSource(scene);
        return source == Engine::NullEntity ? Engine::NullEntity : scene.editor().duplicate(source);
    }

private:
    [[nodiscard]] Engine::Entity findSource(const Engine::ScenePreset &scene) const {
        if (!source_) return Engine::NullEntity;
        Engine::Entity found = Engine::NullEntity;
        scene.editor().view<>([&](const Engine::Entity entity) {
            if (scene.editor().has<Engine::UUIDComponent>(entity) &&
                scene.editor().get<Engine::UUIDComponent>(entity).value == *source_) found = entity;
        });
        return found;
    }

    std::optional<Engine::UUID> source_;
};

}
