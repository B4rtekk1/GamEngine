#pragma once

#include "Engine/ECS/Components/TerrainComponent.h"
#include "Engine/ECS/Entity.h"

#include <memory>
#include <vector>

struct TerrainSculptState final {
    bool enabled{};
    bool grassEnabled{};
    bool grassErase{};
    Engine::TerrainSculptMode mode{Engine::TerrainSculptMode::Raise};
    Engine::TerrainBrushFalloff falloff{Engine::TerrainBrushFalloff::Smooth};
    float radius{2.0F};
    float strength{3.0F};
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
    std::shared_ptr<Engine::Mesh> workingMesh;
    std::vector<float> heightsBeforeStroke;
    Engine::TerrainRegion strokeDirty{};
    bool strokeCompleted{};
    Engine::Entity completedEntity{Engine::NullEntity};
    Engine::TerrainRegion completedDirty{};
};
