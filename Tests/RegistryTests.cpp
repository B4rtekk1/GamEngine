#include <Engine/ECS/Registry.h>
#include <Engine/ECS/GameObject.h>
#include <Engine/ECS/Components/MeshRendererComponent.h>
#include <Engine/Renderer/Geometry/Cube.h>

#include <cstddef>
#include <memory>

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
        meshRenderer.material.roughness != 0.55f) {
        return 2;
    }

    const Registry& constRegistry = registry;
    std::size_t constMatches = 0;
    constRegistry.view<Rare, Common>([&](Entity entity, const Rare& rare,
                                         const Common& common) {
        if (entity == matching && rare.value == 42 && common.value == 714) {
            ++constMatches;
        }
    });
    if (constMatches != 1) {
        return 3;
    }

    Engine::GameObject source(registry);
    source.spawn();
    source.transform().position.setX(12.0f);
    source.meshRenderer().mesh = std::make_shared<Engine::Mesh>(Engine::Cube::createMesh());
    source.add<CloneOnly>(73);

    auto copy = source.clone();
    if (!copy.isSpawned() || copy.entity() == source.entity() ||
        registry.size() != 1'004 || !copy.has<CloneOnly>() ||
        copy.get<CloneOnly>().value != 73 ||
        copy.transform().position.x() != 12.0f ||
        copy.meshRenderer().mesh != source.meshRenderer().mesh) {
        return 5;
    }

    copy.transform().position.setX(99.0f);
    copy.get<CloneOnly>().value = 11;
    if (source.transform().position.x() != 12.0f ||
        source.get<CloneOnly>().value != 73) {
        return 6;
    }

    return 0;
}
