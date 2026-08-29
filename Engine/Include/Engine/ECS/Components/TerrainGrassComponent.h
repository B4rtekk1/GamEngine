#pragma once

#include "Engine/Math/Math.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Renderer/Materials/PBRMaterial.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Engine {
    /** Compact transform used by terrain foliage GPU instances. */
    struct TerrainGrassInstance final {
        Vec3 position{};
        float yaw{};
        float scale{1.0F};
        float bendX{};
        float bendZ{};
        float trampled{};
    };

    /**
     * Foliage painted on a terrain. Instances are kept as compact data rather
     * than ECS entities and are expanded into one renderer instance batch.
     */
    struct TerrainGrassComponent final {
        static constexpr std::size_t MaximumInstances = 1'000'000;

        std::shared_ptr<const Mesh> mesh;
        PBRMaterial material{};
        std::vector<TerrainGrassInstance> instances;
        bool castShadow{true};

        // Runtime-only acceleration structure used for sphere/grass overlap.
        mutable std::unordered_map<std::int64_t, std::vector<std::size_t>> spatialCells;
        mutable std::size_t spatialInstanceCount{};
        mutable std::vector<std::size_t> dirtyInstances;
        mutable bool allInstancesDirty{true};
        static constexpr float SpatialCellSize = 2.0F;

        TerrainGrassComponent() = default;
        TerrainGrassComponent(const TerrainGrassComponent& other)
            : mesh(other.mesh), material(other.material), instances(other.instances),
              castShadow(other.castShadow) {}
        TerrainGrassComponent& operator=(const TerrainGrassComponent& other) {
            if (this == &other) return *this;
            mesh = other.mesh;
            material = other.material;
            instances = other.instances;
            castShadow = other.castShadow;
            spatialCells.clear();
            spatialInstanceCount = 0;
            dirtyInstances.clear();
            allInstancesDirty = true;
            return *this;
        }
        TerrainGrassComponent(TerrainGrassComponent&&) noexcept = default;
        TerrainGrassComponent& operator=(TerrainGrassComponent&&) noexcept = default;

        [[nodiscard]] static std::int64_t spatialKey(const std::int32_t x,
                                                     const std::int32_t z) noexcept {
            const auto packed = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32U) |
                                static_cast<std::uint32_t>(z);
            return static_cast<std::int64_t>(packed);
        }

        void rebuildSpatialIndex() const {
            if (spatialInstanceCount == instances.size()) return;
            spatialCells.clear();
            spatialCells.reserve(instances.size() / 4U + 1U);
            for (std::size_t i = 0; i < instances.size(); ++i) {
                const auto x = static_cast<std::int32_t>(std::floor(instances[i].position.x() / SpatialCellSize));
                const auto z = static_cast<std::int32_t>(std::floor(instances[i].position.z() / SpatialCellSize));
                spatialCells[spatialKey(x, z)].push_back(i);
            }
            spatialInstanceCount = instances.size();
        }

        [[nodiscard]] bool hasPrefab() const noexcept {
            return mesh != nullptr && !mesh->empty();
        }
    };
} // namespace Engine
