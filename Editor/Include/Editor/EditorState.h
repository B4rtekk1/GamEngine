#pragma once

#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/ECS/Components/TerrainComponent.h"
#include "Engine/ECS/Components/TerrainGrassComponent.h"
#include "Engine/Scene/Components/IdentityComponents.h"
#include "Engine/Scene/SceneEditor.h"
#include "Engine/Scene/ScenePresets.h"
#include "Engine/Scene/SceneSerializer.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace Editor {

class SceneHistory final {
  public:
    /**
     * @brief Limits retained snapshot memory; the oldest undo entries are
     * evicted first.
     *
     * A zero limit disables eviction. The default keeps an editor session
     * responsive when scenes contain large terrain or embedded textures.
     */
    void setMemoryLimit(const std::size_t bytes) noexcept {
        memoryLimit_ = bytes;
    }

    /** @brief Approximate bytes retained by the baseline and history entries. */
    [[nodiscard]] std::size_t memoryUsage() const noexcept {
        std::size_t bytes = baseline_.scene.size() + baseline_.data.size();
        for (const Entry& entry : undo_) {
            bytes += entryBytes(entry);
        }
        for (const Entry& entry : redo_) {
            bytes += entryBytes(entry);
        }
        return bytes;
    }

    void reset(const Engine::ScenePreset& scene) {
        baseline_ = Engine::SceneSerializer::saveSnapshot(scene);
        observedRevision_ = scene.view().mutationRevision();
        undo_.clear();
        redo_.clear();
    }

    [[nodiscard]] bool capture(const Engine::ScenePreset& scene) {
        const std::uint64_t revision = scene.view().mutationRevision();
        if (revision == observedRevision_)
            return false;
        Engine::SceneSerializer::Snapshot current = Engine::SceneSerializer::saveSnapshot(scene);
        observedRevision_ = revision;
        // The hash is computed while serializing; comparing it avoids walking
        // the full text and binary payloads of a potentially huge scene.
        if (current.hash == baseline_.hash)
            return false;
        undo_.emplace_back(std::move(baseline_));
        baseline_ = std::move(current);
        redo_.clear();
        trimToMemoryLimit();
        return true;
    }

    [[nodiscard]] bool captureTerrainStroke(const Engine::ScenePreset& scene, const Engine::Entity entity,
                                            const std::vector<float>& before, const Engine::TerrainRegion& region) {
        const auto view = scene.view();
        if (!region.valid || !view.valid(entity) || !view.has<Engine::TerrainComponent>(entity) ||
            !view.has<Engine::UUIDComponent>(entity))
            return false;
        const auto& terrain = view.read<Engine::TerrainComponent>(entity);
        if (before.size() != terrain.heights.size())
            return false;
        TerrainEdit edit;
        edit.entity = view.read<Engine::UUIDComponent>(entity).value;
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
        if (edit.before == edit.after)
            return false;
        undo_.emplace_back(std::move(edit));
        baseline_ = Engine::SceneSerializer::saveSnapshot(scene);
        observedRevision_ = view.mutationRevision();
        redo_.clear();
        trimToMemoryLimit();
        return true;
    }

    [[nodiscard]] bool canUndo() const noexcept {
        return !undo_.empty();
    }
    [[nodiscard]] bool canRedo() const noexcept {
        return !redo_.empty();
    }

    bool undo(Engine::ScenePreset& scene) {
        return restore(scene, undo_, redo_, false);
    }
    bool redo(Engine::ScenePreset& scene) {
        return restore(scene, redo_, undo_, true);
    }

  private:
    struct TerrainEdit final {
        Engine::UUID entity;
        std::uint32_t resolution{};
        Engine::TerrainRegion region{};
        std::vector<float> before;
        std::vector<float> after;
    };
    using Entry = std::variant<Engine::SceneSerializer::Snapshot, TerrainEdit>;

    /** Default cap for in-memory undo state (512 MiB). */
    static constexpr std::size_t DefaultMemoryLimit = 512ULL * 1024ULL * 1024ULL;

    [[nodiscard]] static std::size_t entryBytes(const Entry& entry) noexcept {
        if (const auto* snapshot = std::get_if<Engine::SceneSerializer::Snapshot>(&entry)) {
            return snapshot->scene.size() + snapshot->data.size();
        }
        const auto& edit = std::get<TerrainEdit>(entry);
        return sizeof(TerrainEdit) + (edit.before.size() + edit.after.size()) * sizeof(float);
    }

    void trimToMemoryLimit() {
        if (memoryLimit_ == 0)
            return;
        std::size_t bytes = memoryUsage();
        while (!undo_.empty() && bytes > memoryLimit_) {
            // Dropping the oldest state keeps the most recent undo steps
            // available; a snapshot can be hundreds of megabytes when the
            // scene embeds terrain layers or textures.
            bytes -= entryBytes(undo_.front());
            undo_.erase(undo_.begin());
        }
    }

    bool restore(Engine::ScenePreset& scene, std::deque<Entry>& from, std::deque<Entry>& to, const bool forward) {
        if (from.empty())
            return false;
        Entry entry = std::move(from.back());
        from.pop_back();
        if (auto* snapshot = std::get_if<Engine::SceneSerializer::Snapshot>(&entry)) {
            to.emplace_back(std::move(baseline_));
            baseline_ = std::move(*snapshot);
            Engine::SceneSerializer::loadSnapshot(scene, baseline_);
        } else {
            const TerrainEdit& edit = std::get<TerrainEdit>(entry);
            Engine::Entity found = Engine::NullEntity;
            scene.editor().view<>([&](const Engine::Entity candidate) {
                if (found == Engine::NullEntity && scene.editor().has<Engine::UUIDComponent>(candidate) &&
                    scene.editor().read<Engine::UUIDComponent>(candidate).value == edit.entity)
                    found = candidate;
            });
            if (found == Engine::NullEntity || !scene.editor().has<Engine::TerrainComponent>(found)) {
                from.push_back(std::move(entry));
                return false;
            }
            const std::vector<float>& values = forward ? edit.after : edit.before;
            scene.editor().patch<Engine::TerrainComponent>(found, [&](auto& terrain) {
                if (terrain.resolution != edit.resolution)
                    return;
                std::size_t source = 0;
                for (std::uint32_t z = edit.region.minimumZ; z <= edit.region.maximumZ; ++z) {
                    for (std::uint32_t x = edit.region.minimumX; x <= edit.region.maximumX; ++x) {
                        terrain.heights[static_cast<std::size_t>(z) * terrain.resolution + x] = values[source++];
                    }
                }
            });
            const auto& terrain = scene.editor().read<Engine::TerrainComponent>(found);
            auto mesh = std::make_shared<Engine::Mesh>(terrain.createMesh());
            if (scene.editor().has<Engine::MeshRendererComponent>(found)) {
                scene.editor().patch<Engine::MeshRendererComponent>(found, [&](auto& renderer) {
                    terrain.applyMaterialLayers(*mesh, renderer.material.pbr);
                    renderer.mesh = mesh;
                });
            }
            if (scene.editor().has<Engine::ColliderComponent>(found)) {
                scene.editor().patch<Engine::ColliderComponent>(found, [&](auto& collider) {
                    if (auto* shape = std::get_if<Engine::MeshCollider>(&collider.shape))
                        shape->mesh = mesh;
                });
            }
            if (scene.editor().has<Engine::TerrainGrassComponent>(found)) {
                scene.editor().patch<Engine::TerrainGrassComponent>(found, [&](auto& grass) {
                    for (auto& instance : grass.instances)
                        instance.position.setY(terrain.sampleHeight(instance.position.x(), instance.position.z()));
                    grass.allInstancesDirty = true;
                });
            }
            to.push_back(std::move(entry));
            baseline_ = Engine::SceneSerializer::saveSnapshot(scene);
        }
        observedRevision_ = scene.editor().mutationRevision();
        return true;
    }

    Engine::SceneSerializer::Snapshot baseline_;
    // Deques make evicting the oldest entry O(1); a vector shifted every
    // retained variant on every trim.
    std::deque<Entry> undo_;
    std::deque<Entry> redo_;
    std::uint64_t observedRevision_{};
    std::size_t memoryLimit_{DefaultMemoryLimit};
};

class EntityClipboard final {
  public:
    void copy(const Engine::ScenePreset& scene, const Engine::Entity entity) {
        const auto view = scene.view();
        if (!view.valid(entity) || !view.has<Engine::UUIDComponent>(entity))
            return;
        source_ = view.read<Engine::UUIDComponent>(entity).value;
    }

    [[nodiscard]] bool canPaste(const Engine::ScenePreset& scene) const {
        return findSource(scene) != Engine::NullEntity;
    }

    [[nodiscard]] Engine::Entity paste(Engine::ScenePreset& scene) const {
        const Engine::Entity source = findSource(scene);
        return source == Engine::NullEntity ? Engine::NullEntity : scene.editor().duplicate(source);
    }

  private:
    [[nodiscard]] Engine::Entity findSource(const Engine::ScenePreset& scene) const {
        if (!source_)
            return Engine::NullEntity;
        Engine::Entity found = Engine::NullEntity;
        const auto view = scene.view();
        view.view<>([&](const Engine::Entity entity) {
            if (view.has<Engine::UUIDComponent>(entity) && view.read<Engine::UUIDComponent>(entity).value == *source_)
                found = entity;
        });
        return found;
    }

    std::optional<Engine::UUID> source_;
};

} // namespace Editor
