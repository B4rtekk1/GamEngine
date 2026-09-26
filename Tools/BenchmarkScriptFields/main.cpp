#include "Engine/ECS/Components/ScriptComponent.h"

#include <chrono>
#include <iostream>
#include <vector>

namespace {
class BenchmarkScript final : public Engine::Script {};
}

int main() {
    Engine::ScriptRegistry registry;
    Engine::ScriptClassDescriptor descriptor{
        .name = "BenchmarkScript",
        .create = []() -> Engine::Script * { return new BenchmarkScript; },
        .destroy = [](Engine::Script *script) { delete script; },
    };
    for (int index = 0; index < 8; ++index) {
        descriptor.fields.push_back({
            .name = "field" + std::to_string(index),
            .type = Engine::ScriptFieldType::String,
            .defaultValue = std::string(64, 'a'),
        });
    }
    registry.registerClass(descriptor);
    std::vector<Engine::ScriptComponent> components;
    for (int index = 0; index < 1000; ++index) {
        components.emplace_back(descriptor.name);
        registry.syncFields(components.back());
    }
    const auto update = [&] {
        for (auto &component : components) registry.syncFields(component);
    };
    for (int warmup = 0; warmup < 32; ++warmup) update();
    constexpr int iterations = 500;
    const auto start = std::chrono::steady_clock::now();
    for (int index = 0; index < iterations; ++index) update();
    const auto elapsed = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - start).count();
    std::size_t checksum = 0;
    for (const auto &component : components) {
        for (const auto &[name, value] : component.fields) {
            checksum += name.size() + std::get<std::string>(value).size();
        }
    }
    std::cout << "1000 scripts, 8 string fields: " << elapsed / iterations
              << " us/update\nchecksum: " << checksum << '\n';
}
