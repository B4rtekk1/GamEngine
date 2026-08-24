#include <Engine/ECS/Componentpool.h>

#include <stdexcept>

namespace {
struct Component {
    int value;
    explicit Component(const int value = 0) : value(value) {}
};
}

int main() {
    using namespace Engine;

    ComponentPool<Component> pool;
    const Entity first = makeEntity(3, 1);
    const Entity second = makeEntity(17, 2);
    const Entity replacementGeneration = makeEntity(3, 2);
    const Entity cloneTarget = makeEntity(23, 2);

    if (pool.size() != 0 || pool.has(NullEntity) || pool.has(first)) return 1;
    pool.add(first, 10);
    pool.add(second, 20);
    if (pool.size() != 2 || !pool.has(first) || !pool.has(second) ||
        pool.has(replacementGeneration) || pool.get(first).value != 10 ||
        pool.getUnchecked(second).value != 20) return 2;

    try {
        pool.add(first, 99);
        return 3;
    } catch (const std::logic_error&) {
    }
    try {
        static_cast<void>(pool.get(NullEntity));
        return 4;
    } catch (const std::out_of_range&) {
    }

    pool.remove(first);
    if (pool.size() != 1 || pool.has(first) || pool.has(replacementGeneration) ||
        pool.get(second).value != 20) return 5;

    pool.add(first, 10);
    pool.clone(first, cloneTarget);
    if (pool.size() != 3 || !pool.has(cloneTarget) ||
        pool.get(cloneTarget).value != 10) return 6;

    pool.clone(makeEntity(999, 1), makeEntity(1000, 1));
    if (pool.size() != 3) return 7;

    pool.remove(first);
    if (pool.size() != 2 || pool.has(first) || pool.get(second).value != 20 ||
        pool.get(cloneTarget).value != 10) return 8;
    pool.remove(first);
    if (pool.size() != 2) return 9;

    const ComponentPool<Component>& constPool = pool;
    if (constPool.entities().size() != 2 || constPool.get(second).value != 20 ||
        constPool.get(cloneTarget).value != 10) return 10;

    return 0;
}
