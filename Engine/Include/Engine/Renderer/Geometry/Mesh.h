#pragma once

#include "Engine/Renderer/Geometry/Vertex.h"
#include "Engine/Renderer/Materials/PBRMaterial.h"

#include <cstdint>
#include <filesystem>
#include <vector>

/**
 * @file Mesh.h
 * @brief Defines the CPU-side mesh container used by the renderer.
 */

namespace Engine {
    /**
     * @brief Stores vertex data, indices, materials, and embedded image data for a mesh.
     *
     * The mesh owns its CPU-side arrays. GPU buffers and textures created from this
     * data are managed by the renderer or asset system rather than by this class.
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
        };

        /** @brief Vertex array referenced by the mesh indices. */
        std::vector<Vertex> vertices;

        /** @brief Index array used for indexed rendering. */
        std::vector<uint32_t> indices;

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
    };
} // namespace Engine
