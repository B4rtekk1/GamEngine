#include "Engine/Renderer/GPUSceneDatabase.h"

#include <algorithm>

namespace Engine {
    template <typename Id>
    void GPUSceneDatabase::appendUnique(std::vector<Id>& list, const Id id) {
        if (std::ranges::find(list, id) == list.end()) list.push_back(id);
    }

    GPUSceneInstanceId GPUSceneDatabase::upsertInstance(const std::uint64_t sourceKey, const GPUInstance& instance) {
        if (const auto found = m_instanceIds.find(sourceKey); found != m_instanceIds.end()) {
            m_instances[found->second] = instance;
            m_instances[found->second].alive = true;
            appendUnique(m_dirty.instances, found->second);
            return found->second;
        }
        GPUSceneInstanceId id;
        if (!m_freeInstances.empty()) {
            id = m_freeInstances.back();
            m_freeInstances.pop_back();
            m_instances[id] = instance;
            // The slot is alive again before its pending deletion reaches the
            // GPU. A later upload must write the new record, not clear it.
            std::erase(m_dirty.removedInstances, id);
        } else {
            id = static_cast<GPUSceneInstanceId>(m_instances.size());
            m_instances.push_back(instance);
        }
        m_instances[id].alive = true;
        m_instanceIds.emplace(sourceKey, id);
        appendUnique(m_dirty.instances, id);
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
        appendUnique(m_dirty.removedInstances, id);
    }

    GPUSceneMeshId GPUSceneDatabase::upsertMesh(const std::uint64_t sourceKey, const GPUMesh& mesh) {
        if (const auto found = m_meshIds.find(sourceKey); found != m_meshIds.end()) {
            m_meshes[found->second] = mesh;
            appendUnique(m_dirty.meshes, found->second);
            return found->second;
        }
        const auto id = static_cast<GPUSceneMeshId>(m_meshes.size());
        m_meshes.push_back(mesh);
        m_meshIds.emplace(sourceKey, id);
        appendUnique(m_dirty.meshes, id);
        return id;
    }

    GPUSceneMaterialId GPUSceneDatabase::upsertMaterial(const std::uint64_t sourceKey, const GPUMaterial& material) {
        if (const auto found = m_materialIds.find(sourceKey); found != m_materialIds.end()) {
            m_materials[found->second] = material;
            appendUnique(m_dirty.materials, found->second);
            return found->second;
        }
        const auto id = static_cast<GPUSceneMaterialId>(m_materials.size());
        m_materials.push_back(material);
        m_materialIds.emplace(sourceKey, id);
        appendUnique(m_dirty.materials, id);
        return id;
    }

    GPUSceneInstanceId GPUSceneDatabase::instanceId(const std::uint64_t sourceKey) const noexcept {
        const auto found = m_instanceIds.find(sourceKey);
        return found == m_instanceIds.end() ? InvalidGPUSceneInstanceId : found->second;
    }

    void GPUSceneDatabase::clearDirty() noexcept { m_dirty = {}; }

    void GPUSceneDatabase::clear() noexcept {
        m_instances.clear(); m_meshes.clear(); m_materials.clear(); m_freeInstances.clear();
        m_instanceIds.clear(); m_meshIds.clear(); m_materialIds.clear(); clearDirty();
    }
} // namespace Engine
