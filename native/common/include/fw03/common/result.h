#pragma once

#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

namespace fw03::common {

template <typename T, typename E>
class Result final {
public:
    static Result Success(T value) { return Result(std::move(value)); }
    static Result Failure(E error) { return Result(std::move(error), FailureTag{}); }

    [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(storage_); }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const T& value() const {
        if (!ok()) {
            throw std::logic_error("Result does not contain a value");
        }
        return std::get<T>(storage_);
    }

    [[nodiscard]] T& value() {
        if (!ok()) {
            throw std::logic_error("Result does not contain a value");
        }
        return std::get<T>(storage_);
    }

    [[nodiscard]] T take_value() {
        if (!ok()) {
            throw std::logic_error("Result does not contain a value");
        }
        return std::move(std::get<T>(storage_));
    }

    [[nodiscard]] const E& error() const {
        if (ok()) {
            throw std::logic_error("Result does not contain an error");
        }
        return std::get<E>(storage_);
    }

private:
    struct FailureTag final {};

    explicit Result(T value) : storage_(std::move(value)) {}
    Result(E error, FailureTag) : storage_(std::move(error)) {}

    std::variant<T, E> storage_;
};

template <typename E>
class Result<void, E> final {
public:
    static Result Success() { return Result(); }
    static Result Failure(E error) { return Result(std::move(error)); }

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const E& error() const {
        if (ok()) {
            throw std::logic_error("Result does not contain an error");
        }
        return *error_;
    }

private:
    Result() = default;
    explicit Result(E error) : error_(std::move(error)) {}

    std::optional<E> error_;
};

}  // namespace fw03::common
