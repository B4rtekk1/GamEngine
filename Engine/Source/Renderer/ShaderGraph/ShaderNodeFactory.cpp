#include "Engine/Renderer/ShaderGraph/ShaderNodeFactory.h"
#include "Engine/Renderer/ShaderGraph/ShaderNodeRegistry.h"

namespace Engine {
    ShaderNode ShaderNodeFactory::create(const ShaderNodeType type, const ShaderNodeId nodeId,
                                         ShaderPinId& nextPinId) {
        return ShaderNodeRegistry::create(type, nodeId, nextPinId);
    }
} // namespace Engine
