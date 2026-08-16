#pragma once

#include "Engine/Renderer/Geometry/Vertex.h"
#include "Engine/Renderer/Materials/PBRMateial.h"

#include <cstdint>
#include <vector>

namespace Engine {

class Mesh final {
public:
    struct Image final {
        uint32_t width{};
        uint32_t height{};
        std::vector<std::uint8_t> rgbaPixels;
    };

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<PBRMaterial> materials;
    std::vector<Image> images;

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
