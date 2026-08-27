#pragma once

#include "Engine/Core/Transform.h"
#include "Engine/Math/AABB.h"
#include "Engine/Renderer/Culling/CullingTypes.h"
#include "Engine/Renderer/Materials/MaterialBuffer.h"
#include "Engine/Renderer/Geometry/Mesh.h"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>
#include <unordered_map>

namespace Engine {

/** ECS-derived CPU data shared by the scene upload and culling stages. */
class SceneGpuResources final {
public:
    struct RenderableRecord {
        Entity entity{NullEntity};
        AABB localBounds{};
        std::size_t batchIndex{0};
        std::uint32_t firstVertex{0};
        std::uint32_t vertexCount{0};
        Transform cachedTransform{};
        bool hasCachedTransform{false};
        std::uint8_t transformDirtyFrames{0};
        std::uint8_t materialDirtyFrames{0};
        std::uint8_t cullingDirtyFrames{0};
    };

    struct InstanceBatch {
        const Mesh* mesh{nullptr};
        std::uint32_t firstIndex{0};
        std::uint32_t indexCount{0};
        std::uint32_t firstInstance{0};
        std::uint32_t instanceCount{0};
        bool castShadow{true};
        AABB worldBounds{};
    };

    std::vector<RenderableRecord> renderables;
    std::vector<InstanceBatch> instanceBatches;
    std::vector<std::vector<std::size_t>> batchRenderableIndices;
    std::unordered_map<Entity, std::size_t> renderableIndices;
    std::vector<glm::mat4> instanceModels;
    std::vector<glm::mat4> shadowInstanceModels;
    std::vector<GPUMaterialData> materials;
    std::uint32_t materialSlots{1};
    std::uint64_t lastTransformRevision = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t lastMeshRendererRevision = std::numeric_limits<std::uint64_t>::max();
    std::array<std::vector<std::size_t>, 2> dirtyTransforms;
    std::array<std::vector<std::size_t>, 2> dirtyMaterials;
    std::array<std::vector<std::size_t>, 2> dirtyCullingObjects;
    Vec3 sceneCenter{};
    float sceneRadius{1.0F};
    bool hasShadowCasters{false};
};

} // namespace Engine
