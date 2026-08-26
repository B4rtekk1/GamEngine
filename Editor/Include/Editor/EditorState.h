#pragma once

#include "Engine/Scene/ScenePresets.h"
#include "Engine/Scene/SceneSerializer.h"

#include <optional>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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
        undo_.push_back(std::move(baseline_));
        baseline_ = current;
        redo_.clear();
        return true;
    }

    [[nodiscard]] bool canUndo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !redo_.empty(); }

    bool undo(Engine::ScenePreset &scene) { return restore(scene, undo_, redo_); }
    bool redo(Engine::ScenePreset &scene) { return restore(scene, redo_, undo_); }

private:
    bool restore(Engine::ScenePreset &scene, std::vector<std::string> &from,
                 std::vector<std::string> &to) {
        if (from.empty()) return false;
        to.push_back(std::move(baseline_));
        baseline_ = std::move(from.back());
        from.pop_back();
        std::istringstream input{baseline_};
        Engine::SceneSerializer::load(scene, input);
        observedRevision_ = scene.editor().mutationRevision();
        return true;
    }

    std::string baseline_;
    std::vector<std::string> undo_;
    std::vector<std::string> redo_;
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
