#include "fw03/common/task_executor.h"

#include <utility>

namespace fw03::common {

SerialExecutor::SerialExecutor() : worker_([this] { Run(); }) {}

SerialExecutor::~SerialExecutor() { Shutdown(); }

bool SerialExecutor::Post(std::function<void()> task) {
    if (!task) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return false;
        }
        tasks_.push_back(std::move(task));
    }
    work_available_.notify_one();
    return true;
}

void SerialExecutor::Drain() {
    std::unique_lock<std::mutex> lock(mutex_);
    drained_.wait(lock, [this] { return tasks_.empty() && active_tasks_ == 0U; });
}

void SerialExecutor::Shutdown() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }
    work_available_.notify_all();
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
            task = std::move(tasks_.front());
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
