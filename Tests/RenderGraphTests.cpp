#include "Engine/Renderer/RenderGraph/RenderGraph.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace Engine::RenderGraph {
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

TEST(RenderGraphTests, ReusesAllocationSlotForNonOverlappingCompatibleBuffers) {
    RenderGraph graph;
    constexpr BufferDesc scratch{.size = 4096, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    BufferHandle count;
    BufferHandle scatter;
    BufferHandle gate;
    graph.addPass("Count", [&](PassBuilder& builder) { count = builder.writeBuffer("Count", scratch); }, {});
    graph.addPass("Prefix", [&](PassBuilder& builder) {
        builder.read(count);
        gate = builder.writeBuffer("Gate", scratch);
    }, {});
    graph.addPass("Scatter", [&](PassBuilder& builder) {
        builder.read(gate);
        scatter = builder.writeBuffer("Scatter", scratch);
    }, {});
    graph.compile();
    EXPECT_EQ(graph.lifetime(count).allocationSlot, graph.lifetime(scatter).allocationSlot);
}

TEST(RenderGraphTests, RebuildsTheSameTopologyAfterLogicalReset) {
    RenderGraph graph;
    const auto build = [&graph] {
        TextureHandle intermediate;
        graph.addPass("Producer", [&](PassBuilder& builder) {
            intermediate = builder.writeTexture("Intermediate", ColorTarget);
        }, {});
        graph.addPass("Consumer", [&](PassBuilder& builder) { builder.read(intermediate); }, {});
    };
    build();
    graph.compile();
    EXPECT_EQ(graph.executionOrder(), (std::vector<std::string>{"Producer", "Consumer"}));

    graph.reset();
    build();
    graph.compile();
    EXPECT_EQ(graph.executionOrder(), (std::vector<std::string>{"Producer", "Consumer"}));
}

TEST(RenderGraphTests, ReportsTimelineDependenciesAcrossQueues) {
    RenderGraph graph;
    TextureHandle intermediate;
    graph.addPass("Depth", Queue::Graphics, [&](PassBuilder& builder) {
        intermediate = builder.writeTexture("Depth", ColorTarget);
    }, {});
    graph.addPass("Hi-Z", Queue::AsyncCompute, [&](PassBuilder& builder) {
        builder.read(intermediate, TextureUsage::SampledReadCompute);
    }, {});
    graph.compile();

    ASSERT_EQ(graph.queueDependencies().size(), 1U);
    const auto& dependency = graph.queueDependencies().front();
    EXPECT_EQ(dependency.producerPass, 0U);
    EXPECT_EQ(dependency.consumerPass, 1U);
    EXPECT_EQ(dependency.producerQueue, Queue::Graphics);
    EXPECT_EQ(dependency.consumerQueue, Queue::AsyncCompute);
}

TEST(RenderGraphTests, CullsPassesThatCannotReachAnExport) {
    RenderGraph graph;
    graph.enablePassCulling();
    TextureHandle visible;
    graph.addPass("Visible", [&](PassBuilder& builder) {
        visible = builder.writeTexture("Visible", ColorTarget);
    }, {});
    graph.addPass("Unused", [&](PassBuilder& builder) {
        [[maybe_unused]] const auto unused = builder.writeTexture("Unused", ColorTarget);
    }, {});
    graph.exportTexture(visible);
    graph.compile();

    EXPECT_EQ(graph.executionOrder(), (std::vector<std::string>{"Visible"}));
}

TEST(RenderGraphTests, BuildsQueueBatchesFromTheCompiledDag) {
    RenderGraph graph;
    TextureHandle depth;
    TextureHandle hiZ;
    graph.addPass("Forward", Queue::Graphics, [&](PassBuilder& builder) {
        depth = builder.writeTexture("Depth", ColorTarget);
    }, {});
    graph.addPass("Hi-Z", Queue::AsyncCompute, [&](PassBuilder& builder) {
        builder.read(depth, TextureUsage::SampledReadCompute);
        hiZ = builder.writeTexture("Hi-Z", ColorTarget);
    }, {});
    graph.addPass("Tonemap", Queue::Graphics, [&](PassBuilder& builder) {
        builder.read(hiZ);
    }, {});
    graph.compile();

    ASSERT_EQ(graph.queueBatches().size(), 3U);
    EXPECT_EQ(graph.queueBatches()[0].queue, Queue::Graphics);
    EXPECT_EQ(graph.queueBatches()[1].queue, Queue::AsyncCompute);
    EXPECT_EQ(graph.queueBatches()[1].waitBatches, (std::vector<std::uint32_t>{0U}));
    EXPECT_EQ(graph.queueBatches()[2].waitBatches, (std::vector<std::uint32_t>{1U}));
}
} // namespace Engine::Renderer
