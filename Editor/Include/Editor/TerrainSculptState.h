#pragma once

#include "Engine/ECS/Components/TerrainComponent.h"
#include "Engine/ECS/Entity.h"

#include <memory>
#include <vector>

struct TerrainSculptState final {
    bool enabled{};
    Engine::TerrainSculptMode mode{Engine::TerrainSculptMode::Raise};
    Engine::TerrainBrushFalloff falloff{Engine::TerrainBrushFalloff::Smooth};
    float radius{2.0F};
    float strength{3.0F};
    float spacing{0.2F};
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
