#include "Engine/Renderer/GPUSceneDatabase.h"

namespace Engine {
    template <typename Id>
    void GPUSceneDatabase::markDirty(std::vector<Id>& list, std::vector<std::uint32_t>& stamps,
                                     const std::uint32_t generation, const Id id) {
        if (id >= stamps.size()) stamps.resize(static_cast<std::size_t>(id) + 1U);
        if (stamps[id] == generation) return;
        stamps[id] = generation;
        list.push_back(id);
    }

    void GPUSceneDatabase::markRemovedInstanceDirty(const GPUSceneInstanceId id) {
        if (id >= m_removedInstanceStamps.size()) {
            const auto size = static_cast<std::size_t>(id) + 1U;
            m_removedInstanceStamps.resize(size);
            m_removedInstancePositions.resize(size);
        }
        if (m_removedInstanceStamps[id] == m_dirtyGeneration) return;
        m_removedInstanceStamps[id] = m_dirtyGeneration;
        m_removedInstancePositions[id] = static_cast<std::uint32_t>(m_dirty.removedInstances.size());
        m_dirty.removedInstances.push_back(id);
    }

    void GPUSceneDatabase::unmarkRemovedInstanceDirty(const GPUSceneInstanceId id) noexcept {
        if (id >= m_removedInstanceStamps.size() ||
            m_removedInstanceStamps[id] != m_dirtyGeneration) return;

        const std::uint32_t position = m_removedInstancePositions[id];
        const GPUSceneInstanceId lastId = m_dirty.removedInstances.back();
        m_dirty.removedInstances[position] = lastId;
        m_removedInstancePositions[lastId] = position;
        m_dirty.removedInstances.pop_back();
        m_removedInstanceStamps[id] = 0;
    }

    void GPUSceneDatabase::advanceDirtyGeneration() noexcept {
        ++m_dirtyGeneration;
        if (m_dirtyGeneration != 0) return;

        m_dirtyGeneration = 1;
        m_dirtyInstanceStamps.assign(m_dirtyInstanceStamps.size(), 0);
        m_dirtyMeshStamps.assign(m_dirtyMeshStamps.size(), 0);
        m_dirtyMaterialStamps.assign(m_dirtyMaterialStamps.size(), 0);
        m_removedInstanceStamps.assign(m_removedInstanceStamps.size(), 0);
    }

    GPUSceneInstanceId GPUSceneDatabase::upsertInstance(const std::uint64_t sourceKey, const GPUInstance& instance) {
        if (const auto found = m_instanceIds.find(sourceKey); found != m_instanceIds.end()) {
            m_instances[found->second] = instance;
            m_instances[found->second].alive = true;
            markDirty(m_dirty.instances, m_dirtyInstanceStamps, m_dirtyGeneration, found->second);
            return found->second;
        }
        GPUSceneInstanceId id;
        if (!m_freeInstances.empty()) {
            id = m_freeInstances.back();
            m_freeInstances.pop_back();
            m_instances[id] = instance;
            // The slot is alive again before its pending deletion reaches the
            // GPU. A later upload must write the new record, not clear it.
            unmarkRemovedInstanceDirty(id);
        } else {
            id = static_cast<GPUSceneInstanceId>(m_instances.size());
            m_instances.push_back(instance);
        }
        m_instances[id].alive = true;
        m_instanceIds.emplace(sourceKey, id);
        markDirty(m_dirty.instances, m_dirtyInstanceStamps, m_dirtyGeneration, id);
        return id;
    }

    void GPUSceneDatabase::updateInstanceTransform(const GPUSceneInstanceId instanceId,
                                                   const std::array<float, 16>& worldMatrix,
                                                   const AABB& localBounds) {
        if (instanceId >= m_instances.size() || !m_instances[instanceId].alive) return;
        GPUInstance& instance = m_instances[instanceId];
        instance.worldMatrix = worldMatrix;
        instance.localBounds = localBounds;
        markDirty(m_dirty.instances, m_dirtyInstanceStamps, m_dirtyGeneration, instanceId);
    }

    void GPUSceneDatabase::updateInstanceFlags(const GPUSceneInstanceId instanceId,
                                                const std::uint32_t flags) {
        if (instanceId >= m_instances.size() || !m_instances[instanceId].alive) return;
        m_instances[instanceId].flags = flags;
        markDirty(m_dirty.instances, m_dirtyInstanceStamps, m_dirtyGeneration, instanceId);
    }

    void GPUSceneDatabase::removeInstance(const std::uint64_t sourceKey,
                                          const std::uint64_t retireValue) {
        const auto found = m_instanceIds.find(sourceKey);
        if (found == m_instanceIds.end()) return;
        const GPUSceneInstanceId id = found->second;
        m_instances[id].alive = false;
        m_instanceIds.erase(found);
        // Do not put this ID in m_freeInstances yet.  Old command buffers may
        // still address the same SSBO slot; overwriting it for a new entity
        // would make those commands render the wrong proxy.
        m_deferredInstanceFrees.push_back({id, retireValue});
        markRemovedInstanceDirty(id);
    }

    void GPUSceneDatabase::reclaimDeferredInstances(const std::uint64_t completedValue) {
        auto write = m_deferredInstanceFrees.begin();
        for (auto read = m_deferredInstanceFrees.begin(); read != m_deferredInstanceFrees.end(); ++read) {
            if (read->retireValue <= completedValue) {
                m_freeInstances.push_back(read->id);
            } else {
                *write++ = *read;
            }
        }
        m_deferredInstanceFrees.erase(write, m_deferredInstanceFrees.end());
    }

    GPUSceneMeshId GPUSceneDatabase::upsertMesh(const std::uint64_t sourceKey, const GPUMesh& mesh) {
        if (const auto found = m_meshIds.find(sourceKey); found != m_meshIds.end()) {
            m_meshes[found->second] = mesh;
            markDirty(m_dirty.meshes, m_dirtyMeshStamps, m_dirtyGeneration, found->second);
            return found->second;
        }
        const auto id = static_cast<GPUSceneMeshId>(m_meshes.size());
        m_meshes.push_back(mesh);
        m_meshIds.emplace(sourceKey, id);
        markDirty(m_dirty.meshes, m_dirtyMeshStamps, m_dirtyGeneration, id);
        return id;
    }

    GPUSceneMaterialId GPUSceneDatabase::upsertMaterial(const std::uint64_t sourceKey, const GPUMaterial& material) {
        if (const auto found = m_materialIds.find(sourceKey); found != m_materialIds.end()) {
            m_materials[found->second] = material;
            markDirty(m_dirty.materials, m_dirtyMaterialStamps, m_dirtyGeneration, found->second);
            return found->second;
        }
        const auto id = static_cast<GPUSceneMaterialId>(m_materials.size());
        m_materials.push_back(material);
        m_materialIds.emplace(sourceKey, id);
        markDirty(m_dirty.materials, m_dirtyMaterialStamps, m_dirtyGeneration, id);
        return id;
    }

    void GPUSceneDatabase::updateMesh(const GPUSceneMeshId meshId, const GPUMesh& mesh) {
        if (meshId >= m_meshes.size()) return;
        m_meshes[meshId] = mesh;
        markDirty(m_dirty.meshes, m_dirtyMeshStamps, m_dirtyGeneration, meshId);
    }

    void GPUSceneDatabase::updateMaterial(const GPUSceneMaterialId materialId,
                                          const GPUMaterial& material) {
        if (materialId >= m_materials.size()) return;
        m_materials[materialId] = material;
        markDirty(m_dirty.materials, m_dirtyMaterialStamps, m_dirtyGeneration, materialId);
    }

    GPUSceneInstanceId GPUSceneDatabase::instanceId(const std::uint64_t sourceKey) const noexcept {
        const auto found = m_instanceIds.find(sourceKey);
        return found == m_instanceIds.end() ? InvalidGPUSceneInstanceId : found->second;
    }

    void GPUSceneDatabase::clearDirty() noexcept {
        m_dirty.instances.clear();
        m_dirty.meshes.clear();
        m_dirty.materials.clear();
        m_dirty.removedInstances.clear();
        advanceDirtyGeneration();
    }

    void GPUSceneDatabase::clear() noexcept {
        m_instances.clear(); m_meshes.clear(); m_materials.clear(); m_freeInstances.clear();
        m_deferredInstanceFrees.clear();
        m_instanceIds.clear(); m_meshIds.clear(); m_materialIds.clear(); clearDirty();
        m_dirtyInstanceStamps.clear(); m_dirtyMeshStamps.clear(); m_dirtyMaterialStamps.clear();
        m_removedInstanceStamps.clear(); m_removedInstancePositions.clear();
    }
} // namespace Engine
