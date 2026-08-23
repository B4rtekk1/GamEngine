#pragma once

#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Renderer/Materials/PBRMaterial.h"

#include <filesystem>
#include <memory>

namespace Engine::Assets { class Content; }

namespace Engine {

/** Immutable, reusable description of a renderable actor. */
class Prefab final {
public:
    static Prefab model(Assets::Content& content, std::filesystem::path path,
                        PBRMaterial material = {});
    static Prefab cube(PBRMaterial material = {});

    [[nodiscard]] const std::shared_ptr<const Mesh>& mesh() const noexcept { return mesh_; }
    [[nodiscard]] const PBRMaterial& material() const noexcept { return material_; }
    [[nodiscard]] bool castShadow() const noexcept { return castShadow_; }
    [[nodiscard]] std::uint32_t cullingBatch() const noexcept { return cullingBatch_; }

    void setCastShadow(bool enabled) noexcept { castShadow_ = enabled; }
    void setCullingBatch(std::uint32_t batch) noexcept { cullingBatch_ = batch; }

private:
    Prefab(std::shared_ptr<const Mesh> mesh, PBRMaterial material)
        : mesh_(std::move(mesh)), material_(std::move(material)) {}

    std::shared_ptr<const Mesh> mesh_;
    PBRMaterial material_{};
    bool castShadow_{true};
    std::uint32_t cullingBatch_{0};
};

} // namespace Engine
