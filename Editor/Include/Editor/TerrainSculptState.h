#pragma once

#include "Engine/ECS/Components/TerrainComponent.h"
#include "Engine/ECS/Entity.h"

struct TerrainSculptState final {
    bool enabled{};
    Engine::TerrainSculptMode mode{Engine::TerrainSculptMode::Raise};
    float radius{2.0F};
    float strength{3.0F};
    bool strokeActive{};
    Engine::Entity strokeEntity{Engine::NullEntity};
    float flattenHeight{};
};
