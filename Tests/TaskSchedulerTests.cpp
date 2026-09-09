#include "Engine/Core/TaskScheduler.h"

#include <gtest/gtest.h>

#include <atomic>
#include <numeric>
#include <vector>

namespace Engine {
    TEST(TaskScheduler, RunsDependentTaskAfterPrerequisite) {
        TaskScheduler scheduler{2};
        std::atomic<int> value{};

        const TaskHandle first = scheduler.schedule([&] { value.store(7); });
        const TaskHandle second = scheduler.scheduleAfter(first, [&] { value.fetch_add(5); });

        second.wait();
        EXPECT_EQ(value.load(), 12);
    }

    TEST(TaskScheduler, ParallelForRunsEveryIndexOnce) {
        TaskScheduler scheduler{2};
        std::vector<std::atomic<int>> visits(32);
        const auto tasks = scheduler.parallelFor(visits.size(), [&](const std::size_t index) {
            visits[index].fetch_add(1);
        });

        for (const TaskHandle& task : tasks) task.wait();
        EXPECT_EQ(std::accumulate(visits.begin(), visits.end(), 0,
                                  [](const int sum, const std::atomic<int>& visit) { return sum + visit.load(); }),
                  static_cast<int>(visits.size()));
    }
}
