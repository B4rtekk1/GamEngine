#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scene/Components/IdentityComponents.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {
struct Value { int value{}; };
std::uint64_t checksum{};

template<class Function>
void measure(std::string_view name, std::size_t iterations, Function&& function) {
    for (std::size_t index = 0; index < 32; ++index) function();
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < iterations; ++index) function();
    const double elapsed = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << name << ": " << elapsed / static_cast<double>(iterations) << " us/op\n";
}
}

int main() {
    Engine::Registry registry;
    const auto entity = registry.create();
    registry.add<Value>(entity);
    for (int index = 0; index < 5000; ++index) registry.markChanged<Value>(entity);
    const auto revision = registry.componentRevision<Value>();
    measure("unchanged ECS callback", 100000, [&] {
        registry.forEachComponentChangedSince<Value>(revision,
            [](Engine::Entity item) { checksum += item; });
    });
    measure("one ECS change, unique result", 20000, [&] {
        checksum += registry.componentEntitiesChangedSince<Value>(revision - 1).size();
    });

    const auto root = registry.create();
    registry.add<Engine::Transform>(root);
    registry.add<Engine::UUIDComponent>(root, Engine::UUIDComponent{1});
    Engine::Entity leaf{};
    for (int index = 0; index < 20000; ++index) {
        leaf = registry.create();
        registry.add<Engine::Transform>(leaf);
        registry.add<Engine::ParentComponent>(leaf, Engine::ParentComponent{1});
    }
    Engine::TransformSystem::updateDirty(registry);
    measure("unchanged transform hierarchy", 100000, [&] {
        Engine::TransformSystem::updateDirty(registry);
    });
    measure("one dirty leaf among 20000 siblings", 300, [&] {
        registry.modify<Engine::Transform>(leaf, [](auto& transform) {
            transform.position = {transform.position.x() + 1.0F, 0.0F, 0.0F};
        });
        Engine::TransformSystem::updateDirty(registry);
        checksum += registry.get<Engine::Transform>(leaf).worldRevision();
    });
    Engine::TransformSystem::invalidate(registry);
    std::cout << "checksum: " << checksum << '\n';
}
