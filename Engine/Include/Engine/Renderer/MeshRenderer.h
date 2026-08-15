#pragma once

#include "Engine/ECS/Components/MeshRendererComponent.h"

namespace Engine {

/**
 * @brief Backward-compatible name for MeshRendererComponent.
 *
 * New ECS code may use MeshRendererComponent directly.
 */
using MeshRenderer = MeshRendererComponent;

} // namespace Engine
