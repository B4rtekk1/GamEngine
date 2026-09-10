#pragma once

#include "Engine/Renderer/Geometry/Vertex.h"
#include "Engine/Renderer/Materials/PBRMaterial.h"
#include "Engine/Assets/AssetTypes.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

/**
 * @file Mesh.h
 * @brief Defines decoded mesh source data used before GPU upload.
 */

namespace Engine {
    /**
     * @brief Stores decoded vertex data, indices, materials, and embedded image data.
     *
     * GPU buffers and textures created from this data are managed by the renderer.
     * Keeping source arrays is intentional only while an asset needs CPU access
     * (editing, collision cooking, or a renderer resource rebuild).  Once a
     * mesh has a persistent GPU resource and any requested collider is cooked,
     * releaseSourceData() frees the decoded payload.
     */
    class Mesh final {
    public:
        /**
         * @brief Stores one embedded RGBA image associated with the mesh.
         *
         * Pixels are stored in row-major order as four 8-bit channels per pixel.
         */
        struct Image final {
            /** @brief Image width in pixels. */
            uint32_t width{};

            /** @brief Image height in pixels. */
            uint32_t height{};

            /** @brief Raw RGBA8 pixel data. */
            std::vector<std::uint8_t> rgbaPixels;

            /** GPU-ready data loaded from a cooked .gtex sidecar. */
            std::optional<Assets::CookedTexture> cooked;

            /** Header-only reference to a cooked texture; its payload is streamed at upload time. */
            std::optional<Assets::GtexTexture> gtex;

            /** Path stored by .gmesh, relative to that mesh file. */
            std::filesystem::path cookedPath;
        };

        /** @brief Vertex array referenced by the mesh indices. */
        std::vector<Vertex> vertices;

        /** @brief Index array used for indexed rendering. */
        std::vector<uint32_t> indices;

        /**
         * @brief Monotonically increasing version of the mesh's collision geometry.
         *
         * Call markGeometryChanged() after changing vertices or indices in place.
         * Systems which cache derived geometry (for example PhysX cooked meshes)
         * use this value to avoid reusing stale data.
         */
        std::uint64_t geometryRevision{};

        /** @brief Physically based materials used by the mesh. */
        std::vector<PBRMaterial> materials;

        /** @brief Images embedded in or associated with the mesh. */
        std::vector<Image> images;

        /**
         * @brief Source asset used to create this mesh, when one exists.
         *
         * Scene persistence stores this reference instead of expanding imported
         * geometry and decoded textures into the text scene format.
         */
        std::filesystem::path sourcePath;

        /**
         * @brief Checks whether the mesh lacks either vertices or indices.
         * @return true if the mesh cannot produce indexed geometry; otherwise false.
         */
        [[nodiscard]] bool empty() const noexcept {
            return vertices.empty() || indices.empty();
        }

        /**
         * @brief Returns the number of vertices.
         * @return Vertex count converted to uint32_t.
         */
        [[nodiscard]] uint32_t vertexCount() const noexcept {
            return static_cast<uint32_t>(vertices.size());
        }

        /**
         * @brief Returns the number of indices.
         * @return Index count converted to uint32_t.
         */
        [[nodiscard]] uint32_t indexCount() const noexcept {
            return static_cast<uint32_t>(indices.size());
        }

        /** @brief Marks CPU-side vertex or index geometry as having changed. */
        void markGeometryChanged() noexcept { ++geometryRevision; }

        /**
         * @brief Releases decoded CPU geometry and image payloads.
         *
         * This is a terminal operation for the current decoded representation:
         * callers must retain or be able to reload source data before calling
         * it.  In particular, it must run only after GPU upload and PhysX
         * cooking have completed.  Materials are retained because they are
         * small authored data and can still be used to create material tables.
         */
        void releaseSourceData() noexcept {
            std::vector<Vertex>{}.swap(vertices);
            std::vector<uint32_t>{}.swap(indices);
            for (Image& image : images) {
                std::vector<std::uint8_t>{}.swap(image.rgbaPixels);
                image.cooked.reset();
                image.gtex.reset();
            }
            std::vector<Image>{}.swap(images);
        }

        /** @brief Whether decoded CPU geometry is still available. */
        [[nodiscard]] bool hasSourceGeometry() const noexcept {
            return !vertices.empty() && !indices.empty();
        }
    };

    /**
     * Explicit name for decoded, CPU-resident asset data.  Mesh is retained as
     * the compatibility spelling while call sites are migrated to source/GPU
     * resource ownership.
     */
    using MeshSourceData = Mesh;
} // namespace Engine
