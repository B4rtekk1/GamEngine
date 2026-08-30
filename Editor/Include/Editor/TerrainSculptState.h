#pragma once

#include "Engine/ECS/Components/TerrainComponent.h"
#include "Engine/ECS/Entity.h"

#include <memory>
#include <vector>

struct TerrainStrokeTarget final {
    Engine::Entity entity{Engine::NullEntity};
    std::shared_ptr<Engine::Mesh> workingMesh;
    Engine::TerrainRegion dirty{};
};

struct TerrainSculptState final {
    bool enabled{};
    bool paintEnabled{};
    bool grassEnabled{};
    bool grassErase{};
    Engine::TerrainSculptMode mode{Engine::TerrainSculptMode::Raise};
    Engine::TerrainBrushFalloff falloff{Engine::TerrainBrushFalloff::Smooth};
    float radius{2.0F};
    float strength{3.0F};
    // A soft default preserves already painted layers; 1.0 deliberately replaces them.
    float paintOpacity{0.35F};
    int paintLayer{};
    float spacing{0.2F};
    float grassDensity{2.0F};
    float grassMinimumScale{0.8F};
    float grassMaximumScale{1.2F};
    bool grassRandomYaw{true};
    bool grassStrokeActive{};
    bool grassStrokeChanged{};
    bool grassHasPreviousPoint{};
    Engine::Vec3 grassPreviousPoint{};
    std::uint32_t grassRandomState{0x9e3779b9U};
    std::uint32_t previewLod{};
    bool strokeActive{};
    Engine::Entity strokeEntity{Engine::NullEntity};
    float flattenHeight{};
    bool hasPreviousPoint{};
    Engine::Vec3 previousPoint{};
    bool hasPreviousSculptWorldPoint{};
    Engine::Vec3 previousSculptWorldPoint{};
    std::shared_ptr<Engine::Mesh> workingMesh;
    // Sculpting operates on every terrain intersected by the brush.  Paint
    // and detail tools retain the single-terrain state above.
    std::vector<TerrainStrokeTarget> sculptTargets;
    std::vector<float> heightsBeforeStroke;
    Engine::TerrainRegion strokeDirty{};
    bool strokeCompleted{};
    Engine::Entity completedEntity{Engine::NullEntity};
    Engine::TerrainRegion completedDirty{};
};
