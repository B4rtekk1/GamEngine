#pragma once

#include "vertex.h"

#include <cstdint>
#include <vector>

namespace Engine {

class Mesh final {
public:
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    [[nodiscard]] bool empty() const noexcept {
        return vertices.empty() || indices.empty();
    }

    [[nodiscard]] uint32_t vertexCount() const noexcept {
        return static_cast<uint32_t>(vertices.size());
    }

    [[nodiscard]] uint32_t indexCount() const noexcept {
        return static_cast<uint32_t>(indices.size());
    }
};

} // namespace Engine
