#include "fw03/common/task_executor.h"

#include <utility>

namespace fw03::common {

SerialExecutor::SerialExecutor(std::size_t maximum_queue_depth)
    : maximum_queue_depth_(maximum_queue_depth == 0U ? 1U : maximum_queue_depth),
      worker_([this] { Run(); }),
      worker_id_(worker_.get_id()) {}

SerialExecutor::~SerialExecutor() { Shutdown(); }

bool SerialExecutor::Post(std::function<void()> task) noexcept {
    return Enqueue(0U, false, false, false, 0U, false, false, 0U, std::move(task));
}

bool SerialExecutor::PostCoalescing(
    std::uint64_t key,
    std::function<void()> task) noexcept {
    return Enqueue(key, true, false, false, 0U, false, false, 0U, std::move(task));
}

bool SerialExecutor::PostEventCoalescing(
    std::uint64_t key,
    std::function<void()> task) noexcept {
    return PostEventCoalescing(key, 0U, std::move(task));
}

bool SerialExecutor::PostEventCoalescing(
    std::uint64_t key,
    std::uint64_t generation,
    std::function<void()> task) noexcept {
    return Enqueue(
        key, true, false, true, generation, false, false, 0U, std::move(task));
}

bool SerialExecutor::PostControl(
    std::function<void()> task,
    bool discard_pending_coalescing) noexcept {
    return Enqueue(
        0U,
        false,
        true,
        false,
        0U,
        discard_pending_coalescing,
        false,
        0U,
        std::move(task));
}

bool SerialExecutor::PostConnectionControl(
    std::uint64_t obsolete_event_generation,
    std::function<void()> task) noexcept {
    return Enqueue(
        0U,
        false,
        true,
        false,
        0U,
        false,
        true,
        obsolete_event_generation,
        std::move(task));
}

bool SerialExecutor::Enqueue(
    std::uint64_t key,
    bool coalescing,
    bool control,
    bool discardable_event,
    std::uint64_t event_generation,
    bool discard_pending_coalescing,
    bool discard_specific_event_generation,
    std::uint64_t obsolete_event_generation,
    std::function<void()> task) noexcept {
    if (!task) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return false;
        }
        if (discard_pending_coalescing || discard_specific_event_generation) {
            for (auto queued = tasks_.begin(); queued != tasks_.end();) {
                const bool generation_matches =
                    !discard_specific_event_generation ||
                    queued->event_generation == obsolete_event_generation;
                if (!queued->control && queued->discardable_event && generation_matches) {
                    queued = tasks_.erase(queued);
                    --queued_data_tasks_;
                    ++discarded_coalescing_tasks_;
                } else {
                    ++queued;
                }
            }
        }
        if (control) {
            if (queued_control_tasks_ >= kMaximumControlQueueDepth) {
                ++rejected_control_tasks_;
                return false;
            }
        } else if (coalescing) {
            for (auto& queued : tasks_) {
                if (!queued.control && queued.coalescing && queued.key == key) {
                    queued.task = std::move(task);
                    ++coalesced_tasks_;
                    return true;
                }
            }
        }
        if (!control && queued_data_tasks_ >= maximum_queue_depth_) {
            ++rejected_tasks_;
            return false;
        }
        tasks_.push_back(
            {key, coalescing, control, discardable_event, event_generation, std::move(task)});
        if (control) {
            ++queued_control_tasks_;
        } else {
            ++queued_data_tasks_;
        }
    } catch (...) {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            ++rejected_tasks_;
        } catch (...) {
        }
        return false;
    }
    work_available_.notify_one();
    return true;
}

std::size_t SerialExecutor::PendingTaskCount() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    } catch (...) {
        return maximum_queue_depth_;
    }
}

std::size_t SerialExecutor::RejectedTaskCount() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return rejected_tasks_;
    } catch (...) {
        return 0U;
    }
}

std::size_t SerialExecutor::RejectedControlTaskCount() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return rejected_control_tasks_;
    } catch (...) {
        return 0U;
    }
}

std::size_t SerialExecutor::CoalescedTaskCount() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return coalesced_tasks_;
    } catch (...) {
        return 0U;
    }
}

std::size_t SerialExecutor::DiscardedCoalescingTaskCount() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return discarded_coalescing_tasks_;
    } catch (...) {
        return 0U;
    }
}

void SerialExecutor::Drain() {
    std::unique_lock<std::mutex> lock(mutex_);
    drained_.wait(lock, [this] { return tasks_.empty() && active_tasks_ == 0U; });
}

void SerialExecutor::Shutdown() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    work_available_.notify_all();
    if (std::this_thread::get_id() == worker_id_) {
        // The owned worker cannot join itself. It marks the executor stopping
        // and returns; a subsequent call from the external owner (including
        // the destructor) performs the join after this task unwinds.
        return;
    }
    std::lock_guard<std::mutex> join_lock(join_mutex_);
    if (worker_.joinable()) {
        worker_.join();
    }
}

void SerialExecutor::Run() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_available_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) {
                drained_.notify_all();
                return;
            }
            if (tasks_.front().control) {
                --queued_control_tasks_;
            } else {
                --queued_data_tasks_;
            }
            task = std::move(tasks_.front().task);
            tasks_.pop_front();
            ++active_tasks_;
        }

        try {
            task();
        } catch (...) {
            // The executor owns thread lifetime, not application exception policy.
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            --active_tasks_;
            if (tasks_.empty() && active_tasks_ == 0U) {
                drained_.notify_all();
            }
        }
    }
}

}  // namespace fw03::common
