#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <cstdint>
#include <functional>
#include <span>

namespace Engine { class Mesh; }

namespace Engine::Assets {
    [[nodiscard]] std::shared_ptr<const Mesh> load_fbx_mesh(const std::filesystem::path& path,
                                                             bool forCooking = false);
    [[nodiscard]] std::optional<std::uint64_t> fbx_source_hash(const std::filesystem::path& path);
    using FbxEmbeddedImageVisitor = std::function<bool(std::uint32_t, std::span<const std::uint8_t>,
                                                        std::uint32_t, std::uint32_t)>;
    [[nodiscard]] bool visit_fbx_embedded_images(const std::filesystem::path& path,
                                                  const FbxEmbeddedImageVisitor& visitor);
}
