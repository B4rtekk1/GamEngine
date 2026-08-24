#include <Engine/Core/Transform.h>
#include <Engine/ECS/Components/ColliderComponent.h>
#include <Engine/Scene/Scene.h>

#include <vector>

int main() {
    using namespace Engine;

    Scene scene;
    static_cast<void>(scene.createActor("One"));
    static_cast<void>(scene.createActor("Two"));
    SceneEditor editor = scene.editor();
    if (editor.size() != 2 || editor.structuralRevision() == 0) return 1;

    std::vector<Entity> entities;
    editor.view<>([&](const Entity entity) { entities.push_back(entity); });
    if (entities.size() != 2 || !editor.valid(entities[0]) || !editor.valid(entities[1]) ||
        !editor.has<Transform>(entities[0])) return 2;

    const auto revisionBeforeAdd = editor.structuralRevision();
    editor.add<ColliderComponent>(entities[0], ColliderComponent{.shape = SphereCollider{1.5f}});
    if (!editor.has<ColliderComponent>(entities[0]) ||
        editor.structuralRevision() <= revisionBeforeAdd) return 3;
    editor.modify<Transform>(entities[0], [](auto& transform) {
        transform.position = {7.0f, 8.0f, 9.0f};
    });
    if (editor.get<Transform>(entities[0]).position.x() != 7.0f) return 4;

    const Entity copy = editor.duplicate(entities[0]);
    if (!editor.valid(copy) || editor.size() != 3 || copy == entities[0] ||
        editor.get<Transform>(copy).position.z() != 9.0f ||
        !editor.has<ColliderComponent>(copy)) return 5;

    editor.destroy(entities[1]);
    if (editor.valid(entities[1]) || editor.size() != 2) return 6;
    editor.destroy(entities[1]);
    if (editor.size() != 2) return 7;
    return 0;
}
