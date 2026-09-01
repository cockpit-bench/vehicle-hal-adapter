#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace fw03::common {

class TaskExecutor {
public:
    virtual ~TaskExecutor() = default;
    virtual bool Post(std::function<void()> task) = 0;
};

class InlineExecutor final : public TaskExecutor {
public:
    bool Post(std::function<void()> task) override {
        if (!task) {
            return false;
        }
        task();
        return true;
    }
};

class SerialExecutor final : public TaskExecutor {
public:
    SerialExecutor();
    ~SerialExecutor() override;

    SerialExecutor(const SerialExecutor&) = delete;
    SerialExecutor& operator=(const SerialExecutor&) = delete;

    bool Post(std::function<void()> task) override;
    void Drain();
    void Shutdown() noexcept;

private:
    void Run();

    std::mutex mutex_;
    std::condition_variable work_available_;
    std::condition_variable drained_;
    std::deque<std::function<void()>> tasks_;
    std::thread worker_;
    std::size_t active_tasks_{0U};
    bool stopping_{false};
};

}  // namespace fw03::common
