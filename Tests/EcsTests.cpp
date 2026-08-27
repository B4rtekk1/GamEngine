#include <gtest/gtest.h>

#include "Engine/ECS/Registry.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace {

struct Position { int x{}; };
struct Velocity { int x{}; };

TEST(Entity, PacksAndUnpacksIndexAndGeneration) {
    const Engine::Entity entity = Engine::makeEntity(42, 7);
    EXPECT_EQ(Engine::entityIndex(entity), 42u);
    EXPECT_EQ(Engine::entityGeneration(entity), 7u);
    EXPECT_EQ(Engine::NullEntity, 0u);
}

TEST(ComponentPool, AddsGetsRemovesAndKeepsDenseStorageValid) {
    Engine::ComponentPool<Position> pool;
    const auto first = Engine::makeEntity(1, 0);
    const auto second = Engine::makeEntity(3, 0);
    pool.add(first, Position{10});
    pool.add(second, Position{20});
    EXPECT_EQ(pool.size(), 2u);
    EXPECT_EQ(pool.get(first).x, 10);
    pool.remove(first);
    EXPECT_FALSE(pool.has(first));
    ASSERT_TRUE(pool.has(second));
    EXPECT_EQ(pool.get(second).x, 20);
    EXPECT_EQ(pool.size(), 1u);
    EXPECT_THROW(pool.get(first), std::out_of_range);
    EXPECT_THROW(pool.add(second, Position{}), std::logic_error);
    pool.remove(first);
    EXPECT_EQ(pool.size(), 1u);
}

TEST(ComponentPool, ClonesOnlyExistingSourceComponents) {
    Engine::ComponentPool<Position> pool;
    const auto source = Engine::makeEntity(2, 0);
    const auto target = Engine::makeEntity(4, 0);
    pool.clone(source, target);
    EXPECT_FALSE(pool.has(target));
    pool.add(source, Position{55});
    pool.clone(source, target);
    ASSERT_TRUE(pool.has(target));
    EXPECT_EQ(pool.get(target).x, 55);
}

TEST(Registry, CreatesDestroysAndReusesEntitiesWithNewGeneration) {
    Engine::Registry registry;
    const auto first = registry.create();
    EXPECT_TRUE(registry.valid(first));
    EXPECT_EQ(registry.size(), 1u);
    registry.destroy(first);
    EXPECT_FALSE(registry.valid(first));
    EXPECT_EQ(registry.size(), 0u);
    const auto reused = registry.create();
    EXPECT_EQ(Engine::entityIndex(reused), Engine::entityIndex(first));
    EXPECT_EQ(Engine::entityGeneration(reused), Engine::entityGeneration(first) + 1);
    EXPECT_NE(reused, first);
}

TEST(Registry, ManagesComponentsAndReportsErrorsForInvalidOperations) {
    Engine::Registry registry;
    const auto entity = registry.create();
    registry.add<Position>(entity, Position{10});
    EXPECT_TRUE(registry.has<Position>(entity));
    EXPECT_EQ(registry.get<Position>(entity).x, 10);
    EXPECT_THROW(registry.add<Position>(entity, Position{}), std::logic_error);
    EXPECT_THROW(registry.add<Position>(Engine::NullEntity, Position{}), std::invalid_argument);
    registry.remove<Position>(entity);
    EXPECT_FALSE(registry.has<Position>(entity));
    EXPECT_THROW(registry.get<Position>(entity), std::out_of_range);
    EXPECT_THROW(registry.remove<Position>(Engine::NullEntity), std::invalid_argument);
}

TEST(Registry, ClonesComponentsAndDoesNotCloneInvalidEntities) {
    Engine::Registry registry;
    const auto source = registry.create();
    registry.add<Position>(source, Position{10});
    registry.add<Velocity>(source, Velocity{3});
    const auto clone = registry.clone(source);
    ASSERT_NE(clone, Engine::NullEntity);
    EXPECT_NE(clone, source);
    EXPECT_EQ(registry.get<Position>(clone).x, 10);
    EXPECT_EQ(registry.get<Velocity>(clone).x, 3);
    registry.get<Position>(clone).x = 99;
    EXPECT_EQ(registry.get<Position>(source).x, 10);
    EXPECT_EQ(registry.clone(Engine::NullEntity), Engine::NullEntity);
}

TEST(Registry, TracksMutationsAndPerComponentChanges) {
    Engine::Registry registry;
    const auto entity = registry.create();
    const auto baseline = registry.mutationRevision();
    registry.add<Position>(entity, Position{1});
    const auto afterAdd = registry.mutationRevision();
    EXPECT_GT(afterAdd, baseline);
    EXPECT_EQ(registry.componentRevision<Position>(), 1u);
    registry.modify<Position>(entity, [](Position& position) { position.x = 7; });
    EXPECT_EQ(registry.get<Position>(entity).x, 7);
    EXPECT_EQ(registry.componentRevision<Position>(), 2u);
    const auto changed = registry.componentEntitiesChangedSince<Position>(1);
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(changed.front(), entity);
    EXPECT_THROW(registry.markChanged<Velocity>(entity), std::out_of_range);
}

TEST(Registry, ViewsMatchingComponentsAndProtectsMutableIteration) {
    Engine::Registry registry;
    const auto both = registry.create();
    const auto positionOnly = registry.create();
    registry.add<Position>(both, Position{1});
    registry.add<Velocity>(both, Velocity{2});
    registry.add<Position>(positionOnly, Position{3});

    std::vector<Engine::Entity> visited;
    registry.view<Position, Velocity>([&](Engine::Entity entity, Position& position, Velocity& velocity) {
        visited.push_back(entity);
        position.x += velocity.x;
        EXPECT_THROW(registry.create(), std::logic_error);
    });
    ASSERT_EQ(visited.size(), 1u);
    EXPECT_EQ(visited.front(), both);
    EXPECT_EQ(registry.get<Position>(both).x, 3);

    const Engine::Registry& readOnlyRegistry = registry;
    int positionSum = 0;
    readOnlyRegistry.view<Position>([&](Engine::Entity, const Position& position) { positionSum += position.x; });
    EXPECT_EQ(positionSum, 6);
}

TEST(Registry, DestroyRemovesComponentsAndNoOpDestroyDoesNotChangeRevision) {
    Engine::Registry registry;
    const auto entity = registry.create();
    registry.add<Position>(entity, Position{1});
    const auto beforeDestroy = registry.mutationRevision();
    registry.destroy(entity);
    EXPECT_FALSE(registry.valid(entity));
    EXPECT_FALSE(registry.has<Position>(entity));
    EXPECT_GT(registry.mutationRevision(), beforeDestroy);
    const auto afterDestroy = registry.mutationRevision();
    registry.destroy(entity);
    EXPECT_EQ(registry.mutationRevision(), afterDestroy);
}

TEST(Registry, EmptyAndMissingComponentViewsDoNotInvokeCallbacks) {
    Engine::Registry registry;
    int invocations = 0;
    registry.view<Position>([&](Engine::Entity, Position&) { ++invocations; });
    EXPECT_EQ(invocations, 0);
    const auto entity = registry.create();
    registry.add<Position>(entity, Position{1});
    registry.view<Position, Velocity>([&](Engine::Entity, Position&, Velocity&) { ++invocations; });
    EXPECT_EQ(invocations, 0);
}

TEST(Registry, EmptyViewVisitsEveryLiveEntityAndRemovalTracksRevision) {
    Engine::Registry registry;
    const auto first = registry.create();
    const auto second = registry.create();
    std::vector<Engine::Entity> entities;
    registry.view<>([&](Engine::Entity entity) { entities.push_back(entity); });
    EXPECT_EQ(entities.size(), 2u);
    EXPECT_TRUE(std::find(entities.begin(), entities.end(), first) != entities.end());
    EXPECT_TRUE(std::find(entities.begin(), entities.end(), second) != entities.end());

    registry.add<Position>(first, Position{});
    const auto beforeRemoval = registry.componentRevision<Position>();
    registry.remove<Position>(first);
    EXPECT_EQ(registry.componentRevision<Position>(), beforeRemoval + 1);
    const auto changed = registry.componentEntitiesChangedSince<Position>(beforeRemoval);
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(changed.front(), first);
}

} // namespace
