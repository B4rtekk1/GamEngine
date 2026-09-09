#include "Engine/Core/TaskScheduler.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace Engine {
    struct TaskState final {
        std::mutex mutex;
        std::condition_variable completed;
        bool done{};
        std::exception_ptr error;
        std::vector<std::function<void()>> continuations;
    };

    struct TaskScheduler::Impl final {
        struct QueuedTask final {
            Task task;
            std::shared_ptr<TaskState> state;
        };

        struct WorkerQueue final {
            std::mutex mutex;
            std::array<std::deque<QueuedTask>, 3> queues;
        };

        std::mutex wait_mutex;
        std::condition_variable available;
        std::atomic<bool> stopping{};
        std::atomic<std::size_t> pending{};
        std::atomic<std::size_t> next_queue{};
        std::vector<std::unique_ptr<WorkerQueue>> queues;
        std::vector<std::thread> workers;

        bool popTask(std::size_t worker_index, QueuedTask& result);
    };

    bool TaskScheduler::Impl::popTask(const std::size_t worker_index, QueuedTask& result) {
        const auto take = [&result, this](const std::size_t queue_index) {
            auto& worker_queue = *queues[queue_index];
            std::scoped_lock lock(worker_queue.mutex);
            for (auto& queue : worker_queue.queues) {
                if (!queue.empty()) {
                    result = std::move(queue.front());
                    queue.pop_front();
                    pending.fetch_sub(1, std::memory_order_release);
                    return true;
                }
            }
            return false;
        };
        if (take(worker_index)) return true;
        for (std::size_t offset = 1; offset < queues.size(); ++offset) {
            if (take((worker_index + offset) % queues.size())) return true;
        }
        return false;
    }

    namespace {
        constexpr std::size_t priorityIndex(const TaskPriority priority) noexcept {
            return static_cast<std::size_t>(priority);
        }

        void finish(const std::shared_ptr<TaskState>& state, std::exception_ptr error) {
            std::vector<std::function<void()>> continuations;
            {
                std::scoped_lock lock(state->mutex);
                state->error = std::move(error);
                state->done = true;
                continuations = std::move(state->continuations);
            }
            state->completed.notify_all();
            for (auto& continuation : continuations) continuation();
        }

    }

    TaskScheduler::TaskScheduler(const std::size_t worker_count) : impl_(std::make_unique<Impl>()) {
        const std::size_t count = worker_count == 0 ? 1 : worker_count;
        impl_->workers.reserve(count);
        impl_->queues.reserve(count);
        for (std::size_t worker = 0; worker < count; ++worker) {
            impl_->queues.push_back(std::make_unique<Impl::WorkerQueue>());
        }
        for (std::size_t worker = 0; worker < count; ++worker) {
            impl_->workers.emplace_back([this, worker] {
                for (;;) {
                    Impl::QueuedTask queued;
                    {
                        std::unique_lock lock(impl_->wait_mutex);
                        impl_->available.wait(lock, [this] {
                            return impl_->stopping.load(std::memory_order_acquire) ||
                                   impl_->pending.load(std::memory_order_acquire) != 0;
                        });
                        if (impl_->stopping.load(std::memory_order_acquire) &&
                            impl_->pending.load(std::memory_order_acquire) == 0) {
                            return;
                        }
                    }
                    if (!impl_->popTask(worker, queued)) continue;
                    try {
                        queued.task();
                        finish(queued.state, nullptr);
                    } catch (...) {
                        finish(queued.state, std::current_exception());
                    }
                }
            });
        }
    }

    TaskScheduler::~TaskScheduler() {
        impl_->stopping.store(true, std::memory_order_release);
        impl_->available.notify_all();
        for (auto& worker : impl_->workers) worker.join();
    }

    void TaskScheduler::enqueue(Task task, const TaskPriority priority, const std::shared_ptr<TaskState>& state) {
        if (!task) {
            finish(state, std::make_exception_ptr(std::invalid_argument("Cannot schedule an empty task")));
            return;
        }
        bool rejected = impl_->stopping.load(std::memory_order_acquire);
        if (!rejected) {
            const std::size_t queue_index = impl_->next_queue.fetch_add(1, std::memory_order_relaxed) % impl_->queues.size();
            auto& worker_queue = *impl_->queues[queue_index];
            std::scoped_lock lock(worker_queue.mutex);
            if (impl_->stopping.load(std::memory_order_acquire)) {
                rejected = true;
            } else {
                worker_queue.queues[priorityIndex(priority)].push_back({std::move(task), state});
                impl_->pending.fetch_add(1, std::memory_order_release);
            }
        }
        if (rejected) {
            finish(state, std::make_exception_ptr(std::runtime_error("TaskScheduler is stopping")));
            return;
        }
        impl_->available.notify_one();
    }

    TaskHandle TaskScheduler::schedule(Task task, const TaskPriority priority) {
        auto state = std::make_shared<TaskState>();
        enqueue(std::move(task), priority, state);
        return TaskHandle{std::move(state)};
    }

    TaskHandle TaskScheduler::scheduleAfter(const TaskHandle& dependency, Task task, const TaskPriority priority) {
        if (!dependency.state_) return schedule(std::move(task), priority);
        auto state = std::make_shared<TaskState>();
        auto submit = [this, state, task = std::move(task), priority]() mutable { enqueue(std::move(task), priority, state); };
        {
            std::scoped_lock lock(dependency.state_->mutex);
            if (!dependency.state_->done) {
                dependency.state_->continuations.push_back(std::move(submit));
                return TaskHandle{std::move(state)};
            }
        }
        submit();
        return TaskHandle{std::move(state)};
    }

    void TaskHandle::wait() const {
        if (!state_) return;
        std::unique_lock lock(state_->mutex);
        state_->completed.wait(lock, [this] { return state_->done; });
        if (state_->error) std::rethrow_exception(state_->error);
    }

    bool TaskHandle::ready() const noexcept {
        if (!state_) return true;
        std::scoped_lock lock(state_->mutex);
        return state_->done;
    }

    std::size_t TaskScheduler::worker_count() const noexcept { return impl_->workers.size(); }

    std::size_t TaskScheduler::default_worker_count() noexcept {
        const std::size_t cores = std::thread::hardware_concurrency();
        return cores > 2 ? cores - 2 : 1;
    }

    TaskScheduler& TaskScheduler::global() {
        static TaskScheduler scheduler;
        return scheduler;
    }
}
