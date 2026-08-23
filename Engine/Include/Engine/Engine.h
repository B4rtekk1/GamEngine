#pragma once

// Public convenience header. Applications can start with this single include
// and do not need to know the internal renderer/ECS directory layout.
#include "Engine/Application.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/RenderConfig.h"
#include "Engine/Scene/Scene.h"
#include "Engine/ECS/GameObject.h"
#include "Engine/ECS/Components/TransformComponent.h"
#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/Scene/Components/LightComponent.h"
#include "Engine/Scene/Components/IdentityComponents.h"
#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/Scripting/Script.h"
#include "Engine/Scripting/ScriptRegistry.h"
