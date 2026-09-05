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

    void GPUSceneDatabase::removeInstance(const std::uint64_t sourceKey) {
        const auto found = m_instanceIds.find(sourceKey);
        if (found == m_instanceIds.end()) return;
        const GPUSceneInstanceId id = found->second;
        m_instances[id].alive = false;
        m_instanceIds.erase(found);
        if (std::ranges::find(m_freeInstances, id) == m_freeInstances.end()) {
            m_freeInstances.push_back(id);
        }
        markRemovedInstanceDirty(id);
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
        m_instanceIds.clear(); m_meshIds.clear(); m_materialIds.clear(); clearDirty();
        m_dirtyInstanceStamps.clear(); m_dirtyMeshStamps.clear(); m_dirtyMaterialStamps.clear();
        m_removedInstanceStamps.clear(); m_removedInstancePositions.clear();
    }
} // namespace Engine
