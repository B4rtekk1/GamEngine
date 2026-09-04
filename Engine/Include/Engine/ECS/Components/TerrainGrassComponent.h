#pragma once

#include "Engine/Math/Math.h"
#include "Engine/Math/AABB.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Renderer/Materials/PBRMaterial.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Engine {
    /**
     * GPU-facing, deterministic description of the vegetation owned by one
     * terrain chunk.  It deliberately contains no per-blade transform.
     */
    struct GrassGenerationData final {
        Vec2 chunkOrigin{};
        Vec2 chunkSize{16.0F, 16.0F};
        std::uint32_t seed{};
        float density{1.0F};
        std::uint32_t biomeId{};
        std::uint32_t grassType{};
    };

    /**
     * A spatial render unit.  CPU submits these records; the GPU is expected
     * to generate, cull and compact the candidate instances they describe.
     * instanceOffset/count are retained while loading legacy painted scenes
     * and identify the compact exception range, not scene objects.
     */
    struct GrassChunk final {
        AABB bounds{};
        std::uint32_t instanceOffset{};
        std::uint32_t instanceCount{};
        std::uint32_t grassType{};
        float density{1.0F};
        float maxDistance{100.0F};
        GrassGenerationData generation{};
    };

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
        static constexpr float DefaultChunkSize = 16.0F;

        std::shared_ptr<const Mesh> mesh;
        PBRMaterial material{};
        // Legacy/editor exceptions only (paint, erase and interaction).  The
        // runtime renderer consumes chunks, rather than treating these as
        // individual scene objects.
        std::vector<TerrainGrassInstance> instances;
        std::vector<GrassChunk> chunks;
        float chunkSize{DefaultChunkSize};
        float maxDistance{100.0F};
        std::uint32_t grassType{};
        std::uint32_t biomeId{};
        std::uint32_t seed{};
        bool castShadow{true};

        // Runtime-only acceleration structure used for sphere/grass overlap.
        mutable std::unordered_map<std::int64_t, std::vector<std::size_t>> spatialCells;
        mutable std::size_t spatialInstanceCount{};
        mutable std::vector<std::size_t> dirtyInstances;
        // A generation stamp makes repeated writes to one instance O(1),
        // avoiding a sort/unique pass for every physics update.
        mutable std::vector<std::uint32_t> dirtyInstanceStamps;
        mutable std::uint32_t dirtyInstanceGeneration{1};
        mutable bool allInstancesDirty{true};
        // Only blades touched recently are simulated back towards their rest
        // pose.  This avoids an O(all grass) recovery pass every physics tick.
        std::vector<std::size_t> recoveringInstances;
        std::vector<std::uint8_t> recoveringInstanceMarks;
        static constexpr float SpatialCellSize = 2.0F;

        TerrainGrassComponent() = default;
        TerrainGrassComponent(const TerrainGrassComponent& other)
            : mesh(other.mesh), material(other.material), instances(other.instances),
              chunks(other.chunks), chunkSize(other.chunkSize), maxDistance(other.maxDistance),
              grassType(other.grassType), biomeId(other.biomeId), seed(other.seed),
              castShadow(other.castShadow) {}
        TerrainGrassComponent& operator=(const TerrainGrassComponent& other) {
            if (this == &other) return *this;
            mesh = other.mesh;
            material = other.material;
            instances = other.instances;
            chunks = other.chunks;
            chunkSize = other.chunkSize;
            maxDistance = other.maxDistance;
            grassType = other.grassType;
            biomeId = other.biomeId;
            seed = other.seed;
            castShadow = other.castShadow;
            spatialCells.clear();
            spatialInstanceCount = 0;
            dirtyInstances.clear();
            dirtyInstanceStamps.clear();
            dirtyInstanceGeneration = 1;
            allInstancesDirty = true;
            recoveringInstances.clear();
            recoveringInstanceMarks.clear();
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

        void markInstanceDirty(const std::size_t index) const {
            if (index >= instances.size()) return;
            if (dirtyInstanceStamps.size() != instances.size()) {
                dirtyInstanceStamps.assign(instances.size(), 0);
            }
            if (dirtyInstanceStamps[index] == dirtyInstanceGeneration) return;
            dirtyInstanceStamps[index] = dirtyInstanceGeneration;
            dirtyInstances.push_back(index);
        }

        void clearDirtyInstances() const {
            dirtyInstances.clear();
            ++dirtyInstanceGeneration;
            if (dirtyInstanceGeneration == 0) {
                std::fill(dirtyInstanceStamps.begin(), dirtyInstanceStamps.end(), 0);
                dirtyInstanceGeneration = 1;
            }
        }

        void markRecovering(const std::size_t index) {
            if (index >= instances.size()) return;
            if (recoveringInstanceMarks.size() != instances.size())
                recoveringInstanceMarks.assign(instances.size(), 0);
            if (recoveringInstanceMarks[index] != 0) return;
            recoveringInstanceMarks[index] = 1;
            recoveringInstances.push_back(index);
        }

        [[nodiscard]] bool hasPrefab() const noexcept {
            return mesh != nullptr && !mesh->empty();
        }

        /**
         * Builds stable 16 m (by default) chunks and makes legacy exception
         * instances contiguous per chunk.  This is intentionally performed
         * only after authoring changes; frame rendering must never construct
         * a spatial hash or scan every blade on the CPU.
         */
        void rebuildChunks() {
            const float size = std::max(0.01F, chunkSize);
            if (instances.empty()) {
                chunks.clear();
                return;
            }
            struct IndexedInstance final { std::int32_t x; std::int32_t z; std::size_t index; };
            std::vector<IndexedInstance> ordered;
            ordered.reserve(instances.size());
            for (std::size_t index = 0; index < instances.size(); ++index) {
                const Vec3& position = instances[index].position;
                ordered.push_back({static_cast<std::int32_t>(std::floor(position.x() / size)),
                                   static_cast<std::int32_t>(std::floor(position.z() / size)), index});
            }
            std::sort(ordered.begin(), ordered.end(), [](const IndexedInstance& a, const IndexedInstance& b) {
                return a.x != b.x ? a.x < b.x : (a.z != b.z ? a.z < b.z : a.index < b.index);
            });
            std::vector<TerrainGrassInstance> sorted;
            sorted.reserve(instances.size());
            chunks.clear();
            for (std::size_t begin = 0; begin < ordered.size();) {
                const IndexedInstance key = ordered[begin];
                std::size_t end = begin + 1;
                while (end < ordered.size() && ordered[end].x == key.x && ordered[end].z == key.z) ++end;
                AABB bounds{};
                bool first = true;
                const std::uint32_t offset = static_cast<std::uint32_t>(sorted.size());
                for (std::size_t item = begin; item < end; ++item) {
                    const TerrainGrassInstance& instance = instances[ordered[item].index];
                    sorted.push_back(instance);
                    const Vec3 point = instance.position;
                    if (first) { bounds = {.min = point, .max = point}; first = false; }
                    else {
                        bounds.min = Vec3{std::min(bounds.min.x(), point.x()), std::min(bounds.min.y(), point.y()), std::min(bounds.min.z(), point.z())};
                        bounds.max = Vec3{std::max(bounds.max.x(), point.x()), std::max(bounds.max.y(), point.y()), std::max(bounds.max.z(), point.z())};
                    }
                }
                GrassGenerationData generation{
                    .chunkOrigin = {static_cast<float>(key.x) * size, static_cast<float>(key.z) * size},
                    .chunkSize = {size, size},
                    .seed = seed ^ static_cast<std::uint32_t>(spatialKey(key.x, key.z)),
                    .density = 1.0F,
                    .biomeId = biomeId,
                    .grassType = grassType};
                chunks.push_back({.bounds = bounds, .instanceOffset = offset,
                                  .instanceCount = static_cast<std::uint32_t>(end - begin),
                                  .grassType = grassType, .density = 1.0F,
                                  .maxDistance = maxDistance, .generation = generation});
                begin = end;
            }
            instances = std::move(sorted);
            spatialCells.clear();
            spatialInstanceCount = 0;
            dirtyInstances.clear();
            dirtyInstanceStamps.clear();
            recoveringInstances.clear();
            recoveringInstanceMarks.clear();
            allInstancesDirty = true;
        }
    };
} // namespace Engine
