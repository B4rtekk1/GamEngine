#pragma once

#include <Engine/Math/Math.h>
#include <Engine/Renderer/Geometry/Mesh.h>

namespace Engine {

    /**
     * @brief Renderable scene object with local transformation and geometry.
     *
     * This class stores a mesh and its local transform. The model matrix is built
     * in translation-rotation-scale order.
     *
     * @note This class is currently a direct renderable object rather than an ECS
     * wrapper around an Entity and Registry.
     */
    class GameObject {
    public:
        /**
         * @brief Destroys the game object.
         */
        virtual ~GameObject() = default;

        /**
         * @brief Geometry rendered for this object.
         */
        Mesh mesh;

        /**
         * @brief Local-space position of the object.
         */
        Vec3 position{0.0f, 0.0f, 0.0f};

        /**
         * @brief Local-space orientation of the object.
         */
        Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};

        /**
         * @brief Local-space scale of the object.
         */
        Vec3 scale{1.0f, 1.0f, 1.0f};

        /**
         * @brief Determines whether the object participates in shadow rendering.
         */
        bool castShadow{true};

        /**
         * @brief Builds the object's model matrix.
         *
         * The resulting transformation is composed as:
         * @code
         * translation * rotation * scale
         * @endcode
         *
         * @return Model matrix transforming local coordinates into world space.
         */
        [[nodiscard]] Mat4 modelMatrix() const noexcept {
            return Mat4::translate(position) *
                   Mat4::rotate(rotation) *
                   Mat4::scale(Mat4{}, scale);
        }
    };

}