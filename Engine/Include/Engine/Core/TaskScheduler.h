#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace Engine {
enum class TaskPriority { High, Normal, Low };

struct TaskState;

/** Handle for a task submitted to TaskScheduler. */
class TaskHandle final {
  public:
    TaskHandle() = default;

    void wait() const;

    [[nodiscard]] bool ready() const noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(state_);
    }

  private:
    friend class TaskScheduler;

    explicit TaskHandle(std::shared_ptr<TaskState> state) : state_(std::move(state)) {}

    std::shared_ptr<TaskState> state_;
};

/**
 * Shared CPU task pool for engine subsystems.  The default worker count
 * reserves capacity for the main/render threads and OS/driver work.
 */
class TaskScheduler final {
  public:
    using Task = std::function<void()>;

    explicit TaskScheduler(std::size_t worker_count = default_worker_count());

    ~TaskScheduler();

    TaskScheduler(const TaskScheduler&) = delete;

    TaskScheduler& operator=(const TaskScheduler&) = delete;

    [[nodiscard]] TaskHandle schedule(Task task, TaskPriority priority = TaskPriority::Normal);

    [[nodiscard]] TaskHandle scheduleAfter(const TaskHandle& dependency, Task task,
                                           TaskPriority priority = TaskPriority::Normal);

    template <typename Function>
    [[nodiscard]] std::vector<TaskHandle> parallelFor(const std::size_t count, Function&& function,
                                                      const TaskPriority priority = TaskPriority::Normal) {
        if (count == 0) {
            return {};
        }
        // A task per element used to allocate a shared state, lock and notify
        // for every index. Chunking keeps the work queue short while still
        // giving every worker enough independent ranges to balance the load.
        const std::size_t targetChunks = std::max<std::size_t>(1, worker_count()) * 4;
        const std::size_t chunkCount = std::min(count, targetChunks);
        const std::size_t chunkSize = (count + chunkCount - 1) / chunkCount;

        std::vector<TaskHandle> tasks;
        tasks.reserve(chunkCount);
        auto shared_function = std::make_shared<std::decay_t<Function>>(std::forward<Function>(function));
        for (std::size_t begin = 0; begin < count; begin += chunkSize) {
            const std::size_t end = std::min(count, begin + chunkSize);
            tasks.push_back(schedule(
                [shared_function, begin, end] {
                    for (std::size_t index = begin; index < end; ++index) {
                        (*shared_function)(index);
                    }
                },
                priority));
        }
        return tasks;
    }

    [[nodiscard]] std::size_t worker_count() const noexcept;

    [[nodiscard]] static std::size_t default_worker_count() noexcept;

    [[nodiscard]] static TaskScheduler& global();

  private:
    struct Impl;

    void enqueue(Task task, TaskPriority priority, const std::shared_ptr<TaskState>& state);

    std::unique_ptr<Impl> impl_;
};
} // namespace Engine
