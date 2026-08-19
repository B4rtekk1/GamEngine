#pragma once

#include "Engine/Math/Vec3.h"
#include "Engine/Math/Vec2.h"
#include "Engine/Math/Vec4.h"

#include <cstdint>

/**
 * @file Vertex.h
 * @brief Defines the API-independent vertex representation used by the renderer.
 */

namespace Engine {

    /**
     * @brief API-independent vertex data used by meshes throughout the renderer.
     *
     * The member order is part of the vertex layout contract used by rendering
     * backends. Keep backend-sensitive members stable when extending this type.
     */
    struct Vertex {
        /** @brief Position in object or model space. */
        Vec3 position;

        /** @brief Per-vertex color. */
        Vec3 color;

        /** @brief Two-dimensional texture coordinates. */
        Vec2 texCoord;

        /** @brief Surface normal in object or model space. */
        Vec3 normal;

        /**
         * @brief Tangent frame data.
         *
         * The xyz components contain the tangent direction. The w component stores
         * its handedness, which is used to reconstruct the bitangent.
         */
        Vec4 tangent;

        /**
         * @brief Index into Mesh::materials.
         *
         * This member is kept at the end so existing Vulkan position, color, and
         * normal offsets remain stable.
         */
        std::uint32_t materialIndex{0};
    };

} // namespace Engine