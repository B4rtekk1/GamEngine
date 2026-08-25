#include <Engine/ECS/Registry.h>
#include <Engine/ECS/Components/TransformComponent.h>
#include <Engine/ECS/Components/MeshRendererComponent.h>
#include <Engine/Renderer/Geometry/Cube.h>

#include <cstddef>
#include <memory>
#include <stdexcept>

namespace {
struct Common {
    int value = 0;
};

struct Rare {
    int value = 0;
};

struct CloneOnly {
    int value = 0;
};
}

int main()
{
    using Engine::Entity;
    using Engine::Registry;

    Registry registry;
    Entity matching = Engine::NullEntity;
    for (int i = 0; i < 1'000; ++i) {
        const Entity entity = registry.create();
        registry.add<Common>(entity, i);
        if (i == 713) {
            matching = entity;
            registry.add<Rare>(entity, 42);
        }
    }

    std::size_t matches = 0;
    registry.view<Common, Rare>([&](Entity entity, Common& common, Rare& rare) {
        if (entity != matching || common.value != 713 || rare.value != 42) {
            matches = 99;
            return;
        }
        ++matches;
        common.value = 714;
    });
    if (matches != 1 || registry.get<Common>(matching).value != 714) {
        return 1;
    }

    const auto commonRevision = registry.componentRevision<Common>();
    static_cast<void>(registry.get<Common>(matching));
    if (registry.componentRevision<Common>() != commonRevision) {
        return 7;
    }
    registry.modify<Common>(matching, [](Common& component) { component.value = 715; });
    if (registry.componentRevision<Common>() != commonRevision + 1 ||
        registry.get<Common>(matching).value != 715) {
        return 8;
    }

    try {
        static_cast<void>(registry.get<Rare>(Engine::NullEntity));
        return 9;
    } catch (const std::out_of_range&) {
    }
    try {
        registry.add<Common>(matching, 1);
        return 10;
    } catch (const std::logic_error&) {
    }

    bool blockedStructuralMutation = false;
    registry.view<Common>([&](Entity, Common&) {
        try {
            static_cast<void>(registry.create());
        } catch (const std::logic_error&) {
            blockedStructuralMutation = true;
        }
    });
    if (!blockedStructuralMutation) {
        return 11;
    }

    const Entity recycled = registry.create();
    registry.add<Common>(recycled, 1);
    registry.destroy(recycled);
    const Entity replacement = registry.create();
    if (Engine::entityIndex(replacement) != Engine::entityIndex(recycled) ||
        replacement == recycled || registry.valid(recycled) || !registry.valid(replacement)) {
        return 4;
    }

    const Entity renderable = registry.create();
    registry.add<Engine::MeshRendererComponent>(
        renderable,
        Engine::MeshRendererComponent{
            .mesh = std::make_shared<Engine::Mesh>(Engine::Cube::createMesh()),
            .castShadow = false,
        });
    const auto& meshRenderer = registry.get<Engine::MeshRendererComponent>(renderable);
    if (!meshRenderer.hasMesh() || meshRenderer.castShadow ||
        meshRenderer.material.roughness != 0.55F) {
        return 2;
    }

    const Registry& constRegistry = registry;
    std::size_t constMatches = 0;
    constRegistry.view<Rare, Common>([&](Entity entity, const Rare& rare,
                                         const Common& common) {
        if (entity == matching && rare.value == 42 && common.value == 715) {
            ++constMatches;
        }
    });
    if (constMatches != 1) {
        return 3;
    }

    const Entity source = registry.create();
    registry.add<Engine::TransformComponent>(source).position.setX(12.0F);
    registry.add<Engine::MeshRendererComponent>(source,
        Engine::MeshRendererComponent{
            .mesh = std::make_shared<Engine::Mesh>(Engine::Cube::createMesh())});
    registry.add<CloneOnly>(source, 73);

    const Entity copy = registry.clone(source);
    if (copy == source || registry.size() != 1'004 || !registry.has<CloneOnly>(copy) ||
        registry.get<CloneOnly>(copy).value != 73 ||
        registry.get<Engine::TransformComponent>(copy).position.x() != 12.0F ||
        registry.get<Engine::MeshRendererComponent>(copy).mesh !=
            registry.get<Engine::MeshRendererComponent>(source).mesh) {
        return 5;
    }

    registry.modify<Engine::TransformComponent>(copy, [](auto& transform) {
        transform.position.setX(99.0F);
    });
    registry.get<CloneOnly>(copy).value = 11;
    if (registry.get<Engine::TransformComponent>(source).position.x() != 12.0F ||
        registry.get<CloneOnly>(source).value != 73) {
        return 6;
    }

    return 0;
}