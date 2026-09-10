#pragma once

#include "Engine/Math/AABB.h"
#include "Engine/Renderer/Culling/CullingTypes.h"
#include "Engine/Renderer/Materials/MaterialBuffer.h"
#include "Engine/Renderer/Materials/Material.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Renderer/GPUSceneDatabase.h"
#include "Engine/Renderer/Vulkan/renderer_types.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>
#include <unordered_map>

namespace Engine {
    /** ECS-derived CPU data shared by the scene upload and culling stages. */
    class SceneGpuResources final {
    public:
        GPUSceneDatabase database;

        struct RenderableRecord {
            Entity entity{NullEntity};
            AABB localBounds{};
            std::size_t batchIndex{0};
            std::uint32_t firstVertex{0};
            std::uint32_t vertexCount{0};
            // Revision of the ECS world transform last uploaded to the GPU.
            // Do not retain a second full Transform here: ECS owns it.
            std::uint64_t lastWorldRevision{std::numeric_limits<std::uint64_t>::max()};
            std::uint8_t transformDirtyFrames{0};
            std::uint8_t materialDirtyFrames{0};
            std::uint8_t cullingDirtyFrames{0};
            // Offset into the compact material table.
            std::uint32_t materialTableOffset{};
            // Stable links into the retained GPU scene. Draw-list compaction
            // never changes these IDs.
            RenderProxyHandle renderProxy{};
        };

        struct InstanceBatch {
            const Mesh *mesh{nullptr};
            std::uint32_t firstIndex{0};
            std::uint32_t indexCount{0};
            std::uint32_t lod1IndexCount{0};
            std::uint32_t lod2IndexCount{0};
            float lod1Distance{28.0F};
            float lod2Distance{60.0F};
            std::uint32_t firstInstance{0};
            std::uint32_t instanceCount{0};
            /// GPU-culling bin of the built-in or cooked Shader Graph pipeline.
            std::uint32_t shaderSlot{};
            bool castShadow{true};
            /// True for masked/double-sided vegetation that needs its own raster pipeline.
            bool twoSided{false};
            AABB worldBounds{};
        };

        std::vector<RenderableRecord> renderables;
        std::vector<InstanceBatch> instanceBatches;
        std::vector<std::vector<std::size_t> > batchRenderableIndices;
        std::unordered_map<Entity, std::size_t> renderableIndices;
        std::vector<RendererInstanceData> instanceModels;
        std::vector<RendererPreviousTransformData> previousInstanceTransforms;
        // Dedicated source data for the next grass draw path. These records
        // are 16 B and cluster-relative; normal scene objects never enter it.
        std::vector<GPUGrassInstance> grassInstances;
        std::vector<GPUGrassDeformation> grassDeformations;
        // Maps a terrain-local instance index to its packed GPU record.
        std::unordered_map<Entity, std::vector<std::uint32_t>> grassInstanceGpuIndices;
        std::vector<GPUGrassCluster> grassClusters;
        // Parallel ownership table used only during topology extraction to
        // resolve the shared material table for each GPU grass cluster.
        std::vector<Entity> grassClusterEntities;
        std::vector<GPUMaterialData> materials;
        std::uint32_t materialSlots{1};
        std::uint64_t lastTransformRevision = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t lastMeshRendererRevision = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t lastTerrainGrassRevision = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t lastParentRevision = std::numeric_limits<std::uint64_t>::max();
        std::array<std::vector<std::size_t>, 2> dirtyTransforms;
        std::array<std::vector<std::size_t>, 2> dirtyMaterials;
        std::array<std::vector<std::size_t>, 2> dirtyCullingObjects;
        struct PendingDatabaseUploads final : GPUSceneDatabase::DirtyRanges {
            std::vector<std::uint32_t> instanceStamps;
            std::vector<std::uint32_t> meshStamps;
            std::vector<std::uint32_t> materialStamps;
            std::vector<std::uint32_t> removedInstanceStamps;
            std::uint32_t generation{1};

            void clear() noexcept {
                instances.clear();
                meshes.clear();
                materials.clear();
                removedInstances.clear();

                ++generation;
                if (generation != 0) return;

                std::fill(instanceStamps.begin(), instanceStamps.end(), 0U);
                std::fill(meshStamps.begin(), meshStamps.end(), 0U);
                std::fill(materialStamps.begin(), materialStamps.end(), 0U);
                std::fill(removedInstanceStamps.begin(), removedInstanceStamps.end(), 0U);
                generation = 1;
            }
        };
        // Changes waiting to be copied to each frame-in-flight GPU Scene SSBO.
        std::array<PendingDatabaseUploads, 2> pendingDatabaseUploads;
        Vec3 sceneCenter;
        float sceneRadius{1.0F};
        bool hasShadowCasters{false};
    };
} // namespace Engine
