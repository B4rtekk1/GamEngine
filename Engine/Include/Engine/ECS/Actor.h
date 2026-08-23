#pragma once

#include "Engine/Core/Transform.h"
#include "Engine/ECS/Entity.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Renderer/Materials/PBRMaterial.h"

#include <cstdint>
#include <memory>
#include <string>

namespace Engine {

class Scene;
class GameObject;

/**
 * High-level, non-owning handle to an object in a Scene.
 *
 * Actor intentionally does not expose Registry or generic component access.
 * It is safe to copy and is resolved through the scene's stable ObjectId.
 */
class Actor final {
public:
    Actor() = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] ObjectId id() const noexcept { return objectId_; }
    [[nodiscard]] std::string name() const;

    void setName(std::string name);
    void setPosition(Vec3 position);
    void setRotation(Vec3 rotation);
    void setScale(Vec3 scale);
    [[nodiscard]] Vec3 position() const;
    [[nodiscard]] Vec3 rotation() const;
    [[nodiscard]] Vec3 scale() const;

    void setMesh(std::shared_ptr<const Mesh> mesh);
    void setMaterial(PBRMaterial material);
    void setCastShadow(bool enabled);
    void setCullingBatch(std::uint32_t batch);

    void addRigidbody(RigidbodyComponent body = {});
    void addScript(std::string className, bool enabled = true);
    void destroy();

private:
    friend class Scene;
    Actor(Scene& scene, ObjectId objectId) noexcept : scene_(&scene), objectId_(objectId) {}

    GameObject& object() const;

    Scene* scene_{};
    ObjectId objectId_{NullObjectId};
};

} // namespace Engine
