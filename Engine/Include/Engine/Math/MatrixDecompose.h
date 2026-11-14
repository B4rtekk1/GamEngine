#pragma once

/** @file MatrixDecompose.h Fast TRS decomposition for affine world matrices. */

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <cmath>

namespace Engine {
/**
 * @brief Decomposes @p matrix into scale, rotation and translation.
 *
 * Scene world matrices are affine: their projective row is (0, 0, 0, 1).
 * For those, this performs exactly the arithmetic glm::decompose() performs
 * after its perspective handling, so results are bit-identical, but skips the
 * 4x4 copies, the sixteen divisions by m[3][3] and the 4x4 determinant. Any
 * other matrix is forwarded to glm::decompose(). Shear is removed the same
 * way glm removes it and is not reported.
 *
 * @return false for a singular matrix, like glm::decompose().
 */
[[nodiscard]] inline bool decomposeAffineTrs(const glm::mat4& matrix, glm::vec3& scale, glm::quat& rotation,
                                             glm::vec3& translation) noexcept {
    if (matrix[0][3] != 0.0F || matrix[1][3] != 0.0F || matrix[2][3] != 0.0F || matrix[3][3] != 1.0F) {
        glm::vec3 skew{};
        glm::vec4 perspective{};
        return glm::decompose(matrix, scale, rotation, translation, skew, perspective);
    }
    glm::vec3 row[3]{glm::vec3{matrix[0]}, glm::vec3{matrix[1]}, glm::vec3{matrix[2]}};
    // With an identity projective row the 4x4 determinant glm tests equals
    // the upper 3x3 determinant.
    if (glm::epsilonEqual(glm::dot(row[0], glm::cross(row[1], row[2])), 0.0F, glm::epsilon<float>()))
        return false;
    translation = glm::vec3{matrix[3]};

    scale.x = glm::length(row[0]);
    row[0] = row[0] / glm::length(row[0]);
    const float shearXy = glm::dot(row[0], row[1]);
    row[1] = row[1] + row[0] * -shearXy;
    scale.y = glm::length(row[1]);
    row[1] = row[1] / glm::length(row[1]);
    const float shearXz = glm::dot(row[0], row[2]);
    row[2] = row[2] + row[0] * -shearXz;
    const float shearYz = glm::dot(row[1], row[2]);
    row[2] = row[2] + row[1] * -shearYz;
    scale.z = glm::length(row[2]);
    row[2] = row[2] / glm::length(row[2]);

    if (glm::dot(row[0], glm::cross(row[1], row[2])) < 0.0F) {
        for (int axis = 0; axis < 3; ++axis) {
            scale[axis] *= -1.0F;
            row[axis] *= -1.0F;
        }
    }

    const float trace = row[0].x + row[1].y + row[2].z;
    if (trace > 0.0F) {
        float root = std::sqrt(trace + 1.0F);
        rotation.w = 0.5F * root;
        root = 0.5F / root;
        rotation.x = root * (row[1].z - row[2].y);
        rotation.y = root * (row[2].x - row[0].z);
        rotation.z = root * (row[0].y - row[1].x);
    } else {
        constexpr int next[3] = {1, 2, 0};
        int i = 0;
        if (row[1].y > row[0].x)
            i = 1;
        if (row[2].z > row[i][i])
            i = 2;
        const int j = next[i];
        const int k = next[j];
#ifdef GLM_FORCE_QUAT_DATA_WXYZ
        constexpr int offset = 1;
#else
        constexpr int offset = 0;
#endif
        float root = std::sqrt(row[i][i] - row[j][j] - row[k][k] + 1.0F);
        rotation[i + offset] = 0.5F * root;
        root = 0.5F / root;
        rotation[j + offset] = root * (row[i][j] + row[j][i]);
        rotation[k + offset] = root * (row[i][k] + row[k][i]);
        rotation.w = root * (row[j][k] - row[k][j]);
    }
    return true;
}
} // namespace Engine
