#pragma once

#include "fw03/common/clock.h"

#include <atomic>
#include <chrono>

namespace fw03::test {

class ManualClock final : public common::Clock {
public:
    [[nodiscard]] TimePoint Now() const noexcept override {
        return TimePoint(std::chrono::nanoseconds(now_ns_.load()));
    }

    void Advance(std::chrono::nanoseconds duration) noexcept {
        now_ns_.fetch_add(duration.count());
    }

private:
    std::atomic<std::chrono::nanoseconds::rep> now_ns_{0};
};

}  // namespace fw03::test
