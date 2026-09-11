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

TEST(RenderGraphTests, CreatesTimelineWaitOnlyForCrossQueueResourceDependency) {
    RenderGraph graph;
    TextureHandle depth;
    TextureHandle hiz;
    graph.addPass("Depth", [&](PassBuilder& builder) {
        depth = builder.writeTexture("Depth", ColorTarget, TextureUsage::DepthAttachment);
    }, {}, QueueClass::Graphics);
    graph.addPass("HiZ", [&](PassBuilder& builder) {
        builder.read(depth, TextureUsage::SampledReadCompute);
        hiz = builder.writeTexture("HiZ", ColorTarget, TextureUsage::StorageWriteCompute);
    }, {}, QueueClass::AsyncCompute);
    graph.addPass("Shadow", [](PassBuilder&) {}, {}, QueueClass::Graphics);
    graph.addPass("Cull", [&](PassBuilder& builder) {
        builder.read(hiz, TextureUsage::SampledReadCompute);
    }, {}, QueueClass::AsyncCompute);
    graph.compile();

    const auto& plan = graph.submissionPlan();
    ASSERT_EQ(plan.size(), 4U);
    EXPECT_EQ(plan[0].queue, QueueClass::Graphics);
    EXPECT_EQ(plan[1].queue, QueueClass::Graphics);
    EXPECT_EQ(plan[2].queue, QueueClass::AsyncCompute);
    EXPECT_EQ(plan[3].queue, QueueClass::AsyncCompute);
    EXPECT_TRUE(plan[1].waits.empty());
    ASSERT_EQ(plan[2].waits.size(), 1U);
    EXPECT_EQ(plan[2].waits[0].producerBatch, 0U);
    EXPECT_TRUE(plan[3].waits.empty());
}
} // namespace Engine::Renderer
