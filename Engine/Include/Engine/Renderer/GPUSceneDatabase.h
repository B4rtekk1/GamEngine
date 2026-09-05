#pragma once

#include "Engine/ECS/Entity.h"
#include "Engine/Math/AABB.h"

#include <cstdint>
#include <array>
#include <limits>
#include <unordered_map>
#include <vector>

namespace Engine {
    using GPUSceneInstanceId = std::uint32_t;
    using GPUSceneMeshId = std::uint32_t;
    using GPUSceneMaterialId = std::uint32_t;

    inline constexpr GPUSceneInstanceId InvalidGPUSceneInstanceId =
        std::numeric_limits<GPUSceneInstanceId>::max();

    /**
     * Persistent, renderer-owned scene table.  IDs are indices, never CPU
     * pointers, so shaders can follow instance -> mesh -> material links.
     * The class is deliberately API/Vulkan agnostic: the backend consumes its
     * dirty lists to upload only changed records to storage buffers.
     */
    class GPUSceneDatabase final {
    public:
        struct GPUInstance {
            std::array<float, 16> worldMatrix{};
            AABB localBounds{};
            GPUSceneMeshId meshId{InvalidGPUSceneInstanceId};
            GPUSceneMaterialId materialId{InvalidGPUSceneInstanceId};
            std::uint32_t objectId{0};
            std::uint32_t flags{0};
            bool alive{false};
        };

        struct GPUMesh {
            std::uint32_t firstIndex{0};
            std::uint32_t indexCount{0};
            std::int32_t vertexOffset{0};
            std::uint32_t lod1IndexCount{0};
            std::uint32_t lod2IndexCount{0};
        };

        struct GPUMaterial {
            std::uint32_t materialTableOffset{0};
            std::uint32_t pipelineClass{0};
            std::uint32_t flags{0};
        };

        struct DirtyRanges {
            std::vector<GPUSceneInstanceId> instances;
            std::vector<GPUSceneMeshId> meshes;
            std::vector<GPUSceneMaterialId> materials;
            std::vector<GPUSceneInstanceId> removedInstances;
        };

        /// @p sourceKey identifies one extracted renderable (an entity or one
        /// compact instance owned by an entity, such as terrain grass).
        [[nodiscard]] GPUSceneInstanceId upsertInstance(std::uint64_t sourceKey, const GPUInstance& instance);
        void removeInstance(std::uint64_t sourceKey);
        [[nodiscard]] GPUSceneMeshId upsertMesh(std::uint64_t sourceKey, const GPUMesh& mesh);
        [[nodiscard]] GPUSceneMaterialId upsertMaterial(std::uint64_t sourceKey, const GPUMaterial& material);

        [[nodiscard]] GPUSceneInstanceId instanceId(std::uint64_t sourceKey) const noexcept;
        [[nodiscard]] const std::vector<GPUInstance>& instances() const noexcept { return m_instances; }
        [[nodiscard]] const std::vector<GPUMesh>& meshes() const noexcept { return m_meshes; }
        [[nodiscard]] const std::vector<GPUMaterial>& materials() const noexcept { return m_materials; }
        [[nodiscard]] const DirtyRanges& dirty() const noexcept { return m_dirty; }
        void clearDirty() noexcept;
        void clear() noexcept;

    private:
        template <typename Id>
        static void markDirty(std::vector<Id>& list, std::vector<std::uint32_t>& stamps,
                              std::uint32_t generation, Id id);
        void markRemovedInstanceDirty(GPUSceneInstanceId id);
        void unmarkRemovedInstanceDirty(GPUSceneInstanceId id) noexcept;
        void advanceDirtyGeneration() noexcept;

        std::vector<GPUInstance> m_instances;
        std::vector<GPUMesh> m_meshes;
        std::vector<GPUMaterial> m_materials;
        std::vector<GPUSceneInstanceId> m_freeInstances;
        std::unordered_map<std::uint64_t, GPUSceneInstanceId> m_instanceIds;
        std::unordered_map<std::uint64_t, GPUSceneMeshId> m_meshIds;
        std::unordered_map<std::uint64_t, GPUSceneMaterialId> m_materialIds;
        DirtyRanges m_dirty;
        std::vector<std::uint32_t> m_dirtyInstanceStamps;
        std::vector<std::uint32_t> m_dirtyMeshStamps;
        std::vector<std::uint32_t> m_dirtyMaterialStamps;
        std::vector<std::uint32_t> m_removedInstanceStamps;
        std::vector<std::uint32_t> m_removedInstancePositions;
        std::uint32_t m_dirtyGeneration{1};
    };
} // namespace Engine
