#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace fw03::common {

class TaskExecutor {
public:
    virtual ~TaskExecutor() = default;
    virtual bool Post(std::function<void()> task) = 0;
    virtual bool PostCoalescing(std::uint64_t, std::function<void()> task) {
        return Post(std::move(task));
    }
    virtual bool PostEventCoalescing(std::uint64_t key, std::function<void()> task) {
        return PostCoalescing(key, std::move(task));
    }
    virtual bool PostEventCoalescing(
        std::uint64_t key,
        std::uint64_t generation,
        std::function<void()> task) {
        static_cast<void>(generation);
        return PostEventCoalescing(key, std::move(task));
    }
    virtual bool PostControl(
        std::function<void()> task,
        bool discard_pending_coalescing = false) {
        static_cast<void>(discard_pending_coalescing);
        return Post(std::move(task));
    }
    virtual bool PostConnectionControl(
        std::uint64_t obsolete_event_generation,
        std::function<void()> task) {
        static_cast<void>(obsolete_event_generation);
        return PostControl(std::move(task), false);
    }
};

class InlineExecutor final : public TaskExecutor {
public:
    bool Post(std::function<void()> task) override {
        if (!task) {
            return false;
        }
        try {
            task();
        } catch (...) {
            // Inline execution contains callback exceptions just like SerialExecutor.
        }
        return true;
    }
};

class SerialExecutor final : public TaskExecutor {
public:
    explicit SerialExecutor(std::size_t maximum_queue_depth = 256U);
    ~SerialExecutor() override;

    SerialExecutor(const SerialExecutor&) = delete;
    SerialExecutor& operator=(const SerialExecutor&) = delete;

    bool Post(std::function<void()> task) noexcept override;
    bool PostCoalescing(
        std::uint64_t key,
        std::function<void()> task) noexcept override;
    bool PostEventCoalescing(
        std::uint64_t key,
        std::function<void()> task) noexcept override;
    bool PostEventCoalescing(
        std::uint64_t key,
        std::uint64_t generation,
        std::function<void()> task) noexcept override;
    bool PostControl(
        std::function<void()> task,
        bool discard_pending_coalescing = false) noexcept override;
    bool PostConnectionControl(
        std::uint64_t obsolete_event_generation,
        std::function<void()> task) noexcept override;
    [[nodiscard]] std::size_t PendingTaskCount() const noexcept;
    [[nodiscard]] std::size_t RejectedTaskCount() const noexcept;
    [[nodiscard]] std::size_t RejectedControlTaskCount() const noexcept;
    [[nodiscard]] std::size_t CoalescedTaskCount() const noexcept;
    [[nodiscard]] std::size_t DiscardedCoalescingTaskCount() const noexcept;
    void Drain();
    void Shutdown() noexcept;

private:
    struct QueuedTask final {
        std::uint64_t key{0U};
        bool coalescing{false};
        bool control{false};
        bool discardable_event{false};
        std::uint64_t event_generation{0U};
        std::function<void()> task;
    };

    bool Enqueue(
        std::uint64_t key,
        bool coalescing,
        bool control,
        bool discardable_event,
        std::uint64_t event_generation,
        bool discard_pending_coalescing,
        bool discard_specific_event_generation,
        std::uint64_t obsolete_event_generation,
        std::function<void()> task) noexcept;
    void Run();

    mutable std::mutex mutex_;
    std::condition_variable work_available_;
    std::condition_variable drained_;
    std::deque<QueuedTask> tasks_;
    const std::size_t maximum_queue_depth_;
    static constexpr std::size_t kMaximumControlQueueDepth = 8U;
    std::thread worker_;
    const std::thread::id worker_id_;
    std::mutex join_mutex_;
    std::size_t active_tasks_{0U};
    std::size_t queued_data_tasks_{0U};
    std::size_t queued_control_tasks_{0U};
    std::size_t rejected_tasks_{0U};
    std::size_t rejected_control_tasks_{0U};
    std::size_t coalesced_tasks_{0U};
    std::size_t discarded_coalescing_tasks_{0U};
    bool stopping_{false};
};

}  // namespace fw03::common
