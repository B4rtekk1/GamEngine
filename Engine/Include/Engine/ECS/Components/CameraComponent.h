#pragma once

#include <algorithm>
#include <cassert>

namespace Engine
{
    /**
     * Projection used by a CameraComponent.
     */
    enum class CameraProjection : unsigned char
    {
        Perspective,
        Orthographic
    };

    /**
     * Camera settings attached to an entity.
     *
     * The camera does not own a transform. The entity's TransformComponent is
     * the source of the camera position and rotation. Keeping the two
     * components separate allows the renderer to use the same transform for
     * culling, shadows and camera rendering.
     *
     * Angles are stored in degrees. The renderer is responsible for converting
     * them to radians when building the projection matrix and for applying the
     * Vulkan Y-axis correction required by its graphics API.
     */
    struct CameraComponent final
    {
        CameraProjection projection = CameraProjection::Perspective;

        // Perspective projection field of view, in degrees.
        float fieldOfView = 60.0f;

        // Orthographic projection height in world units.
        float orthographicSize = 10.0f;

        // Clip planes. The near plane must be greater than zero and the far
        // plane must be farther away than the near plane.
        float nearClip = 0.1f;
        float farClip = 1000.0f;

        // Width / height of the render target. It is intentionally stored as
        // a ratio so resizing the swapchain only requires one component update.
        float aspectRatio = 16.0f / 9.0f;

        // Only one camera should normally be selected for the main view.
        bool primary = true;

        [[nodiscard]] bool isPerspective() const noexcept
        {
            return projection == CameraProjection::Perspective;
        }

        [[nodiscard]] bool isOrthographic() const noexcept
        {
            return projection == CameraProjection::Orthographic;
        }

        void setPerspective(float fovDegrees, float nearPlane, float farPlane) noexcept
        {
            projection = CameraProjection::Perspective;
            fieldOfView = std::clamp(fovDegrees, 1.0f, 179.0f);
            nearClip = std::max(0.0001f, nearPlane);
            farClip = std::max(nearClip + 0.0001f, farPlane);
        }

        void setOrthographic(float height, float nearPlane, float farPlane) noexcept
        {
            projection = CameraProjection::Orthographic;
            orthographicSize = std::max(0.0001f, height);
            nearClip = std::max(0.0001f, nearPlane);
            farClip = std::max(nearClip + 0.0001f, farPlane);
        }

        void setAspectRatio(float width, float height) noexcept
        {
            if (width > 0.0f && height > 0.0f)
                aspectRatio = width / height;
        }

        void setAspectRatio(float ratio) noexcept
        {
            if (ratio > 0.0f)
                aspectRatio = ratio;
        }

        [[nodiscard]] bool isValid() const noexcept
        {
            if (!(aspectRatio > 0.0f) || !(nearClip > 0.0f) || !(farClip > nearClip))
                return false;

            if (isPerspective())
                return fieldOfView > 0.0f && fieldOfView < 180.0f;

            return orthographicSize > 0.0f;
        }

        /**
         * Verifies invariants in debug builds. Setters already clamp values,
         * but this is useful after deserializing a scene file.
         */
        void validate() const noexcept
        {
            assert(isValid());
        }
    };
}