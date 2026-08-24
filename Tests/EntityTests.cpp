#include <Engine/ECS/Entity.h>
#include <Engine/ECS/Registry.h>

int main() {
    using namespace Engine;

    if (NullEntity != 0 || NullObjectId != 0 || entityIndex(NullEntity) != 0 ||
        entityGeneration(NullEntity) != 0 || makeEntity(0, 0) != NullEntity) return 1;

    constexpr Entity encoded = makeEntity(0x12345678u, 0x9abcdef0u);
    if (entityIndex(encoded) != 0x12345678u || entityGeneration(encoded) != 0x9abcdef0u ||
        makeEntity(entityIndex(encoded), entityGeneration(encoded)) != encoded) return 2;

    Registry registry;
    const Entity first = registry.create();
    if (!registry.valid(first) || entityIndex(first) == 0) return 3;
    const auto index = entityIndex(first);
    const auto generation = entityGeneration(first);
    registry.destroy(first);
    if (registry.valid(first)) return 4;

    const Entity replacement = registry.create();
    if (entityIndex(replacement) != index || replacement == first ||
        entityGeneration(replacement) == generation || !registry.valid(replacement)) return 5;

    registry.destroy(replacement);
    if (registry.valid(replacement) || registry.size() != 0) return 6;
    return 0;
}
