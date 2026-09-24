#pragma once

#include "Engine/Math/AABB.h"
#include "Engine/Renderer/Geometry/Mesh.h"

#include <cstdint>
#include <atomic>
#include <filesystem>
#include <limits>
#include <memory>
#include <type_traits>

namespace Engine {
    /** Small CPU resident description kept after decoded geometry is released. */
    struct MeshAssetMetadata final {
        AABB bounds{};
        std::vector<Mesh::RenderSection> sections;
        std::vector<PBRMaterial> materials;
        std::vector<std::filesystem::path> imagePaths;
        std::uint32_t vertexCount{};
        std::uint32_t indexCount{};
        std::uint32_t meshletCount{};
        std::uint32_t textureCount{};
    };
    /** Stable, renderer-owned identity of an uploaded mesh. */
    struct MeshId final {
        static constexpr std::uint32_t Invalid = std::numeric_limits<std::uint32_t>::max();

        std::uint32_t value{Invalid};

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != Invalid; }
        [[nodiscard]] constexpr bool operator==(const MeshId&) const noexcept = default;
    };

    /**
     * Range and bounds of one mesh in renderer-owned GPU buffers.
     *
     * This deliberately contains no decoded vertices, indices, or image
     * payload.  A MeshHandle remains valid for rendering after its source
     * asset has released those CPU arrays.
     */
    struct MeshGpuResource final {
        /** Process-unique identity survives source-data reloads and pointer reuse. */
        const std::uint64_t textureIdentity = [] {
            static std::atomic<std::uint64_t> next{1};
            return next.fetch_add(1, std::memory_order_relaxed);
        }();
        MeshId handle{};
        std::uint32_t firstVertex{};
        std::uint32_t vertexCount{};
        std::uint32_t firstIndex{};
        std::uint32_t indexCount{};
        /// Number of meshlets belonging to this LOD. Their streamed payload is
        /// addressed by the mesh-shader path, not by indexed fallback draws.
        std::uint32_t meshletCount{};
        /// Offset into the renderer-wide GpuMeshlet buffer for this mesh LOD.
        std::uint32_t firstMeshlet{};
        AABB bounds{};
        MeshAssetMetadata metadata{};
        /// Asset path used to decode a short-lived source payload on a GPU
        /// resource rebuild. Empty paths identify procedural/editor meshes.
        std::filesystem::path sourcePath;
        /// Procedural and actively edited meshes cannot be reconstructed from
        /// disk, so their source data remains explicitly pinned.
        bool retainSourceData{false};

        // Import ownership only.  Rendering must use the range above; source
        // data can be dropped after upload/collision cooking.
        std::shared_ptr<const MeshSourceData> sourceData;
    };

    /**
     * ECS-facing reference to a renderer resource.  The source accessor is
     * deliberately transitional: rendering code must eventually use only the
     * range stored in MeshGpuResource.
     */
    class MeshHandle final {
    public:
        MeshHandle() = default;
        MeshHandle(std::shared_ptr<const MeshSourceData> source)
            : resource_(std::make_shared<MeshGpuResource>()) {
            resource_->sourceData = std::move(source);
            if (resource_->sourceData) {
                resource_->sourcePath = resource_->sourceData->sourcePath;
                resource_->retainSourceData = resource_->sourcePath.empty();
                const Mesh& mesh = *resource_->sourceData;
                resource_->metadata.vertexCount = mesh.vertexCount();
                resource_->metadata.indexCount = mesh.indexCount();
                resource_->metadata.meshletCount = static_cast<std::uint32_t>(mesh.meshlets.size());
                resource_->metadata.textureCount = static_cast<std::uint32_t>(mesh.images.size());
                resource_->metadata.sections = mesh.renderSections;
                resource_->metadata.materials = mesh.materials;
                resource_->metadata.imagePaths.reserve(mesh.images.size());
                for (const Mesh::Image& image : mesh.images)
                    resource_->metadata.imagePaths.push_back(image.cookedPath);
                if (!mesh.vertices.empty()) {
                    glm::vec3 minimum{std::numeric_limits<float>::max()};
                    glm::vec3 maximum{std::numeric_limits<float>::lowest()};
                    for (const Vertex& vertex : mesh.vertices) {
                        minimum = glm::min(minimum, vertex.position.native());
                        maximum = glm::max(maximum, vertex.position.native());
                    }
                    resource_->metadata.bounds = {Vec3{minimum}, Vec3{maximum}};
                    resource_->bounds = resource_->metadata.bounds;
                }
            }
        }
        template <typename T>
            requires std::is_convertible_v<T*, const MeshSourceData*>
        MeshHandle(std::shared_ptr<T> source)
            : MeshHandle(std::shared_ptr<const MeshSourceData>{std::move(source)}) {}

        [[nodiscard]] explicit operator bool() const noexcept {
            return resource_ != nullptr;
        }
        [[nodiscard]] bool operator==(std::nullptr_t) const noexcept { return !static_cast<bool>(*this); }
        [[nodiscard]] bool operator!=(std::nullptr_t) const noexcept { return static_cast<bool>(*this); }
        [[nodiscard]] const std::shared_ptr<MeshGpuResource>& resource() const noexcept { return resource_; }
        [[nodiscard]] std::shared_ptr<const MeshSourceData> source() const noexcept {
            return resource_ ? resource_->sourceData : nullptr;
        }
        [[nodiscard]] bool hasSourceData() const noexcept {
            return resource_ != nullptr && resource_->sourceData != nullptr;
        }
        [[nodiscard]] bool uploaded() const noexcept {
            return resource_ != nullptr && static_cast<bool>(resource_->handle);
        }

        /**
         * Drops this render handle's ownership of decoded input data.
         *
         * A mesh collider deliberately keeps its own shared source reference
         * until PhysX has cooked it.  The resource metadata remains valid, so
         * draw submission never needs to retain a MeshSourceData pointer.
         */
        void releaseSourceReference() noexcept {
            if (resource_ && !resource_->retainSourceData) resource_->sourceData.reset();
        }

        /** Pins decoded data for an asset currently being edited. */
        void setSourceDataRetained(const bool retained) noexcept {
            if (resource_) resource_->retainSourceData = retained;
        }
    private:
        std::shared_ptr<MeshGpuResource> resource_;
    };
} // namespace Engine
