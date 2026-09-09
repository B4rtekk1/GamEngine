#include "Engine/Renderer/RenderGraph/RenderGraph.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace Engine::Renderer {
namespace {
    constexpr TextureDesc ColorTarget{
        .extent = {1920, 1080, 1},
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
}

TEST(RenderGraphTests, OrdersPassesFromResourceDependencies) {
    RenderGraph graph;
    const auto input = graph.importTexture("Input", reinterpret_cast<VkImage>(static_cast<std::uintptr_t>(1)), ColorTarget,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TextureHandle bloom;
    graph.addPass("Bloom", [&](PassBuilder& builder) {
        builder.read(input);
        bloom = builder.writeTexture("Bloom", ColorTarget);
    }, {});
    graph.addPass("Tonemap", [&](PassBuilder& builder) { builder.read(bloom); }, {});

    graph.compile();
    EXPECT_EQ(graph.executionOrder(), (std::vector<std::string>{"Bloom", "Tonemap"}));
}

TEST(RenderGraphTests, ReusesAllocationSlotForNonOverlappingCompatibleTextures) {
    RenderGraph graph;
    TextureHandle first;
    TextureHandle second;
    TextureHandle gate;
    graph.addPass("First", [&](PassBuilder& builder) { first = builder.writeTexture("First", ColorTarget); }, {});
    graph.addPass("Consume first", [&](PassBuilder& builder) {
        builder.read(first);
        gate = builder.writeTexture("Gate", ColorTarget);
    }, {});
    graph.addPass("Second", [&](PassBuilder& builder) {
        builder.read(gate);
        second = builder.writeTexture("Second", ColorTarget);
    }, {});

    graph.compile();
    EXPECT_EQ(graph.lifetime(first).allocationSlot, graph.lifetime(second).allocationSlot);
}
} // namespace Engine::Renderer
