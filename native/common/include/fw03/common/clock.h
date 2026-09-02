#pragma once

#include <chrono>

namespace fw03::common {

class Clock {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    virtual ~Clock() = default;
    [[nodiscard]] virtual TimePoint Now() const noexcept = 0;
};

class SteadyClock final : public Clock {
public:
    [[nodiscard]] TimePoint Now() const noexcept override {
        return std::chrono::steady_clock::now();
    }
};

}  // namespace fw03::common
